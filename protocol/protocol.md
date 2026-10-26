<p align="right">
  <a href="protocol.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Dashboard Status Protocol

Source of truth for the compact status string that the host pushes to the AI
Passport over BLE. Host-side reference implementation: `../host/protocol.py`.

## Purpose

The dev-forge host periodically collects node health and agent activity, encodes
them into one compact ASCII status string, and writes it to the badge over BLE.
The badge decodes the string and renders three pages: host health, agent status,
and network. The badge also derives the "last heartbeat age" that satisfies the
node-monitor requirement: it starts counting when a status arrives and keeps
counting while none does, so a frozen or rebooting host becomes visible.

The badge has no wall clock (no RTC battery, and networking is out of scope), so
it MUST measure that age with its own monotonic clock as "time since the last
successfully received status", never by comparing the badge clock against `t`.
The `t` field stays in the protocol as the host's own timestamp: it detects host
clock jumps, drives host-side diagnostics, and is what a future clock-synced
badge would use directly.

## Wire format

```text
v=1;t=1782530000;h=dev-forge;up=86400;ld=0.42;cpu=7;mem=38;dsk=33;tmp=51;net=eno1,up,10.2.65.61;ag=2;as=5
```

Rules:

- Plain ASCII UTF-8 text; no trailing `;`.
- Ordered `key=value` pairs joined by `;`. Keys are fixed, lowercase, and
  transmitted in the field order listed below.
- `v` and `t` are required. Every other field is optional and MUST be omitted
  (not sent empty) when the producer cannot measure it; the badge renders
  "no data" for missing fields.
- Total encoded length MUST NOT exceed **200 bytes** so the payload fits in one
  BLE ATT write after MTU negotiation. The badge MUST negotiate an ATT MTU of at
  least 200 payload bytes (the ESP32-C3 stack supports MTU exchange up to 517).
- Value charset is `[A-Za-z0-9._/-~]` only. A trailing `~` marks a value the
producer shortened to fit its limit, so a cut label reads as truncated rather than
misspelled. `net` additionally uses `,` as its
  internal separator. No `;`, `=`, space, or newline in any value. Producers
  sanitize: illegal characters are dropped; an empty result becomes `-`.

## Fields

| Key | Type | Meaning | Example |
| --- | --- | --- | --- |
| `v` | int | Protocol version, currently `1`. Must be first. | `v=1` |
| `t` | int | Host wall-clock time at collection, Unix seconds. The heartbeat. | `t=1782530000` |
| `h` | str | Hostname (sanitized). | `h=dev-forge` |
| `nc` | int | Online logical CPU cores, 1–1024. Required to make `ld` meaningful. | `nc=4` |
| `up` | int | Uptime in seconds. | `up=86400` |
| `ld` | str | 1-minute load average, fixed two decimals. Judge it against `nc`. | `ld=0.42` |
| `cpu` | int | CPU busy percent, 0–100. | `cpu=7` |
| `mem` | int | Memory used percent, 0–100. | `mem=38` |
| `dsk` | int | Root filesystem used percent, 0–100. | `dsk=33` |
| `tmp` | int | CPU package temperature in °C, −40 to 125. | `tmp=51` |
| `net` | str | `iface,state,ip`; `state` is `up` or `down`, `ip` is IPv4 or `-`. | `net=eno1,up,10.2.65.61` |
| `ag` | int | Active agent sessions: sessions with activity in the last 300 s. | `ag=2` |
| `as` | int | Seconds since the most recent agent activity; `-1` if never seen. | `as=5` |
| `ar` | int | Agents currently running, 0–99. `0` means every agent is idle. | `ar=1` |
| `ws` | str | Workspace of the most recently started running agent, ASCII, ≤ 20 chars. | `ws=feature-dashboard` |
| `pv` | str | Provider id of that agent, ≤ 8 chars; selects the logo. | `pv=dsh` |

The badge maps fields to pages:

- Host health page: `up`, `ld`, `cpu`, `mem`, `dsk`, `tmp`.
- Network page: `net`.
- Agent page: `ar`, `ws`, `pv` when something is running; otherwise `as`.

`ag`/`as` remain the fallback signal: they come from session file activity, which
goes quiet during a long tool call. A producer that can query the agent supervisor
should fill `ar`/`ws`/`pv`/`pm` instead, because that reports the agent's own state.
- Heartbeat banner (all pages): age = badge monotonic clock − monotonic time of
  the last successfully received status. `t` is not compared against the badge
  clock.

## Push cadence and link thresholds

The producer publishes one status per **5 s** while the connection is up. The
badge classifies the link from the age defined above:

| Link state | Age | Badge presentation |
| --- | --- | --- |
| none | no status since boot, or since the last disconnect poll | every field shows "no data" |
| live | 0–15 s | normal values, green heartbeat dot |
| stale | 16–120 s | values dimmed with an amber dot; age still shown |
| lost | > 120 s | values replaced by "no data", red dot, age still shown |

A BLE disconnect clears the measured values immediately but retains the last
heartbeat timestamp, so the age keeps growing across a host reboot.

## Decoding contract

A conforming decoder MUST:

1. Split on `;`; each part MUST contain exactly one `=`.
2. Reject unknown or duplicated keys; accept only the keys in the table.
   Because of this rule, adding a key requires updating the decoder and the
   producer in the same commit. There is no "decoders ignore unknown keys"
   fallback: rejecting is what keeps a version skew visible instead of silent.
3. Validate each value against the per-key charset and type above.
4. Require `v` and `t`; treat any other missing field as "no data", not an error.
5. Never treat an absent/optional field as a fatal parse error.

Both reference implementations are checked against the same shared fixture file,
[`fixtures/status_cases.txt`](fixtures/status_cases.txt), so the host encoder and
the firmware decoder cannot drift apart unnoticed.

## Versioning- `v=1` is frozen as the baseline. Because decoders reject unknown keys, adding a
  key is a coordinated change shipped to both sides in one commit; it is not
  silently backward compatible.
- A breaking change (renamed/removed keys, changed wire framing, or a required
  key added) MUST bump `v`. The badge rejects a payload whose `v` is greater than
  what it implements.

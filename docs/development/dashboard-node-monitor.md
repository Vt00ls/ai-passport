<p align="right">
  <a href="dashboard-node-monitor.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Node Monitor Dashboard (Firmware Design)

Design for using the AI Passport as the physical status panel of a always-on
Linux development node (`dev-forge`). The wire format is defined in
[`protocol/protocol.md`](../../protocol/protocol.md); this document covers the
firmware side: code layout, BLE service, RAM budget, screens, and failure states.

## Scope

In scope for v1:

- BLE peripheral that accepts one status write per 5 s from a single central.
- Three English-language screens (host health, agents, network) plus link state.
- "Last heartbeat age" that keeps counting when the host freezes or reboots.

Out of scope for v1 (unchanged from the task contract): Wi-Fi, host-directed
commands from the badge, OTA, multiple hosts, history graphs.

## Code layout

```text
main/app_dashboard/
  dashboard_protocol.h/.c   pure C: decode + heartbeat age + link state (host-tested)
  dashboard_ble.h/.c        NimBLE peripheral, GATT service, write callback
  dashboard_state.h/.c      shared snapshot: last status, monotonic receive stamp
  dashboard_ui.h/.c         three screens, page switching, refresh timer
  dashboard_app.h/.c        app lifecycle: init BLE + UI, key routing, teardown
```

Only `dashboard_protocol.c` is logic-bearing in a way that must never regress
silently; it has no ESP-IDF or LVGL dependency and is covered by
`tests/test_dashboard_protocol.c`. Everything else needs the device.

## Boot path

`app_main()` boots the dashboard, not the baseline test menu. Derivative
applications must not ship the upstream demo shell as their UI
(`AGENTS.md`), and the boot path is the proof of that.

The baseline `demo_*.c` files stay in the repository as the upstream hardware
reference. A single compile-time switch in `main/main.c`:

```c
#define APP_BOOT_DEMO_MENU 0   // 1 = boot the upstream demo menu (maintenance only)
```

keeps the reference menu reachable for board bring-up without making it the
product UI. This is a source-level switch on purpose: a runtime key-hold escape
hatch would collide with the existing wake-from-sleep key guard.

## BLE service

The badge is peripheral, the host is central (the company network uses 802.1X
EAP-TLS, which the badge cannot authenticate, so Wi-Fi is not an option).

| Item | Value |
| --- | --- |
| Device name | `PassportDash` |
| Advertising | flags + complete local name (17 bytes) |
| Scan response | 128-bit service UUID (18 bytes) |
| Service UUID | `1f7a0001-6b3c-4a1e-9c2d-5e8f0a9b4c10` |
| Status characteristic | `1f7a0002-6b3c-4a1e-9c2d-5e8f0a9b4c10`, `WRITE` + `READ` |
| Payload | The status string, ≤ 200 bytes, one ATT write |
| Connections | 1 (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`) |

Name and UUID are split between advertising and scan response because both
together exceed the 31-byte legacy advertising payload.

`READ` returns the last accepted payload. It exists for debugging with
`bluetoothctl`/`gatttool` and adds no state beyond what the write path already
keeps.

### MTU and framing

The payload needs 200 bytes in a single write, so the ATT MTU must be at least
203 bytes after negotiation. NimBLE's default preferred MTU is 256, so this
holds without configuration changes; the firmware logs the negotiated MTU on
connect so the assumption is verified on the device rather than assumed.

No chunked-write reassembly in v1. If a central writes a partial frame the
parser rejects it, the badge keeps showing the previous values with a growing
heartbeat age, and the next complete write recovers. This is deliberate: silent
concatenation of partial writes would corrupt a frame that looks valid.

### Lifecycle

The write callback runs on the NimBLE host task and must not touch LVGL. It
copies the payload into the shared snapshot under a mutex and returns; the LVGL
timer picks it up on its next tick (200 ms). This follows the repository rule
that callbacks never block and never touch LVGL directly.

On disconnect the snapshot keeps the last heartbeat timestamp but marks all
measured values absent, so the age keeps growing across a host reboot
(acceptance criterion A4) while values read "no data" (A3).

## RAM budget

The target has no PSRAM and a largest free block of about 7.7 KB, so the design
is fixed-size and allocation-free after init.

| Consumer | Approximate cost | Note |
| --- | --- | --- |
| LVGL draw buffer | 19.2 KB (fixed) | `BSP_LVGL_DRAW_BUFFER_LINES` 40 × 240 px × 2 B, BSP-owned |
| LVGL heap pool | 24 KB (fixed) | `CONFIG_LV_MEM_SIZE_KILOBYTES` |
| Status snapshot | ~90 B | one `dashboard_status_t`, reused |
| BLE write buffer | ≤ 200 B | one `dashboard_status_t` copy path |
| Three screens | ~2–3 KB LVGL objects | labels only, no images |
| NimBLE host stack | task stack, existing | unchanged from the BLE demo |

Rules for implementation:

- No `malloc` in application code; all state is statically allocated.
- No images or new fonts. Only `lv_font_montserrat_14` and
  `lv_font_montserrat_20` are enabled (`sdkconfig.defaults`).
- Screens are built once and updated in place; no create/delete per refresh.
- On-device verification: log `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`
  and total free heap at boot and after the first full render. That number, not
  this table, decides whether the design fits.

## Screens

240×320 portrait, inverted color, 30 px corner mask. Keep content inside a
12 px inset and out of the corners so the mask never clips text.

Common chrome on every screen:

- Top row: link dot (green/amber/red/grey) + heartbeat age (`12s`, `3m 20s`,
  `NEVER`) + battery percent from `bsp_battery_soc()` (hidden when `-1`).
- Bottom row: three page dots, current page filled.

Page 1 — HOST (`up`, `ld`, `cpu`, `mem`, `dsk`, `tmp`):

```text
 dev-forge            87%
 UPTIME
 1d 11h 34m
 LOAD        CPU
 0.42        7%
 MEM  [####----] 38%
 DISK [###-----] 33%
 TEMP   51 C
```

Page 2 — AGENTS (`ag`, `as`):

```text
 AGENTS
   2
 ACTIVE SESSIONS
 last activity
 5s ago
```

Page 3 — NETWORK (`net`):

```text
 NETWORK
 eno1
 ONLINE / OFFLINE
 10.2.65.61
```

Every value that is absent from the last payload renders `--`, never `0`, so a
missing field cannot be mistaken for a real measurement.

Link states (from `protocol/protocol.md`): `none` shows `NEVER` and `--` for all
values; `live` draws normally; `stale` dims values and shows an amber dot;
`lost` replaces values with `--`, draws a red dot, and keeps the age line.

## Key interaction

Three buttons share GPIO0 through a resistor divider; callbacks stay
non-blocking and enqueue to the existing input task.

| Input | Action |
| --- | --- |
| UP click | previous page |
| DOWN click | next page |
| OK click | force redraw of the current page |
| OK long press (1 s) | toggle backlight between 100 % and 20 % |

Deep sleep is not used in v1: a desk panel that sleeps is not a status panel.
The wake-from-sleep key guard in `main/key_guard.h` stays untouched because the
switch preserves the existing boot path.

## Failure states

| Condition | Behaviour |
| --- | --- |
| Display/LVGL init fails | log pins and return, as `app_main` does today |
| NimBLE init fails | UI still renders, link dot grey, `BLE FAILED` line |
| Never receives a status | `NEVER`, all values `--`, grey dot |
| Malformed payload | frame rejected, previous values retained, age keeps growing |
| Unsupported protocol version | frame rejected; link dot red to flag a version skew |
| Battery gauge absent | battery percent hidden, no placeholder |
| Button init fails | UI renders and refreshes; only page switching is lost |

## Verification

Host (runs anywhere):

```bash
./tools/validate.sh --static
```

This covers `check_repo.py`, the existing BSP/demo host tests, and the new
`tests/test_dashboard_protocol.c` plus the Python producer tests, all replayed
against `protocol/fixtures/status_cases.txt`.

Device (needs the badge, ESP-IDF 5.5.3, and an explicit flashing approval):

1. `./tools/validate.sh --firmware` builds and verifies the merged image.
2. Boot: the dashboard appears; the demo menu must not.
3. Host pushes for 60 s: values track the host; age stays ≤ 5 s.
4. Stop the host for 3 minutes: values go `--`, the dot turns red, the age grows.
5. Restart the host: the link recovers and the age resets.
6. Power-cycle the badge: it re-advertises and the host reconnects.
7. Log the free-heap numbers from the RAM budget section.

## Bring-up checklist (first flash)

Prerequisites: ESP-IDF 5.5.3 activated, the badge connected with a data-capable USB
cable, and explicit approval to flash.

```bash
source ~/esp/esp-idf/export.sh
./tools/validate.sh --firmware
idf.py -p /dev/cu.usbmodem* flash monitor
```

`GPIO0` is the boot strapping pin and is shared by the three buttons. Do not hold
UP, DOWN, or OK while the board resets or flashes: a low level during reset can put
the chip into download mode instead of running the application.

Read the boot log for these four things before looking at the screen:

1. No panic or reboot loop.
2. `dash_ble: advertising as PassportDash`.
3. On connect: `dash_ble: connected; ATT MTU <n> (need >= 203)`. A warning here means
   the frame needs a different transport, not another retry.
4. `dash_app: [boot]` and `dash_app: [ui started]` free-heap and largest-block
   numbers. Compare them against the RAM budget table; the measured values win.

Then confirm the product boot path: the dashboard must appear, and the baseline
demo menu must not.

```bash
~/.venvs/dashboard/bin/python host/ble_central.py --verbose
```

Finally, the two behavioural checks:

- **A3:** stop the pusher (`Ctrl-C`) and watch the values turn into `--` while the
  age keeps counting.
- **A4:** leave the host idle for a few minutes, then power-cycle the badge: it must
  re-advertise, the pusher must reconnect on its own, and the age must reset.

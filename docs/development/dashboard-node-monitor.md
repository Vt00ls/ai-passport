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
| Three screens | ~4 KB LVGL objects | labels, four bars, one chart; no images |
| Montserrat 36 + 48 | 0 B RAM, +151 KB flash | glyphs render straight from flash |
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

The panel exists because a headless machine that freezes gives no other signal. It
is therefore **status first**: page one answers "is the box OK?" before showing any
number, and the panel only becomes loud when something is wrong.

240×320 portrait, inverted colour at the panel level (so LVGL colours are the
colours on screen), 30 px corner mask, 14 px inset.

Typography: Montserrat 14 (captions), 20 (values), 36 (status word), 48 (the one
big number). The 36/48 faces cost +56 KB and +95 KB of flash and no RAM, because
glyphs render straight out of flash.

### Page 1 — STATUS

```text
 Ubuntu-Box-Dev    87%
 +----------------------+
 |         OK           |   status word, 36 px, state colour
 |       UPTIME         |   what the number means, 14 px
 |       1d 11h         |   36 px
 +----------------------+
 |||||||||||||||||||||      heartbeat history, 60 slots x 5 s
 LAST 5 MIN
 LOAD 10%      MEM 38%
 [-]           [###--]
 DISK 33%      TEMP 51%
 [##-]          [##--]
 eno1   2 agents
```

The big number carries a caption, because its meaning depends on the state:
`UPTIME` while the host answers, `NO FRAMES FOR` when frames stop, `DOWN FOR` when
the link itself drops, and `LAST PUSH` before anything has arrived. Uptime is
monotonic, so a healthy panel no longer shows a counter that resets every push
interval, which read like a fault.

`DOWN FOR` deliberately counts from the disconnect, not from the last frame: a
pusher that stops while the link stays up makes those two times differ by minutes,
and quoting the frame age there was misleading.

The heartbeat strip is one bar per 5 s slot. A received frame fills its slot; a
frame that never arrived leaves a gap. A frozen host therefore leaves a visible
run of empty slots, which is the same failure that motivated the project.

Page 2 — DETAIL: uptime, load with the core count (`0.42 / 4`), CPU, memory, disk,
temperature, and the network line (`eno1 up 10.2.65.61`).

Page 3 — AGENTS: active session count and time since the last agent activity.

Every value that is absent from the last payload renders `--`, never `0`, so a
missing field cannot be mistaken for a real measurement.

### States

The badge reports what it observes, never a cause it cannot prove. It cannot
distinguish "host dead" from "pusher crashed", so it never claims the former.

| State | Card | Meaning | What to do |
| --- | --- | --- | --- |
| `OK` | dark, green border | everything within thresholds | nothing |
| `WARN` | dark, amber border | a threshold is close | watch |
| `<ITEM>` | red fill, dark text | a metric breached: `DISK 96%`, `MEMORY 93%`, `LOAD 8.40`, `TEMP 88C`, `NETWORK DOWN` | act now |
| `SILENT` | dark, red border | link is up, frames stopped: the host is the likely cause | walk over |
| `NO LINK` | dark, border by duration | BLE connection down; ambiguous (badge, pusher or host) | check Bluetooth and the pusher |
| `NO DATA` | dark, grey border | nothing received since boot | wait, or start the pusher |

`SILENT` is the strong signal: the transport works, so the host is what stopped.
`NO LINK` is weak: many causes collapse into it, which is why it does not go red
immediately.

### Alerting

CRIT must take over the screen, and must not cry wolf.

| Rule | Value | Why |
| --- | --- | --- |
| CRIT dwell | 10 s | a single spike must not flash red |
| Recovery debounce | 5 s below threshold | a value hovering on a threshold must not flap |
| `NO LINK` escalation | grey 0–20 s, amber 20 s–2 min, red after | a reconnect blip is common; two minutes is not a blip |
| `SILENT` threshold | 60 s without a frame | already long, so no extra dwell |
| Acknowledgement | OK click | stops the pulse and the repeats, **keeps the colour** until the condition clears |
| Repeat reminder | every 30 s while unacknowledged | only matters when nobody is looking |
| Backlight | 3 pulses of 400 ms on/off, then hold | audible alarms get disabled in an office within a week, so v1 is silent |

A new kind of problem re-arms the acknowledgement, so fixing one alert does not
silence the next.

## Thresholds

| Item | WARN | CRIT |
| --- | --- | --- |
| Heartbeat age | > 15 s | > 60 s |
| Disk used | ≥ 80 % | ≥ 90 % |
| Memory used | ≥ 80 % | ≥ 90 % |
| Load / cores | > 1.0 | > 2.0 |
| Temperature | ≥ 75 °C | ≥ 85 °C |
| Network | — | interface down or no address |

Load is judged against `nc`, the core count the producer sends, because load 1.0 is
saturated on two cores and idle on sixteen. Without `nc` the load is not judged at
all rather than guessed.

## Key interaction

Three buttons share GPIO0 through a resistor divider; callbacks stay non-blocking
and enqueue to the existing input task.

| Input | Action |
| --- | --- |
| UP click | previous page |
| DOWN click | next page |
| OK click | acknowledge the current alert |
| OK long press (1 s) | toggle backlight between 100 % and 20 % |

Deep sleep is not used in v1: a desk panel that sleeps is not a status panel. The
wake-from-sleep key guard in `main/key_guard.h` stays untouched.

## Failure states

| Condition | Behaviour |
| --- | --- |
| Display/LVGL init fails | log pins and return, as `app_main` does today |
| NimBLE init fails | UI still renders; card shows `NO LINK` once the grace period passes |
| Never receives a status | `NO DATA` + `NEVER`, all values `--` |
| Malformed payload | frame rejected, previous values retained, age keeps growing |
| Unsupported version or unknown key | frame rejected; the panel goes `SILENT`, which exposes the version skew |
| A metric field is missing | that indicator reads `--` and never raises an alert |
| Battery gauge absent | battery percent hidden, no placeholder |
| Button init fails | UI renders and refreshes; alerts still escalate, but cannot be acknowledged |

## Verification

Host (runs anywhere):

```bash
./tools/validate.sh --static
```

This covers `check_repo.py`, the existing BSP/demo host tests,
`tests/test_dashboard_format.c`, `tests/test_dashboard_protocol.c` (replayed
against `protocol/fixtures/status_cases.txt`), `tests/test_dashboard_health.c`
(thresholds, dwell, escalation ladder, acknowledgement, reboot detection), and the
Python producer tests.

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

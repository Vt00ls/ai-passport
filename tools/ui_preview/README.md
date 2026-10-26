<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Dashboard UI Preview

Renders the dashboard's real screens off-device, so layout and state changes can be
reviewed without flashing a badge.

```bash
./tools/ui_preview/build.sh
```

Images land in `tools/ui_preview/build/`, one BMP per state.

## Why this is not a mock-up

The harness compiles the firmware's own sources:

```text
main/app_dashboard/dashboard_ui.c        pages, card, indicators, strip
main/app_dashboard/dashboard_health.c    thresholds, dwell, escalation, hysteresis
main/app_dashboard/dashboard_format.c    text formatting
main/app_dashboard/dashboard_protocol.c  frame decoding
```

Only the device side is stubbed: the shared snapshot, `bsp_battery_soc()`,
`bsp_display_backlight()` and `bsp_lvgl_lock()`. Nothing about the layout is
re-implemented here, so an image that looks wrong means the firmware is wrong.

Time is advanced one second at a time while the UI's own 200 ms timer is pumped, so
dwell windows, escalation ladders and hysteresis behave as they do on the device.

## What it renders

| Image | State |
| --- | --- |
| `live-1-status-ok` | Healthy host, `OK` card with uptime |
| `live-2-detail` | DETAIL page |
| `live-3-agents` | AGENTS page |
| `live-4-status-crit-disk` | Disk breach, red takeover card |
| `live-4b-status-reboot` | Host reboot notice on the status line |
| `live-5-status-silent` | Link up, frames stopped |
| `live-6-status-nolink-warn` | Link down, amber stage |
| `live-7-status-nolink-crit` | Link down past two minutes |
| `live-8b-battery-warn` | Badge battery at 18 %, amber hint |
| `live-8-battery-crit` | Badge battery at 8 %, `LOW BATT` takeover |
| `live-9-battery-charging` | Charging suppresses the battery alert |

## Requirements

- A host C toolchain (the same `cc` the static gate uses)
- The LVGL sources under `managed_components/`, which the firmware build fetches. Run
  `./tools/validate.sh --firmware` once if that directory is absent.

This is deliberately not part of `./tools/validate.sh`: CI does not carry
`managed_components`, and the harness produces images for humans rather than a
pass/fail verdict. Behaviour that must not regress is covered by `tests/` instead.

## Limits

- The panel's 30 px corner mask is **not** simulated: BSP rounds the corners to black
  on the device and these images keep square corners.
- Panel inversion, colour response and the real SPI refresh rate are not modelled.
- BMP is written directly so the harness needs no image library. macOS Preview opens
  them; `sips -s format png <file>.bmp --out <file>.png` converts if needed.

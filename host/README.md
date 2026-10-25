<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Host Side: Collector and BLE Pusher

Everything in this directory runs on the **host** (the Linux node being monitored),
not on the badge. The badge is a BLE peripheral; this side is the central that
connects to it and writes status frames.

| File | Purpose |
| --- | --- |
| `collector.py` | Reads node health and agent activity, prints one status frame |
| `protocol.py` | Reference encoder/decoder for the frame format |
| `ble_central.py` | Connects to the badge and writes frames on an interval |
| `dashboard.conf` | Connection settings and intervals (no secrets, no addresses) |
| `systemd/ai-passport-dashboard.service` | Runs `ble_central.py` as a service |
| `http_debug.py` | Serves the frame over HTTP for `curl` debugging |

The frame format is defined in [`../protocol/protocol.md`](../protocol/protocol.md).
Nothing here hardcodes a network address; the interface and address are read from
the kernel at runtime.

## Requirements

- Linux with BlueZ 5.43 or newer and a working Bluetooth adapter
- Python 3.9 or newer
- `bleak` for the BLE path only (`collector.py` and `http_debug.py` use the
  standard library alone)

## Install

```bash
sudo apt-get install -y bluez python3-venv
sudo usermod -aG bluetooth "$USER"     # log out and back in for this to apply
python3 -m venv ~/.venvs/dashboard
~/.venvs/dashboard/bin/pip install bleak
```

Verify the adapter is up:

```bash
bluetoothctl --version
systemctl is-active bluetooth
bluetoothctl list
```

## Verify before a badge exists

`--dry-run` needs neither Bluetooth nor `bleak`. Use it to confirm the collector
produces valid frames and to watch the values move:

```bash
python3 ble_central.py --dry-run --frames 5 --interval 1
```

```text
[1] 114 B  v=1;t=1792919687;h=Ubuntu-Box-Dev;up=151346;ld=0.07;cpu=0;mem=8;dsk=5;tmp=40;net=eno1,up,10.2.65.61;ag=0;as=150955
```

Each frame is decoded before it is printed, so a malformed frame fails here rather
than silently at the badge.

## Run against the badge

```bash
~/.venvs/dashboard/bin/python ble_central.py --verbose
```

The pusher scans for the badge's service UUID (falling back to its advertised
name), connects, logs the negotiated ATT MTU, and writes one frame per
`interval_s`. A dropped link reconnects with exponential backoff up to
`max_retry_delay_s`; a badge that is switched off is a normal condition and does
not exit the process.

## Install as a service

```bash
sudo install -m 0644 host/systemd/ai-passport-dashboard.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ai-passport-dashboard
journalctl -u ai-passport-dashboard -f
```

Edit the three paths in the unit (`WorkingDirectory`, `ExecStart`) if the checkout
or the venv lives somewhere other than `/home/vt/ai-passport` and
`/home/vt/.venvs/dashboard`. Check the unit before installing it:

```bash
systemd-analyze verify host/systemd/ai-passport-dashboard.service
```

## Configuration

`dashboard.conf` holds every tunable. Command-line flags override it for one-off
runs. `sessions_dir` is empty by default, meaning the collector uses
`~/.pi/agent/sessions`; set it explicitly if the service user's home differs from
the account running the agents.

`interval_s` must stay well below 15 s: the badge marks the link stale after 15 s
and lost after 120 s.

## HTTP debugging

```bash
python3 http_debug.py --host 127.0.0.1 --port 8000
curl -s http://127.0.0.1:8000/status        # raw frame
curl -s http://127.0.0.1:8000/status.json   # frame plus decoded fields
```

The debug server has no authentication. Bind it to `127.0.0.1` unless you
deliberately need LAN access, and stop it afterwards.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `badge not found in scan` | Badge powered? Advertised name/UUID match `dashboard.conf`? |
| `MTU 23 is below 203` | The badge rejected the larger MTU request; the frame will not fit in one write |
| `write failed` repeatedly | BlueZ permissions: is the service user in the `bluetooth` group? |
| Frame builds but values are `--` | No agent sessions under `sessions_dir`; that is reported, not an error |
| `the 'bleak' package is required` | Install `bleak` into the venv used by `ExecStart` |

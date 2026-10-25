#!/usr/bin/env python3
"""Collect dev-forge node health + agent activity and print the status string.

Python 3 standard library only. Intended to run on the dev-forge host (Linux).
Every read is best-effort: a source that cannot be measured is omitted from the
output so the badge renders "no data" instead of a stale or garbage value.

The collector's own send time (``t``) is the heartbeat: while it runs on the
same host it freezes together with the node, so the badge's "last heartbeat
age" (badge clock - ``t``) grows whenever the host stalls. The node monitor's
``~/health.log`` (a record every 10 s) is an available alternative heartbeat
source; v1 does not depend on it.

No network address is hardcoded: the primary interface and its IPv4 address are
read at runtime from ``/proc/net/route`` and ``ioctl(SIOCGIFADDR)``. Environment
specific paths (agent sessions, procfs) are defaults overridable via CLI flags.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import socket
import struct
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from protocol import MAX_BYTES, PROTOCOL_VERSION, encode  # noqa: E402


@dataclass
class Sources:
    """Injectable filesystem locations and sampling parameters."""

    now: float | None = None
    proc_uptime: str = "/proc/uptime"
    proc_loadavg: str = "/proc/loadavg"
    proc_meminfo: str = "/proc/meminfo"
    proc_stat: str = "/proc/stat"
    proc_route: str = "/proc/net/route"
    sys_net: str = "/sys/class/net"
    thermal_glob: str = "/sys/class/thermal/thermal_zone*/temp"
    sessions_dir: str = os.path.join(os.path.expanduser("~"), ".pi", "agent", "sessions")
    active_window: float = 300.0
    cpu_interval: float = 0.2
    statvfs_path: str = "/"


# ---------------------------------------------------------------------------
# Small, dependency-free readers. Each returns None on failure.
# ---------------------------------------------------------------------------


def _first_number(path: str) -> float | None:
    try:
        return float(Path(path).read_text(encoding="utf-8", errors="replace").split()[0])
    except (OSError, ValueError, IndexError):
        return None


def read_cores() -> int | None:
    """Online logical CPU cores.

    Load average is only meaningful relative to this number, so the badge cannot
    judge LOAD without it. Uses the CPUs this process may run on when the kernel
    reports an affinity mask, which is the set the load average reflects for us.
    """
    try:
        affinity = os.sched_getaffinity(0)
        if affinity:
            return len(affinity)
    except (AttributeError, OSError):
        pass
    count = os.cpu_count()
    return count if count else None


def read_uptime(proc_uptime: str) -> int | None:
    value = _first_number(proc_uptime)
    return None if value is None else int(value)


def read_load1(proc_loadavg: str) -> str | None:
    value = _first_number(proc_loadavg)
    return None if value is None else f"{value:.2f}"


def read_mem_percent(proc_meminfo: str) -> int | None:
    try:
        info: dict[str, int] = {}
        for line in Path(proc_meminfo).read_text(encoding="utf-8", errors="replace").splitlines():
            key, _, rest = line.partition(":")
            if key and rest.split():
                info[key.strip()] = int(rest.split()[0])
        total = info["MemTotal"]
        available = info.get("MemAvailable")
        if available is None:
            available = info.get("MemFree", 0) + info.get("Buffers", 0) + info.get("Cached", 0)
        if total <= 0:
            return None
        return int(round(100.0 * (total - available) / total))
    except (OSError, ValueError, KeyError):
        return None


def read_disk_percent(path: str) -> int | None:
    try:
        stat = os.statvfs(path)
        total = stat.f_blocks
        if total <= 0:
            return None
        used = total - stat.f_bfree
        return int(round(100.0 * used / total))
    except (OSError, ValueError):
        return None


def _cpu_times(proc_stat: str) -> list[int] | None:
    try:
        for line in Path(proc_stat).read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("cpu "):
                return [int(token) for token in line.split()[1:]]
    except (OSError, ValueError):
        return None
    return None


def read_cpu_percent(proc_stat: str, interval: float = 0.2) -> int | None:
    first = _cpu_times(proc_stat)
    if first is None or len(first) < 5:
        return None
    if interval > 0:
        time.sleep(interval)
    second = _cpu_times(proc_stat)
    if second is None or len(second) < 5 or len(second) != len(first):
        return None
    delta_total = sum(second) - sum(first)
    delta_idle = (second[3] + second[4]) - (first[3] + first[4])
    if delta_total <= 0:
        return None
    return int(round(100.0 * (delta_total - delta_idle) / delta_total))


def read_temperature(thermal_glob: str, thermal_type_glob: str = "") -> int | None:
    try:
        import glob

        preferred: int | None = None
        best: int | None = None
        for zone_path in sorted(glob.glob(thermal_glob)):
            type_path = str(zone_path).rsplit("/", 1)[0] + "/type"
            try:
                kind = Path(type_path).read_text(encoding="utf-8", errors="replace").strip()
                millideg = int(Path(zone_path).read_text(encoding="utf-8", errors="replace").strip())
            except (OSError, ValueError):
                continue
            celsius = millideg // 1000
            if kind == "x86_pkg_temp":
                preferred = celsius
            if best is None or celsius > best:
                best = celsius
        return preferred if preferred is not None else best
    except Exception:
        return None


def _default_interface(proc_route: str, sys_net: str) -> str | None:
    entries: list[tuple[int, str]] = []
    try:
        lines = Path(proc_route).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    for line in lines[1:]:
        cols = line.split()
        if len(cols) < 11:
            continue
        iface, destination, _, _, _, _, metric = cols[0], cols[1], cols[2], cols[3], cols[4], cols[5], cols[6]
        if iface == "lo":
            continue
        try:
            metric_num = int(metric)
        except ValueError:
            continue
        if destination == "00000000":
            entries.append((metric_num, iface))
    if entries:
        entries.sort(key=lambda item: item[0])
        return entries[0][1]
    # Fall back to the first non-loopback interface present.
    try:
        for name in sorted(os.listdir(sys_net)):
            if name != "lo" and os.path.exists(os.path.join(sys_net, name)):
                return name
    except OSError:
        return None
    return None


def _iface_state(sys_net: str, iface: str) -> str:
    operstate = os.path.join(sys_net, iface, "operstate")
    try:
        state = Path(operstate).read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        state = ""
    if state == "up":
        return "up"
    if state in ("down", "dormant", "lowerlayerdown", "unknown"):
        return "down"
    # Fall back to the carrier flag.
    carrier = os.path.join(sys_net, iface, "carrier")
    try:
        return "up" if Path(carrier).read_text(encoding="utf-8", errors="replace").strip() == "1" else "down"
    except OSError:
        return "down"


def _iface_ipv4(iface: str) -> str:
    # SIOCGIFADDR (0x8915); struct ifreq packs a 16-byte name and sockaddr_in.
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            request = struct.pack("256s", iface.encode("utf-8")[:15])
            response = fcntl.ioctl(sock.fileno(), 0x8915, request)
            return socket.inet_ntoa(response[20:24])
        finally:
            sock.close()
    except (OSError, struct.error):
        return "-"


def read_network(proc_route: str, sys_net: str) -> str | None:
    iface = _default_interface(proc_route, sys_net)
    if not iface:
        return None
    return f"{iface},{_iface_state(sys_net, iface)},{_iface_ipv4(iface)}"


# ---------------------------------------------------------------------------
# Agent activity: pi session jsonl files (mtime + last record timestamp).
# ---------------------------------------------------------------------------


def _parse_timestamp(value: object) -> float | None:
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, (int, float)):
        ts = float(value)
        if ts > 10_000_000_000:  # milliseconds
            ts /= 1000.0
        return ts
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return None
        try:
            ts = float(text)
            if ts > 10_000_000_000:
                ts /= 1000.0
            return ts
        except ValueError:
            pass
        iso = text[:-1] + "+00:00" if text.endswith("Z") else text
        try:
            return datetime.fromisoformat(iso).timestamp()
        except ValueError:
            return None
    return None


def _tail_lines(path: Path, max_bytes: int = 8192) -> list[str]:
    try:
        with open(path, "rb") as handle:
            handle.seek(0, os.SEEK_END)
            size = handle.tell()
            if size == 0:
                return []
            handle.seek(max(0, size - max_bytes))
            data = handle.read().decode("utf-8", errors="replace")
    except OSError:
        return []
    return [line for line in data.splitlines() if line.strip()]


def _last_activity_time(path: Path) -> float | None:
    """Most recent activity for one jsonl: max(mtime, newest record timestamp)."""
    try:
        mtime = path.stat().st_mtime
    except OSError:
        return None
    latest = mtime
    for line in reversed(_tail_lines(path)):
        try:
            record = json.loads(line)
        except (json.JSONDecodeError, TypeError):
            continue
        if not isinstance(record, dict):
            continue
        for key in ("timestamp", "ts", "time", "created_at", "createdAt"):
            stamp = _parse_timestamp(record.get(key))
            if stamp is not None:
                latest = max(latest, stamp)
                break
        else:
            message = record.get("message")
            if isinstance(message, dict):
                stamp = _parse_timestamp(message.get("timestamp"))
                if stamp is not None:
                    latest = max(latest, stamp)
    return latest


def read_agent_state(sessions_dir: str, now: float, active_window: float = 300.0) -> tuple[int, int]:
    """Return ``(active_count, seconds_since_last_activity)``.

    ``seconds_since_last_activity`` is ``-1`` when no session has any known
    activity. Only files touched recently are tail-read for the precise record
    timestamp; older files use their mtime.
    """
    root = Path(sessions_dir)
    if not root.is_dir():
        return 0, -1

    lookback = max(active_window * 2.0, 600.0)
    active = 0
    best_last: float | None = None
    try:
        files = list(root.glob("**/*.jsonl"))
    except OSError:
        return 0, -1

    for path in files:
        try:
            mtime = path.stat().st_mtime
        except OSError:
            continue
        effective = mtime
        if now - mtime <= lookback:
            stamp = _last_activity_time(path)
            if stamp is not None and stamp > effective:
                effective = stamp
        if best_last is None or effective > best_last:
            best_last = effective
        if now - effective <= active_window:
            active += 1

    if best_last is None:
        return 0, -1
    return active, max(0, int(now - best_last))


# ---------------------------------------------------------------------------
# Assembly.
# ---------------------------------------------------------------------------


def _fit(fields: dict[str, object]) -> str:
    """Encode and, if needed, drop low-priority optional fields to fit 200 B."""
    payload = encode(fields)
    if len(payload.encode("utf-8")) <= MAX_BYTES:
        return payload
    for drop in ("net", "tmp", "dsk", "cpu", "ld", "mem", "up", "ag", "as"):
        if drop in fields:
            reduced = {k: v for k, v in fields.items() if k != drop}
            payload = encode(reduced)
            if len(payload.encode("utf-8")) <= MAX_BYTES:
                return payload
    while len(payload.encode("utf-8")) > MAX_BYTES and len(str(fields.get("h", ""))) > 1:
        fields["h"] = str(fields["h"])[:-1]
        payload = encode(fields)
    return payload


def collect(sources: Sources | None = None) -> str:
    src = sources or Sources()
    now = src.now if src.now is not None else time.time()

    fields: dict[str, object] = {"v": PROTOCOL_VERSION, "t": int(now)}
    hostname = socket.gethostname()
    if hostname:
        fields["h"] = hostname

    cores = read_cores()
    if cores:
        fields["nc"] = cores

    uptime = read_uptime(src.proc_uptime)
    if uptime is not None:
        fields["up"] = uptime

    load1 = read_load1(src.proc_loadavg)
    if load1 is not None:
        fields["ld"] = load1

    cpu = read_cpu_percent(src.proc_stat, src.cpu_interval)
    if cpu is not None:
        fields["cpu"] = cpu

    mem = read_mem_percent(src.proc_meminfo)
    if mem is not None:
        fields["mem"] = mem

    disk = read_disk_percent(src.statvfs_path)
    if disk is not None:
        fields["dsk"] = disk

    temp = read_temperature(src.thermal_glob)
    if temp is not None:
        fields["tmp"] = temp

    network = read_network(src.proc_route, src.sys_net)
    if network is not None:
        fields["net"] = network

    active, last_age = read_agent_state(src.sessions_dir, now, src.active_window)
    if active or last_age != -1:
        fields["ag"] = active
        fields["as"] = last_age

    return _fit(fields)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sessions-dir", default=None, help="agent session jsonl directory (default: ~/.pi/agent/sessions)")
    parser.add_argument("--active-window", type=float, default=300.0, help="seconds an agent counts as active (default: 300)")
    parser.add_argument("--cpu-interval", type=float, default=0.2, help="seconds between CPU samples (default: 0.2)")
    parser.add_argument("--json", action="store_true", help="emit a JSON object instead of the raw status string")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    sources = Sources(active_window=args.active_window, cpu_interval=args.cpu_interval)
    if args.sessions_dir:
        sources.sessions_dir = args.sessions_dir
    payload = collect(sources)
    if args.json:
        import protocol

        print(json.dumps({"payload": payload, "bytes": len(payload.encode("utf-8"))}))
    else:
        print(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

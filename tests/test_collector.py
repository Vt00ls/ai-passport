#!/usr/bin/env python3
"""Host tests for the dashboard status collector (pure logic and file readers)."""

from __future__ import annotations

import importlib.util
import os
import re
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("host_collector", ROOT / "host" / "collector.py")
assert SPEC and SPEC.loader
collector = importlib.util.module_from_spec(SPEC)
sys.modules["host_collector"] = collector  # dataclasses resolve __module__ via sys.modules
SPEC.loader.exec_module(collector)

# collector imports the protocol module under the top-level name "protocol";
# reuse that already-loaded module.
import protocol as protocol_mod  # noqa: E402


class FileReaderTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="ai-passport-collector-tests-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def _write(self, rel: str, text: str) -> Path:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def test_uptime_and_load1(self) -> None:
        self.assertEqual(collector.read_uptime(str(self._write("uptime", "128099.42 900000.00\n"))), 128099)
        self.assertEqual(collector.read_load1(str(self._write("loadavg", "0.42 0.31 0.28 1/345 1234\n"))), "0.42")
        self.assertIsNone(collector.read_uptime(str(self.root / "missing")))

    def test_mem_percent_uses_memavailable(self) -> None:
        text = "MemTotal:       8000000 kB\nMemFree:        1000000 kB\nMemAvailable:   3000000 kB\nBuffers:        100000 kB\n"
        self.assertEqual(collector.read_mem_percent(str(self._write("meminfo", text))), 62)

    def test_mem_percent_falls_back_to_free_buffers_cached(self) -> None:
        text = "MemTotal:       1000000 kB\nMemFree:         400000 kB\nBuffers:         100000 kB\nCached:          100000 kB\n"
        self.assertEqual(collector.read_mem_percent(str(self._write("meminfo", text))), 40)

    def test_disk_percent_bounds_and_missing(self) -> None:
        self.assertIsNone(collector.read_disk_percent(str(self.root / "no-such-dir")))
        percent = collector.read_disk_percent("/")
        self.assertIsInstance(percent, int)
        self.assertGreaterEqual(percent, 0)
        self.assertLessEqual(percent, 100)

    def test_cpu_percent_two_samples(self) -> None:
        # user nice system idle iowait irq softirq steal
        first = [100, 0, 100, 400, 0, 0, 0, 0]
        second = [110, 0, 110, 410, 0, 0, 0, 0]
        with patch.object(collector, "_cpu_times", side_effect=[first, second]):
            self.assertEqual(collector.read_cpu_percent("/proc/stat", interval=0.0), 67)

    def test_temperature_prefers_x86_pkg_temp(self) -> None:
        self._write("thermal/thermal_zone0/temp", "30000\n")
        self._write("thermal/thermal_zone0/type", "acpitz\n")
        self._write("thermal/thermal_zone1/temp", "40000\n")
        self._write("thermal/thermal_zone1/type", "x86_pkg_temp\n")
        pattern = str(self.root / "thermal" / "thermal_zone*" / "temp")
        self.assertEqual(collector.read_temperature(pattern), 40)

    def test_temperature_missing(self) -> None:
        self.assertIsNone(collector.read_temperature(str(self.root / "no" / "thermal_zone*" / "temp")))

    def test_default_interface_prefers_default_route(self) -> None:
        proc_route = (
            "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
            "lo\t00000000\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n"
            "wlp0s20f3\t00000000\t0140020A\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
            "eno1\t00000000\t0140020A\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
        )
        sys_net = self.root / "net"
        sys_net.mkdir()
        self.assertEqual(collector._default_interface(str(self._write("route", proc_route)), str(sys_net)), "eno1")

    def test_iface_state(self) -> None:
        sys_net = self.root / "net"
        (sys_net / "eno1").mkdir(parents=True)
        (sys_net / "eno1" / "operstate").write_text("up\n", encoding="utf-8")
        self.assertEqual(collector._iface_state(str(sys_net), "eno1"), "up")


class HostnameTest(unittest.TestCase):
    def test_fqdn_is_reduced_to_the_first_label(self) -> None:
        # This is the exact name macOS reported, and the badge rejects it verbatim.
        self.assertEqual(
            collector.short_hostname("VTdeMacBook-Pro.unknown.host.in.chaitin.net"),
            "VTdeMacBook-Pro",
        )

    def test_short_name_is_kept(self) -> None:
        self.assertEqual(collector.short_hostname("Ubuntu-Box-Dev"), "Ubuntu-Box-Dev")

    def test_truncated_workspace_is_marked(self) -> None:
        # Real name from the host: 23 characters against a 20-character limit.
        self.assertEqual(
            collector._sanitize_label("chore-dsh-devboard-mcp", 20), "chore-dsh-devboard-~"
        )

    def test_workspace_that_fits_is_untouched(self) -> None:
        self.assertEqual(collector._sanitize_label("devilish-ladybug", 20), "devilish-ladybug")

    def test_over_long_label_is_capped(self) -> None:
        name = collector.short_hostname("a" * 60)
        self.assertEqual(len(name), protocol_mod.HOST_MAX)
        self.assertTrue(name)


class TimestampAndAgentTest(unittest.TestCase):
    def test_parse_timestamp_variants(self) -> None:
        self.assertAlmostEqual(collector._parse_timestamp(1782530000), 1782530000)
        self.assertAlmostEqual(collector._parse_timestamp(1782530000123), 1782530000.123)
        self.assertAlmostEqual(collector._parse_timestamp("1782530000"), 1782530000)
        iso_z = collector._parse_timestamp("2026-10-25T10:00:00Z")
        iso_offset = collector._parse_timestamp("2026-10-25T10:00:00+00:00")
        self.assertIsNotNone(iso_z)
        self.assertAlmostEqual(iso_z, iso_offset)
        self.assertGreater(iso_z, 1_000_000_000)
        self.assertIsNone(collector._parse_timestamp("not-a-time"))
        self.assertIsNone(collector._parse_timestamp(None))
        self.assertIsNone(collector._parse_timestamp(True))

    def _make_session(self, root: Path, name: str, mtime: float, record_ts: float | None) -> None:
        from datetime import datetime, timezone

        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if record_ts is None:
            path.write_text("", encoding="utf-8")
        else:
            iso = datetime.fromtimestamp(record_ts, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")
            path.write_text(
                '{"type":"message","timestamp":"%s","message":{"timestamp":%d}}\n'
                % (iso, int(record_ts * 1000)),
                encoding="utf-8",
            )
        os.utime(path, (mtime, mtime))

    def test_agent_state_active_count_and_age(self) -> None:
        now = 1782530000.0
        root = Path(tempfile.mkdtemp(prefix="ai-passport-sessions-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(root))
        # Recent session, last record 5 s ago.
        self._make_session(root / "a", "recent.jsonl", now - 5, now - 5)
        # Old session touched an hour ago.
        self._make_session(root / "b", "old.jsonl", now - 3600, now - 3600)
        active, age = collector.read_agent_state(str(root), now, active_window=300.0)
        self.assertEqual(active, 1)
        self.assertGreaterEqual(age, 4)
        self.assertLessEqual(age, 6)

    def test_agent_state_empty_and_missing(self) -> None:
        empty = Path(tempfile.mkdtemp(prefix="ai-passport-sessions-empty-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(empty))
        self.assertEqual(collector.read_agent_state(str(empty), time.time()), (0, -1))
        self.assertEqual(collector.read_agent_state(str(empty / "nope"), time.time()), (0, -1))


class CollectIntegrationTest(unittest.TestCase):
    def test_collect_with_fake_sources_produces_valid_payload(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="ai-passport-collect-int-")
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)

        (root / "thermal" / "thermal_zone0").mkdir(parents=True)
        (root / "thermal" / "thermal_zone0" / "temp").write_text("40000\n", encoding="utf-8")
        (root / "thermal" / "thermal_zone0" / "type").write_text("x86_pkg_temp\n", encoding="utf-8")

        (root / "net" / "eno1").mkdir(parents=True)
        (root / "net" / "eno1" / "operstate").write_text("up\n", encoding="utf-8")

        (root / "sessions" / "s").mkdir(parents=True)
        now = 1782530000.0
        from datetime import datetime, timezone

        iso = datetime.fromtimestamp(now - 10, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")
        session = root / "sessions" / "s" / "recent.jsonl"
        session.write_text('{"type":"message","timestamp":"%s"}\n' % iso, encoding="utf-8")
        os.utime(session, (now - 10, now - 10))

        sources = collector.Sources(
            now=now,
            proc_uptime=str((root / "uptime")),
            proc_loadavg=str((root / "loadavg")),
            proc_meminfo=str((root / "meminfo")),
            proc_stat=str((root / "stat")),
            proc_route=str((root / "route")),
            sys_net=str(root / "net"),
            thermal_glob=str(root / "thermal" / "thermal_zone*" / "temp"),
            sessions_dir=str(root / "sessions"),
            active_window=300.0,
            cpu_interval=0.0,
            statvfs_path=str(root),
        )
        (root / "uptime").write_text("128099.00 900000.00\n", encoding="utf-8")
        (root / "loadavg").write_text("0.42 0.31 0.28 1/345 1234\n", encoding="utf-8")
        (root / "meminfo").write_text(
            "MemTotal: 1000000 kB\nMemFree: 400000 kB\nMemAvailable: 600000 kB\n", encoding="utf-8"
        )
        (root / "stat").write_text("cpu  100 0 100 400 0 0 0 0 0 0\n", encoding="utf-8")
        (root / "route").write_text(
            "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
            "eno1\t00000000\t0140020A\t0003\t0\t0\t100\t00000000\t0\t0\t0\n",
            encoding="utf-8",
        )

        payload = collector.collect(sources)
        self.assertLessEqual(len(payload.encode("utf-8")), 200)
        fields = protocol_mod.decode(payload)
        self.assertEqual(fields["v"], "1")
        self.assertEqual(fields["t"], str(int(now)))
        self.assertEqual(fields["up"], "128099")
        self.assertEqual(fields["ld"], "0.42")
        self.assertEqual(fields["mem"], "40")
        self.assertEqual(fields["tmp"], "40")
        # The address lookup hits the real kernel interface table, so the test must
        # not assume whether the sandbox knows an "eno1" device: either a real IPv4
        # comes back or the lookup fails and the protocol placeholder "-" is used.
        iface, state, address = fields["net"].split(",")
        self.assertEqual(iface, "eno1")
        self.assertEqual(state, "up")
        self.assertTrue(
            address == "-" or re.fullmatch(r"\d{1,3}(\.\d{1,3}){3}", address),
            f"unexpected address token: {address!r}",
        )
        self.assertEqual(fields["ag"], "1")
        self.assertIn("as", fields)


if __name__ == "__main__":
    unittest.main()

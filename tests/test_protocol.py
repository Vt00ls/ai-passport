#!/usr/bin/env python3
"""Host tests for the dashboard status protocol reference implementation."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("host_protocol", ROOT / "host" / "protocol.py")
assert SPEC and SPEC.loader
protocol = importlib.util.module_from_spec(SPEC)
sys.modules["host_protocol"] = protocol
SPEC.loader.exec_module(protocol)


class EncodeTest(unittest.TestCase):
    def test_emits_known_fields_in_wire_order_and_skips_unknown(self) -> None:
        payload = protocol.encode({"t": 1, "v": 1, "h": "dev-forge", "junk": "x", "cpu": None})
        self.assertEqual(payload, "v=1;t=1;h=dev-forge")

    def test_sanitizes_values(self) -> None:
        self.assertEqual(protocol.sanitize("a;b=c d"), "abcd")
        self.assertEqual(protocol.sanitize(";;;"), "-")
        self.assertEqual(protocol.sanitize("eno1,up,10.2.65.61", allow_comma=True), "eno1,up,10.2.65.61")
        self.assertEqual(protocol.sanitize("a,b", allow_comma=False), "ab")

    def test_realistic_payload_stays_under_limit(self) -> None:
        payload = protocol.encode({
            "v": 1, "t": 1782530000, "h": "dev-forge", "up": 128099, "ld": "0.42",
            "cpu": 7, "mem": 38, "dsk": 33, "tmp": 51,
            "net": "eno1,up,10.2.65.61", "ag": 2, "as": 5,
        })
        self.assertLessEqual(len(payload.encode("utf-8")), protocol.MAX_BYTES)


class DecodeTest(unittest.TestCase):
    def test_roundtrip(self) -> None:
        text = "v=1;t=1782530000;h=dev-forge;up=86400;ld=0.42;cpu=7;mem=38;dsk=33;tmp=51;net=eno1,up,10.2.65.61;ag=2;as=5"
        fields = protocol.decode(text)
        self.assertEqual(list(fields), ["v", "t", "h", "up", "ld", "cpu", "mem", "dsk", "tmp", "net", "ag", "as"])
        self.assertEqual(protocol.parse_int(fields, "cpu"), 7)
        self.assertEqual(protocol.parse_int(fields, "as"), 5)
        self.assertIsNone(protocol.parse_int(fields, "nope", None))

    def test_missing_optional_is_not_an_error(self) -> None:
        fields = protocol.decode("v=1;t=5")
        self.assertEqual(fields, {"v": "1", "t": "5"})

    def test_rejects_malformed_inputs(self) -> None:
        bad = [
            "",
            "v=1;t=5\n",
            "v=1",  # missing required t
            "t=5",  # missing required v
            "v=1;t=5;",  # empty trailing segment
            "v=1;t=5;nokey",  # missing '='
            "v=1;t=5;foo=bar",  # unknown key
            "v=1;t=5;t=6",  # duplicate
            "v=1;t=5;h=a=b",  # two '=' in one segment
            "v=1;t=5;cpu=12.5",  # int field not integer
            "v=1;t=5;up=-1",  # negative (non-as) field
            "v=1;t=5;ld=0.5",  # ld not two decimals
            "v=1;t=5;net=eno1,up",  # net missing a component
            "v=1;t=5;net=eno1,maybe,10.0.0.1",  # bad state
            "v=2;t=5",  # unsupported version
        ]
        for text in bad:
            with self.subTest(text=text):
                with self.assertRaises(protocol.ProtocolError):
                    protocol.decode(text)

    def test_allows_negative_as(self) -> None:
        self.assertEqual(protocol.decode("v=1;t=5;as=-1")["as"], "-1")

    def test_allows_negative_temperature(self) -> None:
        self.assertEqual(protocol.decode("v=1;t=5;tmp=-5")["tmp"], "-5")


class FixtureTest(unittest.TestCase):
    """Replay protocol/fixtures/status_cases.txt.

    The same file drives tests/test_dashboard_protocol.c, so the Python producer
    and the firmware decoder are held to one set of cases.
    """

    FIXTURES = ROOT / "protocol" / "fixtures" / "status_cases.txt"

    def _cases(self):
        accepted, rejected = [], []
        for number, raw in enumerate(self.FIXTURES.read_text(encoding="utf-8").splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("ok|"):
                accepted.append((number, line[3:]))
            elif line.startswith("err|"):
                reason, _, payload = line[4:].partition("|")
                rejected.append((number, reason, payload))
            else:
                self.fail(f"{self.FIXTURES.name}:{number}: unknown directive: {line}")
        return accepted, rejected

    def test_fixture_file_is_populated(self) -> None:
        accepted, rejected = self._cases()
        self.assertGreaterEqual(len(accepted) + len(rejected), 30)
        self.assertGreaterEqual(len(accepted), 5)
        self.assertGreaterEqual(len(rejected), 10)

    def test_accepted_cases_parse(self) -> None:
        accepted, _ = self._cases()
        for number, payload in accepted:
            with self.subTest(line=number, payload=payload):
                self.assertEqual(protocol.decode(payload)["v"], "1")

    def test_rejected_cases_raise(self) -> None:
        _, rejected = self._cases()
        for number, reason, payload in rejected:
            with self.subTest(line=number, reason=reason, payload=payload):
                with self.assertRaises(protocol.ProtocolError):
                    protocol.decode(payload)


if __name__ == "__main__":
    unittest.main()

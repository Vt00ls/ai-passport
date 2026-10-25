#!/usr/bin/env python3
"""Push the dev-forge status string to the AI Passport badge over BLE.

This is the BLE *central* side: the badge is a peripheral that advertises the
dashboard service, and this process connects, then writes one status frame every
few seconds. Frame format: ``protocol/protocol.md``.

Requires BlueZ 5.43+ and the ``bleak`` package:

    python3 -m venv ~/.venvs/dashboard
    ~/.venvs/dashboard/bin/pip install bleak

Run as a systemd service so a host reboot restores the link without a login:

    host/systemd/ai-passport-dashboard.service

``--dry-run`` needs no Bluetooth stack and no ``bleak``: it prints the frames this
process would send, which is how the host side can be verified before a badge
exists.
"""

from __future__ import annotations

import argparse
import asyncio
import configparser
import errno
import logging
import os
import signal
import sys
import time
from dataclasses import dataclass
from pathlib import Path

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

import collector  # noqa: E402
import protocol  # noqa: E402

LOGGER = logging.getLogger("ble_central")

DEFAULTS = {
    "device_name": "PassportDash",
    "service_uuid": "1f7a0001-6b3c-4a1e-9c2d-5e8f0a9b4c10",
    "characteristic_uuid": "1f7a0002-6b3c-4a1e-9c2d-5e8f0a9b4c10",
    "interval_s": "5",
    "connect_timeout_s": "20",
    "retry_delay_s": "5",
    "max_retry_delay_s": "60",
    "sessions_dir": "",
}

# The badge needs this much ATT MTU to take the frame in a single write.
REQUIRED_MTU = protocol.MAX_BYTES + 3


@dataclass
class Settings:
    """Connection settings from the config file, overridable on the command line."""

    device_name: str
    service_uuid: str
    characteristic_uuid: str
    interval_s: float
    connect_timeout_s: float
    retry_delay_s: float
    max_retry_delay_s: float
    sessions_dir: str | None


def load_settings(path: str | None, overrides: dict[str, object]) -> Settings:
    """Read the config file, then apply explicit command-line overrides."""
    values = dict(DEFAULTS)
    if path:
        parser = configparser.ConfigParser()
        read = parser.read(path, encoding="utf-8")
        if not read:
            raise FileNotFoundError(errno.ENOENT, "config not found", path)
        if parser.has_section("dashboard"):
            for key in values:
                if parser.has_option("dashboard", key):
                    values[key] = parser.get("dashboard", key)
    for key, value in overrides.items():
        if value is not None:
            values[key] = value

    sessions_dir = values["sessions_dir"] or None
    return Settings(
        device_name=str(values["device_name"]),
        service_uuid=str(values["service_uuid"]),
        characteristic_uuid=str(values["characteristic_uuid"]),
        interval_s=float(values["interval_s"]),
        connect_timeout_s=float(values["connect_timeout_s"]),
        retry_delay_s=float(values["retry_delay_s"]),
        max_retry_delay_s=float(values["max_retry_delay_s"]),
        sessions_dir=sessions_dir,
    )


def build_frame(sessions_dir: str | None) -> bytes:
    """Collect the current status and return it as the BLE write payload."""
    sources = collector.Sources()
    if sessions_dir:
        sources.sessions_dir = sessions_dir
    payload = collector.collect(sources)
    encoded = payload.encode("utf-8")
    if len(encoded) > protocol.MAX_BYTES:
        # The collector already trims; this is a last-line guard so we never send
        # a frame the badge would reject.
        raise ValueError(f"frame is {len(encoded)} bytes, limit is {protocol.MAX_BYTES}")
    protocol.decode(payload)  # fail fast on our own malformed output
    return encoded


def backoff_delay(attempt: int, base: float, maximum: float) -> float:
    """Exponential backoff with a ceiling, so a badge left off does not spin."""
    if attempt <= 0:
        return base
    delay = base * (2 ** min(attempt - 1, 8))
    return min(delay, maximum)


async def find_device(settings: Settings):
    """Scan for the badge by service UUID, falling back to its advertised name."""
    from bleak import BleakScanner

    LOGGER.info("scanning for %s / %s", settings.device_name, settings.service_uuid)
    devices = await BleakScanner.discover(timeout=10.0, return_adv=True)
    for device, advertisement in devices.values():
        if settings.service_uuid.lower() in {str(u).lower() for u in advertisement.service_uuids}:
            LOGGER.info("found %s by service UUID (%s)", device.name or "unnamed", device.address)
            return device
    for device, advertisement in devices.values():
        if (device.name or advertisement.local_name or "") == settings.device_name:
            LOGGER.info("found %s by name (%s)", settings.device_name, device.address)
            return device
    return None


class Pusher:
    """One connection's worth of state. Recreated on every reconnect."""

    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.client = None
        self.stop_requested = asyncio.Event()
        self.frames_sent = 0
        self.write_errors = 0

    def request_stop(self) -> None:
        self.stop_requested.set()

    async def run(self) -> None:
        from bleak import BleakClient

        device = await find_device(self.settings)
        if device is None:
            raise RuntimeError("badge not found in scan")

        disconnected = asyncio.Event()

        def on_disconnect(_client) -> None:
            disconnected.set()

        LOGGER.info("connecting to %s", device.address)
        async with BleakClient(device, timeout=self.settings.connect_timeout_s,
                               disconnected_callback=on_disconnect) as client:
            self.client = client
            mtu = getattr(client, "mtu_size", None)
            if mtu:
                LOGGER.info("connected; ATT MTU %d (need >= %d)", mtu, REQUIRED_MTU)
                if mtu < REQUIRED_MTU:
                    LOGGER.warning(
                        "MTU %d is below %d: the badge may reject the frame", mtu, REQUIRED_MTU
                    )
            else:
                LOGGER.info("connected; MTU not reported by this backend")

            next_frame = time.monotonic()
            while not self.stop_requested.is_set():
                if disconnected.is_set():
                    LOGGER.warning("central disconnected; reconnecting")
                    return
                now = time.monotonic()
                if now >= next_frame:
                    await self._send_once(client)
                    next_frame = now + self.settings.interval_s
                # Sleep in short slices so SIGTERM is honoured promptly.
                try:
                    await asyncio.wait_for(self.stop_requested.wait(), timeout=0.25)
                except asyncio.TimeoutError:
                    pass

    async def _send_once(self, client) -> None:
        try:
            frame = build_frame(self.settings.sessions_dir)
        except (ValueError, OSError) as error:
            LOGGER.error("could not build frame: %s", error)
            return
        try:
            await client.write_gatt_char(
                self.settings.characteristic_uuid, frame, response=True
            )
            self.frames_sent += 1
            LOGGER.debug("sent %d bytes: %s", len(frame), frame.decode("utf-8", "replace"))
        except Exception as error:  # bleak raises backend-specific errors
            self.write_errors += 1
            LOGGER.warning("write failed (%d total): %s", self.write_errors, error)


async def run_forever(settings: Settings) -> int:
    pusher = Pusher(settings)
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        with_signal = getattr(loop, "add_signal_handler", None)
        if with_signal is not None:
            try:
                loop.add_signal_handler(sig, pusher.request_stop)
            except NotImplementedError:  # pragma: no cover - non-Unix platforms
                pass

    attempt = 0
    while not pusher.stop_requested.is_set():
        try:
            await pusher.run()
            attempt = 0
            if pusher.stop_requested.is_set():
                break
        except Exception as error:
            attempt += 1
            delay = backoff_delay(attempt, settings.retry_delay_s, settings.max_retry_delay_s)
            LOGGER.warning("connection attempt %d failed: %s (retry in %.0fs)",
                           attempt, error, delay)
            try:
                await asyncio.wait_for(pusher.stop_requested.wait(), timeout=delay)
            except asyncio.TimeoutError:
                pass
    LOGGER.info("stopped after %d frame(s), %d write error(s)",
                pusher.frames_sent, pusher.write_errors)
    return 0


def run_dry(settings: Settings, frames: int, interval: float) -> int:
    """Print frames without touching Bluetooth, for host-side verification."""
    sent = 0
    try:
        while frames <= 0 or sent < frames:
            frame = build_frame(settings.sessions_dir)
            print(f"[{sent + 1}] {len(frame):3d} B  {frame.decode('utf-8')}", flush=True)
            sent += 1
            if frames > 0 and sent >= frames:
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        pass
    return 0


def build_parser() -> argparse.ArgumentParser:
    default_config = os.path.join(_THIS_DIR, "dashboard.conf")
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default=default_config if os.path.exists(default_config) else None,
                        help="INI config path (default: host/dashboard.conf when present)")
    parser.add_argument("--dry-run", action="store_true",
                        help="print frames instead of writing them (no Bluetooth needed)")
    parser.add_argument("--frames", type=int, default=0,
                        help="with --dry-run, stop after N frames (0 = forever)")
    parser.add_argument("--interval", type=float, default=None, help="seconds between frames")
    parser.add_argument("--device-name", default=None)
    parser.add_argument("--sessions-dir", default=None)
    parser.add_argument("--verbose", "-v", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        stream=sys.stderr,
    )

    overrides = {
        "interval_s": args.interval,
        "device_name": args.device_name,
        "sessions_dir": args.sessions_dir,
    }
    try:
        settings = load_settings(args.config, overrides)
    except (FileNotFoundError, configparser.Error, ValueError) as error:
        LOGGER.error("configuration error: %s", error)
        return 2

    if args.dry_run:
        return run_dry(settings, args.frames, settings.interval_s)

    try:
        import bleak  # noqa: F401
    except ImportError:
        LOGGER.error(
            "the 'bleak' package is required: python3 -m venv ~/.venvs/dashboard && "
            "~/.venvs/dashboard/bin/pip install bleak"
        )
        return 2

    try:
        return asyncio.run(run_forever(settings))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Reference encoder/decoder for the dashboard status protocol.

Pure Python 3 standard-library code shared by collector.py, http_debug.py, and
the upcoming ble_central.py. The authoritative format lives in
``protocol/protocol.md``; keep this module aligned with that document.
"""

from __future__ import annotations

from collections import OrderedDict
from typing import Dict, List, Tuple

PROTOCOL_VERSION = 1
MAX_BYTES = 200

# Field length caps, mirroring main/app_dashboard/dashboard_protocol.h.
HOST_MAX = 24
NET_IFACE_MAX = 12
NET_IPV4_MAX = 15

# Field order on the wire. Keys are lowercase ASCII.
FIELD_ORDER: Tuple[str, ...] = (
    "v",
    "t",
    "h",
    "nc",
    "up",
    "ld",
    "cpu",
    "mem",
    "dsk",
    "tmp",
    "net",
    "ag",
    "as",
)
KNOWN_KEYS = frozenset(FIELD_ORDER)
INT_KEYS = frozenset({"v", "t", "nc", "up", "cpu", "mem", "dsk", "tmp", "ag", "as"})
REQUIRED_KEYS = frozenset({"v", "t"})
# Signed range per integer key: (minimum, maximum).
INT_RANGES = {
    "v": (1, 255),
    "t": (0, 2**32 - 1),
    "nc": (1, 1024),
    "up": (0, 2**32 - 1),
    "cpu": (0, 100),
    "mem": (0, 100),
    "dsk": (0, 100),
    "tmp": (-40, 125),
    "ag": (0, 65535),
    "as": (-1, 2**31 - 1),
}

_VALUE_CHARS = frozenset(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._/-"
)
# net is the only field whose value may contain a comma separator.
_NET_CHARS = _VALUE_CHARS | frozenset({","})


class ProtocolError(ValueError):
    """Raised when a status string violates the protocol."""


def sanitize(value: object, allow_comma: bool = False) -> str:
    """Return a protocol-safe scalar value string.

    Illegal characters are dropped; an empty result becomes ``-`` so the badge
    can distinguish "no value" from a corrupted token.
    """
    allowed = _NET_CHARS if allow_comma else _VALUE_CHARS
    text = str(value)
    out = "".join(ch for ch in text if ch in allowed)
    return out or "-"


def encode(fields: Dict[str, object]) -> str:
    """Encode known fields into a protocol-compliant status string.

    Fields are emitted in :data:`FIELD_ORDER`; unknown keys and ``None`` values
    are skipped. Values are sanitized, not validated, so callers must pass
    correctly-typed scalars (see protocol/protocol.md).
    """
    parts: List[str] = []
    for key in FIELD_ORDER:
        if key not in fields:
            continue
        value = fields[key]
        if value is None:
            continue
        parts.append(f"{key}={sanitize(value, allow_comma=key == 'net')}")
    return ";".join(parts)


def _is_int(value: str) -> bool:
    body = value[1:] if value.startswith("-") else value
    return bool(body) and body.isdigit() and body.isascii()


def _check_value(key: str, value: str) -> None:
    allowed = _NET_CHARS if key == "net" else _VALUE_CHARS
    if not value:
        raise ProtocolError(f"field {key!r} has an empty value")
    if any(ch not in allowed for ch in value):
        raise ProtocolError(f"field {key!r} has invalid characters: {value!r}")
    if key in INT_KEYS:
        if not _is_int(value):
            raise ProtocolError(f"field {key!r} must be an integer: {value!r}")
        low, high = INT_RANGES[key]
        number = int(value)
        if not low <= number <= high:
            raise ProtocolError(
                f"field {key!r} out of range [{low}, {high}]: {value!r}"
            )
    if key == "ld":
        if value.count(".") != 1:
            raise ProtocolError(f"field {key!r} must be a two-decimal float string: {value!r}")
        whole, frac = value.split(".")
        if not (whole.isascii() and whole.isdigit() and frac.isascii() and frac.isdigit() and len(frac) == 2):
            raise ProtocolError(f"field {key!r} must be a two-decimal float string: {value!r}")
        if int(whole) > 655:
            raise ProtocolError(f"field {key!r} out of range: {value!r}")
    if key == "net":
        if value.count(",") != 2:
            raise ProtocolError(f"field {key!r} must be iface,state,ip: {value!r}")
        iface, state, ip = value.split(",")
        if state not in ("up", "down"):
            raise ProtocolError(f"field {key!r} state must be up or down: {value!r}")
        if not iface or len(iface) > NET_IFACE_MAX:
            raise ProtocolError(f"field {key!r} interface name out of range: {value!r}")
        if not ip or len(ip) > NET_IPV4_MAX:
            raise ProtocolError(f"field {key!r} address out of range: {value!r}")
    if key == "h" and len(value) > HOST_MAX:
        raise ProtocolError(f"field {key!r} host name out of range: {value!r}")


def decode(text: str) -> "OrderedDict[str, str]":
    """Parse and validate a status string, returning an ordered mapping.

    Values are returned as strings; numeric fields are validated but not
    coerced. Raises :class:`ProtocolError` on any violation.
    """
    if not isinstance(text, str) or not text:
        raise ProtocolError("status string must be a non-empty string")
    if "\n" in text or "\r" in text:
        raise ProtocolError("status string must be a single line")

    result: "OrderedDict[str, str]" = OrderedDict()
    for part in text.split(";"):
        if "=" not in part:
            raise ProtocolError(f"missing '=' in segment: {part!r}")
        key, _, value = part.partition("=")
        if "=" in value:
            raise ProtocolError(f"more than one '=' in segment: {part!r}")
        if key not in KNOWN_KEYS:
            raise ProtocolError(f"unknown field: {key!r}")
        if key in result:
            raise ProtocolError(f"duplicate field: {key!r}")
        _check_value(key, value)
        result[key] = value

    missing = REQUIRED_KEYS - set(result)
    if missing:
        raise ProtocolError(f"missing required field(s): {', '.join(sorted(missing))}")
    if int(result["v"]) > PROTOCOL_VERSION:
        raise ProtocolError(
            f"unsupported protocol version {result['v']} (implemented: {PROTOCOL_VERSION})"
        )
    return result


def parse_int(fields: "OrderedDict[str, str]", key: str, default: int | None = None) -> int | None:
    """Return an integer field value, or ``default`` when absent."""
    if key not in fields:
        return default
    return int(fields[key])

from __future__ import annotations

import json
from pathlib import Path

import pytest

from fhcontent.server import ContentServer, Origin
from fhcontent.trace import LinkTrace

DATA = Path(__file__).parent / "data"


def make_trace(
    duration_s: float = 30.0,
    rate_kbps: int = 8000,
    handoffs: list[tuple[float, float, bool]] = (),
    imminent: list[float] = (),
    algo: str = "adapted",
) -> LinkTrace:
    """Synthetic trace: constant rate, handoffs as (start_s, end_s, subnet_changed)."""
    recs: list[dict] = [{"type": "META", "scenario": "synthetic", "algo": algo, "subnet": 0}]
    subnet = 0
    for t in imminent:
        recs.append({"type": "HANDOFF_IMMINENT", "t": int(t * 1000), "from": 0, "to": 1, "eta_ms": 1000})
    for start, end, changed in handoffs:
        subnet += int(changed)
        recs.append({"type": "HANDOFF_START", "t": int(start * 1000), "from": 0, "to": 1})
        recs.append(
            {
                "type": "HANDOFF_DONE",
                "t": int(end * 1000),
                "from": 0,
                "to": 1,
                "subnet": subnet,
                "subnet_changed": changed,
                "aborted": False,
            }
        )
    for ms in range(0, int(duration_s * 1000) + 1, 250):
        recs.append({"type": "LINK", "t": ms, "up": True, "ap": 0, "rssi": -60, "rate_kbps": rate_kbps})
    recs.append({"type": "END", "t": int(duration_s * 1000)})
    recs.sort(key=lambda r: (r.get("t", -1), r["type"] != "META"))
    return LinkTrace.from_lines(json.dumps(r) for r in recs)


@pytest.fixture
def server():
    origin = Origin(bitrate_kbps=2000, n_segments=200, files={"f.bin": 4 * 1024 * 1024, "small.bin": 1000})
    with ContentServer(origin, cache_bytes=16 * 1024 * 1024) as srv:
        yield srv

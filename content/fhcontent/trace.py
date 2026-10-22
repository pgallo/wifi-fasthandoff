"""Reader for the simulator's JSONL trace (driver events plus link samples)."""

from __future__ import annotations

import bisect
import json
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path


@dataclass(frozen=True)
class DriverEvent:
    t: float  # seconds
    type: str
    data: dict = field(default_factory=dict, compare=False)


@dataclass(frozen=True)
class Outage:
    """Data path down between HANDOFF_START and HANDOFF_DONE."""

    start: float
    end: float
    ip_change: bool
    aborted: bool


def client_ip(subnet: int, host: int = 23) -> str:
    """Address a client gets on a subnet (one subnet per group of APs)."""
    return f"10.{20 + subnet}.{host // 250}.{host % 250 + 1}"


class LinkTrace:
    """Piecewise-constant link rate with explicit outages and an IP timeline."""

    def __init__(self, meta: dict, records: list[dict]):
        self.meta = meta
        self.scenario = meta.get("scenario", "?")
        self.algo = meta.get("algo", "?")
        self.events: list[DriverEvent] = []
        self.outages: list[Outage] = []
        self._st: list[float] = []  # sample times
        self._rate: list[float] = []  # bits per second
        self._subnet_t: list[float] = [0.0]
        self._subnet: list[int] = [int(meta.get("subnet", 0))]
        self.end = 0.0
        start: float | None = None

        for r in records:
            t = r["t"] / 1000.0
            kind = r["type"]
            if kind == "LINK":
                self._st.append(t)
                self._rate.append(r["rate_kbps"] * 1000.0 if r["up"] else 0.0)
            elif kind == "END":
                self.end = t
            else:
                self.events.append(DriverEvent(t, kind, r))
                if kind == "HANDOFF_START":
                    start = t
                elif kind == "HANDOFF_DONE":
                    s = t if start is None else start
                    self.outages.append(Outage(s, t, bool(r.get("subnet_changed")), bool(r.get("aborted"))))
                    if r.get("subnet_changed"):
                        self._subnet_t.append(t)
                        self._subnet.append(int(r["subnet"]))
                    start = None
        if not self._st:
            raise ValueError("trace has no LINK samples")
        self.end = max(self.end, self._st[-1])
        self._ostarts = [o.start for o in self.outages]

    @classmethod
    def from_lines(cls, lines: Iterable[str]) -> LinkTrace:
        recs = [json.loads(line) for line in lines if line.strip()]
        if not recs or recs[0].get("type") != "META":
            raise ValueError("trace must start with a META record")
        return cls(recs[0], recs[1:])

    @classmethod
    def load(cls, path: str | Path) -> LinkTrace:
        with open(path, encoding="utf-8") as f:
            return cls.from_lines(f)

    def outage_at(self, t: float) -> Outage | None:
        i = bisect.bisect_right(self._ostarts, t) - 1
        if i >= 0 and self.outages[i].start <= t < self.outages[i].end:
            return self.outages[i]
        return None

    def rate_at(self, t: float) -> float:
        """Goodput in bit/s at time t, 0 during an outage."""
        if self.outage_at(t):
            return 0.0
        i = max(0, bisect.bisect_right(self._st, t) - 1)
        return self._rate[i]

    def next_change(self, t: float) -> float:
        """The next time after t at which rate_at may change."""
        cands = [self.end]
        i = bisect.bisect_right(self._st, t)
        if i < len(self._st):
            cands.append(self._st[i])
        j = bisect.bisect_right(self._ostarts, t)
        if j < len(self._ostarts):
            cands.append(self._ostarts[j])
        o = self.outage_at(t)
        if o:
            cands.append(o.end)
        nxt = min(c for c in cands if c > t) if any(c > t for c in cands) else t
        return nxt

    def subnet_at(self, t: float) -> int:
        i = bisect.bisect_right(self._subnet_t, t) - 1
        return self._subnet[max(i, 0)]

    def ip_at(self, t: float, host: int = 23) -> str:
        return client_ip(self.subnet_at(t), host)

    def handoffs(self) -> int:
        return sum(1 for o in self.outages if not o.aborted)

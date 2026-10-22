"""Clients that replay a driver trace against the content server.

Bytes move over real HTTP; time is simulated. Each chunk read from the socket
is charged to the trace's link rate, so a handoff in the trace interrupts the
transfer at the right byte. Two policies share all the code:

- unaware: ignores driver events. A transfer cut by an address change is
  noticed only when its read times out; it then reconnects, starts the object
  again from byte 0, and, because its session is bound to the old address,
  has to open a new session first.
- aware: subscribes to driver events. HANDOFF_IMMINENT raises the buffer
  target (prefetch); between HANDOFF_START and HANDOFF_DONE it starts no new
  request; after HANDOFF_DONE it reconnects at once and resumes with a byte
  range and its session token, so the server continues the same session.
"""

from __future__ import annotations

import http.client
import json
import math
import zlib
from collections import deque
from dataclasses import dataclass
from urllib.parse import urlparse

from fhcontent.trace import DriverEvent, LinkTrace, Outage


@dataclass(frozen=True)
class ClientConfig:
    handoff_aware: bool
    buffer_target_s: float = 6.0
    prefetch_boost_s: float = 6.0
    startup_s: float = 2.0
    rebuffer_s: float = 2.0
    rtt_s: float = 0.02
    app_timeout_s: float = 2.0  # how long a read on a dead connection blocks
    chunk_bytes: int = 32 * 1024
    host_id: int = 23  # host part of the simulated address, unique per device

    @property
    def name(self) -> str:
        return "aware" if self.handoff_aware else "unaware"


@dataclass
class TransferStats:
    bytes_received: int = 0
    bytes_redownloaded: int = 0
    resumed_requests: int = 0
    restarted_requests: int = 0
    session_restarts: int = 0
    integrity_failures: int = 0


class _Response:
    def __init__(self, status: int, headers: dict[str, str], body: bytes, complete: bool):
        self.status, self.headers, self.body, self.complete = status, headers, body, complete


class TraceClient:
    """Simulated clock, driver event delivery and link-limited HTTP transfers."""

    def __init__(self, trace: LinkTrace, base_url: str, cfg: ClientConfig):
        self.trace = trace
        self.cfg = cfg
        u = urlparse(base_url)
        self._host, self._port = u.hostname or "127.0.0.1", u.port or 80
        self.t = 0.0
        self._events: deque[DriverEvent] = deque(trace.events)
        self.in_transition = False
        self.need_new_conn = True
        self.conn_ip = self.ip_at(0.0)
        self.token: str | None = None
        self.xfer = TransferStats()

    def ip_at(self, t: float) -> str:
        return self.trace.ip_at(t, self.cfg.host_id)

    # ---- time and events ----
    @property
    def done(self) -> bool:
        return self.t >= self.trace.end

    def advance(self, t_new: float) -> None:
        t_new = min(t_new, self.trace.end)
        while True:
            while self._events and self._events[0].t <= self.t:
                self._dispatch(self._events.popleft())
            if self.t >= t_new:
                return
            nxt = self._events[0].t if self._events else math.inf
            step = min(t_new, nxt)
            self.elapse(step - self.t)
            self.t = step

    def _dispatch(self, ev: DriverEvent) -> None:
        if not self.cfg.handoff_aware:
            return
        if ev.type == "HANDOFF_START":
            self.in_transition = True
        elif ev.type == "HANDOFF_DONE":
            self.in_transition = False
            if ev.data.get("subnet_changed"):
                self.need_new_conn = True  # the old socket is bound to an address we no longer have
        self.on_driver_event(ev)

    def on_driver_event(self, ev: DriverEvent) -> None:
        pass

    def elapse(self, dt: float) -> None:
        pass

    # ---- link model ----
    @staticmethod
    def _tcp_resume(o: Outage) -> float:
        """First retransmission after the link is back (RTO 200 ms, doubling)."""
        rto, t = 0.2, o.start + 0.2
        while t < o.end:
            rto *= 2
            t += rto
        return t

    def _deliver(self, nbytes: int, ip: str) -> tuple[int, bool]:
        """Push nbytes through the link. Returns (bytes delivered, connection cut)."""
        sent = 0.0
        while sent < nbytes:
            if self.done:
                return int(sent), True
            o = self.trace.outage_at(self.t)
            if o:
                if o.ip_change:
                    return int(sent), True
                self.advance(self._tcp_resume(o))  # same subnet: TCP rides it out
                continue
            if self.ip_at(self.t) != ip:
                return int(sent), True
            rate = self.trace.rate_at(self.t)
            nxt = self.trace.next_change(self.t)
            if rate <= 0:
                self.advance(nxt)
                continue
            cap = rate / 8.0 * (nxt - self.t)
            if nbytes - sent >= cap:
                sent += cap
                self.advance(nxt)
            else:
                self.advance(self.t + (nbytes - sent) * 8.0 / rate)
                sent = nbytes
        return nbytes, False

    def _wait_for_link(self) -> None:
        o = self.trace.outage_at(self.t)
        if not o:
            return
        if self.cfg.handoff_aware:
            self.advance(o.end)  # HANDOFF_DONE tells us when
            return
        k = 1.0  # connect retries at 1, 3, 7, 15 s
        while self.t + k < o.end:
            k = 2 * k + 1
        self.advance(self.t + k)

    def _after_cut(self) -> None:
        o = self.trace.outage_at(self.t)
        end = o.end if o else self.t
        self.need_new_conn = True
        if self.cfg.handoff_aware:
            self.advance(end)
        else:
            self.advance(max(self.t + self.cfg.app_timeout_s, end))

    # ---- HTTP ----
    def get(self, path: str, offset: int = 0, extra: dict[str, str] | None = None) -> _Response:
        self._wait_for_link()
        ip = self.ip_at(self.t)
        if not self.need_new_conn and ip != self.conn_ip:
            # idle keep-alive connection on a stale address: found out by timing out
            self.advance(self.t + self.cfg.app_timeout_s)
            self.need_new_conn = True
            ip = self.ip_at(self.t)
        self.advance(self.t + self.cfg.rtt_s * (2 if self.need_new_conn else 1))
        if self.need_new_conn:
            self.conn_ip = ip
            self.need_new_conn = False

        headers = {"X-Client-Addr": ip, **(extra or {})}
        if self.token:
            headers["X-Session"] = self.token
        if offset:
            headers["Range"] = f"bytes={offset}-"
        conn = http.client.HTTPConnection(self._host, self._port, timeout=30)
        try:
            conn.request("GET", path, headers=headers)
            resp = conn.getresponse()
            hdrs = {k.lower(): v for k, v in resp.getheaders()}
            if resp.status not in (200, 206):
                return _Response(resp.status, hdrs, resp.read(), True)
            got = bytearray()
            while chunk := resp.read(self.cfg.chunk_bytes):
                n, cut = self._deliver(len(chunk), ip)
                got += chunk[:n]
                if cut:
                    return _Response(resp.status, hdrs, bytes(got), False)
            return _Response(resp.status, hdrs, bytes(got), True)
        finally:
            conn.close()

    def fetch_object(self, path: str) -> bytes | None:
        """Fetch a whole object across interruptions; None if the trace ends first."""
        data = bytearray()
        while not self.done:
            if data and not self.cfg.handoff_aware:
                self.xfer.bytes_redownloaded += len(data)
                self.xfer.restarted_requests += 1
                data = bytearray()
            elif data:
                self.xfer.resumed_requests += 1
            r = self.get(path, offset=len(data))
            if r.status == 403:
                self.restart_session()
                continue
            if r.status not in (200, 206):
                raise RuntimeError(f"GET {path}: HTTP {r.status}")
            data += r.body
            self.xfer.bytes_received += len(r.body)
            if r.complete:
                if zlib.crc32(data) != int(r.headers.get("x-content-crc32", "-1")):
                    self.xfer.integrity_failures += 1
                return bytes(data)
            self._after_cut()
        return None

    def restart_session(self) -> None:
        raise RuntimeError("session rejected")


@dataclass
class StreamMetrics:
    client: str
    startup_s: float | None = None
    stalls: int = 0
    stall_s: float = 0.0
    played_s: float = 0.0
    segments: int = 0
    prefetch_windows: int = 0
    session_restarts: int = 0
    bytes_received: int = 0
    bytes_redownloaded: int = 0
    resumed_requests: int = 0
    integrity_failures: int = 0


class StreamingClient(TraceClient):
    """Segment player with a buffer target; playback runs on the simulated clock."""

    def __init__(self, trace: LinkTrace, base_url: str, cfg: ClientConfig, asset: str = "campus"):
        super().__init__(trace, base_url, cfg)
        self.asset = asset
        self.buffer = 0.0
        self.playing = False
        self.started = False
        self.boost = False
        self.m = StreamMetrics(cfg.name)

    @property
    def target(self) -> float:
        return self.cfg.buffer_target_s + (self.cfg.prefetch_boost_s if self.boost else 0.0)

    def on_driver_event(self, ev: DriverEvent) -> None:
        if ev.type == "HANDOFF_IMMINENT":
            self.boost = True
            self.m.prefetch_windows += 1
        elif ev.type == "HANDOFF_DONE":
            self.boost = False

    def elapse(self, dt: float) -> None:
        if not self.playing:
            if self.started:
                self.m.stall_s += dt
            return
        if self.buffer >= dt:
            self.buffer -= dt
            self.m.played_s += dt
            return
        self.m.played_s += self.buffer
        self.m.stall_s += dt - self.buffer
        self.buffer = 0.0
        self.playing = False
        self.m.stalls += 1

    def open_session(self) -> dict:
        extra = {"X-Want-Token": "1"} if self.cfg.handoff_aware else None
        self.token = None
        r = self.get(f"/manifest/{self.asset}", extra=extra)
        if r.status != 200 or not r.complete:
            raise RuntimeError("manifest request failed")
        man = json.loads(r.body)
        if self.cfg.handoff_aware:
            self.token = man["token"]
        return man

    def restart_session(self) -> None:
        self.m.session_restarts += 1
        self.open_session()

    def run(self) -> StreamMetrics:
        man = self.open_session()
        seg_dur, n_seg, seg = man["seg_duration"], man["segments"], 0
        while not self.done and seg < n_seg:
            if self.buffer >= self.target:
                self.advance(self.t + min(0.25, self.buffer - self.target + 0.01))
                continue
            if self.cfg.handoff_aware and self.in_transition:
                o = self.trace.outage_at(self.t)
                self.advance(o.end if o else self.t + 0.01)
                continue
            if self.fetch_object(f"/media/{self.asset}/{seg}") is None:
                break
            seg += 1
            self.m.segments += 1
            self.buffer += seg_dur
            need = self.cfg.rebuffer_s if self.started else self.cfg.startup_s
            if not self.playing and self.buffer >= need:
                self.playing = True
                if not self.started:
                    self.started = True
                    self.m.startup_s = self.t
        self.advance(self.trace.end)
        x = self.xfer
        self.m.bytes_received, self.m.bytes_redownloaded = x.bytes_received, x.bytes_redownloaded
        self.m.resumed_requests, self.m.integrity_failures = x.resumed_requests, x.integrity_failures
        return self.m


@dataclass
class DownloadMetrics:
    client: str
    completed: bool
    time_s: float | None
    bytes_received: int
    bytes_redownloaded: int
    restarts_from_zero: int
    resumed_requests: int
    integrity_failures: int


class Downloader(TraceClient):
    def run(self, name: str) -> DownloadMetrics:
        data = self.fetch_object(f"/files/{name}")
        x = self.xfer
        return DownloadMetrics(
            client=self.cfg.name,
            completed=data is not None,
            time_s=self.t if data is not None else None,
            bytes_received=x.bytes_received,
            bytes_redownloaded=x.bytes_redownloaded,
            restarts_from_zero=x.restarted_requests,
            resumed_requests=x.resumed_requests,
            integrity_failures=x.integrity_failures,
        )

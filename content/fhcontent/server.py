"""A small content delivery server: segmented media and large files over HTTP/1.1.

- byte ranges (RFC 9110 single-range requests: 206, 416, Content-Range)
- sessions that survive a client address change when the client presents its token
- an LRU edge cache in front of a deterministic origin

The simulated client address travels in the X-Client-Addr header, which stands
in for the TCP source address (everything runs on loopback).
"""

from __future__ import annotations

import json
import random
import re
import secrets
import sys
import threading
import zlib
from collections import OrderedDict
from dataclasses import asdict, dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SEG_DURATION_S = 2.0


class RangeNotSatisfiable(ValueError):
    pass


_RANGE = re.compile(r"^bytes=(\d*)-(\d*)$")


def parse_range(header: str | None, size: int) -> tuple[int, int] | None:
    """Return an inclusive (first, last) byte range, or None for the whole object.

    Multi-range and malformed headers are ignored (the full object is served),
    which RFC 9110 allows. An unsatisfiable range raises RangeNotSatisfiable.
    """
    if not header:
        return None
    m = _RANGE.match(header.strip())
    if not m or (m.group(1) == "" and m.group(2) == ""):
        return None
    first_s, last_s = m.groups()
    if first_s == "":  # suffix range: the last N bytes
        n = int(last_s)
        if n == 0:
            raise RangeNotSatisfiable(header)
        return max(0, size - n), size - 1
    first = int(first_s)
    last = size - 1 if last_s == "" else min(int(last_s), size - 1)
    if first >= size or first > last:
        raise RangeNotSatisfiable(header)
    return first, last


class Origin:
    """Deterministic synthetic content: same name, same bytes, every time."""

    def __init__(self, bitrate_kbps: int = 4000, n_segments: int = 900, files: dict[str, int] | None = None):
        self.bitrate_kbps = bitrate_kbps
        self.n_segments = n_segments
        self.seg_bytes = int(bitrate_kbps * 1000 / 8 * SEG_DURATION_S)
        self.files = files if files is not None else {"dataset.bin": 32 * 1024 * 1024}
        self.fetches = 0

    @staticmethod
    def _bytes(key: str, n: int) -> bytes:
        return random.Random(zlib.crc32(key.encode())).randbytes(n)

    def manifest(self, asset: str) -> dict:
        return {
            "asset": asset,
            "segments": self.n_segments,
            "seg_bytes": self.seg_bytes,
            "seg_duration": SEG_DURATION_S,
            "bitrate_kbps": self.bitrate_kbps,
        }

    def load(self, key: str) -> bytes:
        self.fetches += 1
        kind, _, rest = key.partition(":")
        if kind == "seg":
            return self._bytes(key, self.seg_bytes)
        if kind == "file" and rest in self.files:
            return self._bytes(key, self.files[rest])
        raise KeyError(key)


class EdgeCache:
    """Byte-bounded LRU cache in front of the origin."""

    def __init__(self, origin: Origin, capacity_bytes: int = 48 * 1024 * 1024):
        self.origin = origin
        self.capacity = capacity_bytes
        self._items: OrderedDict[str, tuple[bytes, int]] = OrderedDict()
        self._size = 0
        self._lock = threading.Lock()
        self.hits = self.misses = self.evictions = 0

    def get(self, key: str) -> tuple[bytes, int]:
        """Object bytes and their CRC32."""
        with self._lock:
            if key in self._items:
                self._items.move_to_end(key)
                self.hits += 1
                return self._items[key]
            self.misses += 1
        data = self.origin.load(key)
        item = (data, zlib.crc32(data))
        with self._lock:
            if key not in self._items:
                self._items[key] = item
                self._size += len(data)
                while self._size > self.capacity and len(self._items) > 1:
                    _, (old, _) = self._items.popitem(last=False)
                    self._size -= len(old)
                    self.evictions += 1
        return item


@dataclass
class Session:
    token: str
    asset: str
    ip: str
    anonymous: bool
    ip_changes: int = 0
    bytes_served: int = 0


@dataclass
class SessionStats:
    created: int = 0
    resumed_after_ip_change: int = 0
    rejected: int = 0


class SessionTable:
    """Token sessions follow the client across addresses; anonymous ones are pinned to one."""

    def __init__(self) -> None:
        self._by_token: dict[str, Session] = {}
        self._by_ip: dict[str, str] = {}
        self._lock = threading.Lock()
        self.stats = SessionStats()

    def open(self, asset: str, ip: str, anonymous: bool) -> Session:
        s = Session(secrets.token_hex(8), asset, ip, anonymous)
        with self._lock:
            self._by_token[s.token] = s
            if anonymous:
                self._by_ip[ip] = s.token
            self.stats.created += 1
        return s

    def lookup(self, token: str | None, ip: str) -> Session | None:
        with self._lock:
            if token:
                s = self._by_token.get(token)
                if s and s.ip != ip:
                    s.ip = ip
                    s.ip_changes += 1
                    self.stats.resumed_after_ip_change += 1
            else:
                s = self._by_token.get(self._by_ip.get(ip, ""))
            if s is None:
                self.stats.rejected += 1
            return s


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: _Server

    def log_message(self, format: str, *args) -> None:  # noqa: A002 - signature from the base class
        pass

    def _client(self) -> str:
        return self.headers.get("X-Client-Addr") or self.client_address[0]

    def _send(self, code: int, body: bytes, headers: dict[str, str] | None = None) -> None:
        try:
            self.send_response(code)
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # the client went away mid-response: a cut link

    def _json(self, code: int, obj: dict, headers: dict[str, str] | None = None) -> None:
        self._send(code, json.dumps(obj).encode(), {"Content-Type": "application/json", **(headers or {})})

    def _object(self, key: str, session: Session | None) -> None:
        data, crc = self.server.edge.get(key)
        hdr = {
            "Accept-Ranges": "bytes",
            "X-Content-CRC32": str(crc),
            "Content-Type": "application/octet-stream",
        }
        try:
            rng = parse_range(self.headers.get("Range"), len(data))
        except RangeNotSatisfiable:
            self._send(416, b"", {**hdr, "Content-Range": f"bytes */{len(data)}"})
            return
        if rng is None:
            body, code = data, 200
        else:
            first, last = rng
            body, code = data[first : last + 1], 206
            hdr["Content-Range"] = f"bytes {first}-{last}/{len(data)}"
        if session:
            session.bytes_served += len(body)
        self.server.bytes_out += len(body)
        self._send(code, body, hdr)

    def do_GET(self) -> None:  # noqa: N802 - http.server naming
        parts = [p for p in self.path.split("?")[0].split("/") if p]
        ip = self._client()
        token = self.headers.get("X-Session")
        origin = self.server.edge.origin

        if len(parts) == 2 and parts[0] == "manifest":
            anonymous = token is None and not self.headers.get("X-Want-Token")
            s = self.server.sessions.open(parts[1], ip, anonymous=anonymous)
            self._json(200, {**origin.manifest(parts[1]), "token": s.token}, {"X-Session": s.token})
        elif len(parts) == 3 and parts[0] == "media" and parts[2].isdigit():
            s = self.server.sessions.lookup(token, ip)
            if s is None or s.asset != parts[1]:
                self._json(403, {"error": "no session for this client"})
                return
            n = int(parts[2])
            if n >= origin.n_segments:
                self._json(404, {"error": "no such segment"})
                return
            self._object(f"seg:{parts[1]}/{n}", s)
        elif len(parts) == 2 and parts[0] == "files":
            if parts[1] not in origin.files:
                self._json(404, {"error": "no such file"})
                return
            self._object(f"file:{parts[1]}", None)
        elif parts == ["stats"]:
            self._json(200, self.server.stats())
        else:
            self._json(404, {"error": "not found"})


class _Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, edge: EdgeCache):
        super().__init__(addr, _Handler)
        self.edge = edge
        self.sessions = SessionTable()
        self.bytes_out = 0

    def handle_error(self, request, client_address) -> None:
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionResetError, BrokenPipeError)):
            return  # clients drop connections on purpose when the simulated link is cut
        super().handle_error(request, client_address)

    def stats(self) -> dict:
        e = self.edge
        return {
            "sessions": asdict(self.sessions.stats),
            "edge": {"hits": e.hits, "misses": e.misses, "evictions": e.evictions},
            "bytes_out": self.bytes_out,
        }


@dataclass
class ContentServer:
    """Runs the server on a loopback port in a background thread."""

    origin: Origin = field(default_factory=Origin)
    cache_bytes: int = 128 * 1024 * 1024
    host: str = "127.0.0.1"
    port: int = 0
    _srv: _Server | None = field(default=None, init=False, repr=False)
    _thread: threading.Thread | None = field(default=None, init=False, repr=False)

    def start(self) -> ContentServer:
        self._srv = _Server((self.host, self.port), EdgeCache(self.origin, self.cache_bytes))
        self.port = self._srv.server_address[1]
        self._thread = threading.Thread(target=self._srv.serve_forever, name="fhcontent-server", daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        if self._srv:
            self._srv.shutdown()
            self._srv.server_close()
            self._srv = None

    def __enter__(self) -> ContentServer:
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}"

    @property
    def sessions(self) -> SessionTable:
        assert self._srv
        return self._srv.sessions

    def stats(self) -> dict:
        assert self._srv
        return self._srv.stats()

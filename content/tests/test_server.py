import http.client
import json
import zlib
from urllib.parse import urlparse

import pytest

from fhcontent.server import EdgeCache, Origin, RangeNotSatisfiable, SessionTable, parse_range


@pytest.mark.parametrize(
    ("header", "expected"),
    [
        (None, None),
        ("bytes=0-99", (0, 99)),
        ("bytes=100-", (100, 999)),
        ("bytes=-10", (990, 999)),
        ("bytes=-5000", (0, 999)),
        ("bytes=900-5000", (900, 999)),
        ("bytes=0-1,5-6", None),  # multi-range: whole object
        ("items=0-1", None),
    ],
)
def test_parse_range(header, expected):
    assert parse_range(header, 1000) == expected


@pytest.mark.parametrize("header", ["bytes=1000-", "bytes=5-4", "bytes=-0"])
def test_parse_range_unsatisfiable(header):
    with pytest.raises(RangeNotSatisfiable):
        parse_range(header, 1000)


def _get(srv, path, **headers):
    u = urlparse(srv.url)
    conn = http.client.HTTPConnection(u.hostname, u.port, timeout=10)
    conn.request("GET", path, headers=headers)
    r = conn.getresponse()
    body = r.read()
    conn.close()
    return r.status, {k.lower(): v for k, v in r.getheaders()}, body


def test_range_requests_reassemble_the_object(server):
    status, hdr, whole = _get(server, "/files/small.bin")
    assert status == 200 and len(whole) == 1000
    assert int(hdr["x-content-crc32"]) == zlib.crc32(whole)
    s1, h1, a = _get(server, "/files/small.bin", Range="bytes=0-399")
    s2, h2, b = _get(server, "/files/small.bin", Range="bytes=400-")
    assert (s1, s2) == (206, 206)
    assert h2["content-range"] == "bytes 400-999/1000"
    assert a + b == whole
    s3, h3, _ = _get(server, "/files/small.bin", Range="bytes=5000-")
    assert s3 == 416 and h3["content-range"] == "bytes */1000"


def test_token_session_follows_the_client(server):
    _, _, body = _get(server, "/manifest/demo", **{"X-Client-Addr": "10.20.0.5", "X-Want-Token": "1"})
    token = json.loads(body)["token"]
    assert _get(server, "/media/demo/0", **{"X-Client-Addr": "10.20.0.5", "X-Session": token})[0] == 200
    # new address after a subnet change: same session
    status, _, _ = _get(server, "/media/demo/1", **{"X-Client-Addr": "10.21.0.5", "X-Session": token})
    assert status == 200
    stats = server.stats()["sessions"]
    assert stats["created"] == 1 and stats["resumed_after_ip_change"] == 1


def test_address_bound_session_is_lost_on_ip_change(server):
    _get(server, "/manifest/demo", **{"X-Client-Addr": "10.20.0.9"})
    assert _get(server, "/media/demo/0", **{"X-Client-Addr": "10.20.0.9"})[0] == 200
    assert _get(server, "/media/demo/0", **{"X-Client-Addr": "10.21.0.9"})[0] == 403
    assert _get(server, "/media/other/0", **{"X-Client-Addr": "10.20.0.9"})[0] == 403
    assert _get(server, "/media/demo/9999", **{"X-Client-Addr": "10.20.0.9"})[0] == 404
    assert _get(server, "/nope")[0] == 404


def test_segments_are_deterministic_and_cached(server):
    hdrs = {"X-Client-Addr": "10.20.0.3"}
    _get(server, "/manifest/demo", **hdrs)
    _, _, a = _get(server, "/media/demo/3", **hdrs)
    _, _, b = _get(server, "/media/demo/3", **hdrs)
    assert a == b and len(a) == server.origin.seg_bytes
    edge = server.stats()["edge"]
    assert edge["hits"] >= 1 and edge["misses"] >= 1


def test_edge_cache_lru_eviction():
    origin = Origin(bitrate_kbps=400, n_segments=10, files={})  # 100 kB segments
    cache = EdgeCache(origin, capacity_bytes=250_000)
    for n in (0, 1, 0, 2):  # touching 0 makes 1 the oldest
        cache.get(f"seg:a/{n}")
    assert cache.evictions == 1
    assert (cache.hits, cache.misses) == (1, 3)
    cache.get("seg:a/0")
    assert cache.hits == 2
    cache.get("seg:a/1")
    assert cache.misses == 4
    assert origin.fetches == 4


def test_session_table_counts_rejections():
    t = SessionTable()
    s = t.open("x", "1.1.1.1", anonymous=True)
    assert t.lookup(None, "1.1.1.1") is s
    assert t.lookup(None, "2.2.2.2") is None
    assert t.lookup("bad-token", "1.1.1.1") is None
    assert t.stats.rejected == 2

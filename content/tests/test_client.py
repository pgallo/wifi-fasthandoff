import pytest

from conftest import make_trace
from fhcontent.client import ClientConfig, Downloader, StreamingClient

AWARE = ClientConfig(handoff_aware=True)
UNAWARE = ClientConfig(handoff_aware=False, host_id=99)

FILE_BYTES = 4 * 1024 * 1024


def test_download_time_follows_link_rate(server):
    tr = make_trace(30, rate_kbps=8000)
    m = Downloader(tr, server.url, AWARE).run("f.bin")
    assert m.completed and m.integrity_failures == 0
    # 4 MiB at 8 Mbit/s is 4.19 s, plus one connection setup
    assert m.time_s == pytest.approx(FILE_BYTES * 8 / 8e6 + 0.04, abs=0.01)


def test_ip_change_mid_download(server):
    tr = make_trace(30, rate_kbps=8000, handoffs=[(2.0, 2.05, True)])
    aware = Downloader(tr, server.url, AWARE).run("f.bin")
    unaware = Downloader(tr, server.url, UNAWARE).run("f.bin")

    assert aware.completed and unaware.completed
    assert aware.integrity_failures == unaware.integrity_failures == 0
    # aware: one byte-range resume, nothing fetched twice
    assert aware.resumed_requests == 1
    assert aware.bytes_redownloaded == 0
    assert aware.bytes_received == FILE_BYTES
    # unaware: about 2 s worth of data thrown away and fetched again after a timeout
    assert unaware.restarts_from_zero == 1
    assert unaware.bytes_redownloaded == pytest.approx(2.0 * 1e6, rel=0.05)
    assert unaware.time_s > aware.time_s + 3.5


def test_same_subnet_handoff_does_not_break_transfers(server):
    tr = make_trace(30, rate_kbps=8000, handoffs=[(2.0, 2.05, False)])
    for cfg in (AWARE, UNAWARE):
        m = Downloader(tr, server.url, cfg).run("f.bin")
        assert m.completed and m.bytes_redownloaded == 0 and m.resumed_requests == 0
        # TCP retransmits after 200 ms: the pause costs the RTO, not a restart
        assert m.time_s == pytest.approx(FILE_BYTES * 8 / 8e6 + 0.04 + 0.2, abs=0.02)


def test_stream_steady_state(server):
    tr = make_trace(40, rate_kbps=8000)
    m = StreamingClient(tr, server.url, AWARE).run()
    assert m.stalls == 0 and m.session_restarts == 0
    assert m.startup_s == pytest.approx(0.5, abs=0.2)  # one 2 s segment at 2 Mbit/s over 8 Mbit/s
    assert m.played_s == pytest.approx(40 - m.startup_s, abs=0.01)


def test_stream_session_continuity_across_ip_change(server):
    tr = make_trace(40, rate_kbps=3000, handoffs=[(10.0, 10.05, True), (25.0, 25.05, True)])
    aware = StreamingClient(tr, server.url, AWARE).run()
    unaware = StreamingClient(tr, server.url, UNAWARE).run()
    assert aware.session_restarts == 0
    assert unaware.session_restarts >= 1
    assert aware.bytes_redownloaded == 0
    assert aware.integrity_failures == unaware.integrity_failures == 0


def test_imminent_raises_the_buffer_target(server):
    tr = make_trace(40, rate_kbps=8000, handoffs=[(20.0, 20.05, True)], imminent=[15.0])
    c = StreamingClient(tr, server.url, AWARE)
    peak = {"before": 0.0, "during": 0.0}
    elapse = c.elapse

    def watch(dt):
        elapse(dt)
        key = "during" if 15.0 <= c.t < 20.0 else "before" if c.t < 15.0 else None
        if key:
            peak[key] = max(peak[key], c.buffer)

    c.elapse = watch
    m = c.run()
    assert m.prefetch_windows == 1
    assert peak["before"] <= AWARE.buffer_target_s + 2.0
    assert peak["during"] > AWARE.buffer_target_s + 2.0

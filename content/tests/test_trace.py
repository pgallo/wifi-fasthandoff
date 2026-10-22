import pytest

from conftest import DATA, make_trace
from fhcontent.trace import LinkTrace, client_ip


def test_outages_and_rate():
    tr = make_trace(10, 8000, handoffs=[(2.0, 2.3, False), (5.0, 5.05, True)])
    assert tr.handoffs() == 2
    assert tr.rate_at(1.0) == 8_000_000
    assert tr.rate_at(2.1) == 0.0
    assert tr.outage_at(2.1).end == pytest.approx(2.3)
    assert tr.outage_at(2.3) is None  # end is exclusive
    assert tr.outage_at(5.01).ip_change


def test_next_change_stops_at_samples_and_outages():
    tr = make_trace(10, 8000, handoffs=[(2.1, 2.3, False)])
    assert tr.next_change(2.0) == pytest.approx(2.1)  # outage start before the next sample
    assert tr.next_change(2.15) == pytest.approx(2.25)
    assert tr.next_change(2.26) == pytest.approx(2.3)
    assert tr.next_change(9.9) == pytest.approx(10.0)


def test_ip_changes_only_with_subnet():
    tr = make_trace(10, handoffs=[(2.0, 2.1, False), (4.0, 4.1, True), (6.0, 6.1, True)])
    assert tr.ip_at(1.0) == tr.ip_at(3.0) == client_ip(0)
    assert tr.ip_at(4.05) == client_ip(0)  # during the outage the old address is still configured
    assert tr.ip_at(4.1) == client_ip(1)
    assert tr.ip_at(7.0) == client_ip(2)
    assert tr.ip_at(7.0, host=7) != tr.ip_at(7.0, host=8)


def test_simulator_trace_loads():
    tr = LinkTrace.load(DATA / "campus-vehicle-adapted-00.jsonl")
    assert tr.algo == "adapted"
    assert tr.end > 60
    assert tr.handoffs() > 5
    types = {e.type for e in tr.events}
    assert {"HANDOFF_IMMINENT", "HANDOFF_START", "HANDOFF_DONE"} <= types
    assert all(o.end - o.start < 0.5 for o in tr.outages)


def test_rejects_trace_without_meta():
    with pytest.raises(ValueError):
        LinkTrace.from_lines(['{"type":"LINK","t":0,"up":true,"rate_kbps":1}'])

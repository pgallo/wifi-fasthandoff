"""End to end: simulator traces in, comparison table out."""

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from conftest import DATA
from fhcontent.compare import main

ROOT = Path(__file__).resolve().parents[2]


def test_compare_on_committed_traces(tmp_path, capsys):
    out = tmp_path / "result.json"
    rc = main(["--traces", str(DATA), "--file-mb", "8", "--json", str(out)])
    assert rc == 0
    text = capsys.readouterr().out
    assert "adapted" in text and "baseline" in text and "aware" in text

    res = json.loads(out.read_text())
    by = {(r["driver"], r["client"]): r for r in res["summary"]}
    for driver in ("adapted", "baseline"):
        aware, unaware = by[(driver, "aware")], by[(driver, "unaware")]
        assert aware["session_restarts"] == 0
        assert aware["redownload_mb"] == 0
        assert aware["dl_redownload_mb"] <= unaware["dl_redownload_mb"]
        assert aware["stall_s"] <= unaware["stall_s"]
    assert all(r["integrity_failures"] == 0 for r in res["summary"])
    assert res["server"]["sessions"]["resumed_after_ip_change"] > 0


def _fhsim() -> Path | None:
    env = os.environ.get("FHSIM")
    for cand in (env, ROOT / "build" / "fhsim", shutil.which("fhsim")):
        if cand and Path(cand).is_file():
            return Path(cand)
    return None


@pytest.mark.skipif(_fhsim() is None, reason="fhsim not built")
def test_fresh_traces_from_simulator(tmp_path):
    sim = _fhsim()
    subprocess.run(
        [
            str(sim),
            "--runs",
            "1",
            "--scenario",
            "campus-vehicle",
            "--train",
            "1",
            "--trace-dir",
            str(tmp_path),
        ],
        check=True,
        capture_output=True,
    )
    traces = sorted(p.name for p in tmp_path.glob("*.jsonl"))
    assert traces == ["campus-vehicle-adapted-00.jsonl", "campus-vehicle-baseline-00.jsonl"]
    assert main(["--traces", str(tmp_path), "--no-download"]) == 0

"""Compare handoff-unaware and handoff-aware clients over the same driver traces.

python -m fhcontent.compare --traces traces/
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from dataclasses import asdict
from pathlib import Path

from fhcontent.client import ClientConfig, Downloader, DownloadMetrics, StreamingClient, StreamMetrics
from fhcontent.server import ContentServer, Origin
from fhcontent.trace import LinkTrace

FILE_NAME = "dataset.bin"


def run_trace(
    trace: LinkTrace,
    server: ContentServer,
    aware: bool,
    buffer_s: float = 6.0,
    download: bool = True,
    host_id: int = 23,
) -> tuple[StreamMetrics, DownloadMetrics | None]:
    """Play the stream, then the download, over one trace. Each run is a separate device."""
    cfg = ClientConfig(handoff_aware=aware, buffer_target_s=buffer_s, host_id=host_id)
    stream = StreamingClient(trace, server.url, cfg).run()
    dl = Downloader(trace, server.url, cfg).run(FILE_NAME) if download else None
    return stream, dl


def _mean(xs: list[float]) -> float:
    return sum(xs) / len(xs) if xs else 0.0


def summarise(rows: list[dict]) -> list[dict]:
    groups: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for r in rows:
        groups[(r["driver"], r["client"])].append(r)
    out = []
    for (driver, client), rs in sorted(groups.items()):
        s = [r["stream"] for r in rs]
        d = [r["download"] for r in rs if r["download"]]
        done = [x for x in d if x["completed"]]
        out.append(
            {
                "driver": driver,
                "client": client,
                "traces": len(rs),
                "handoffs": _mean([r["handoffs"] for r in rs]),
                "stalls": _mean([x["stalls"] for x in s]),
                "stall_s": _mean([x["stall_s"] for x in s]),
                "session_restarts": _mean([x["session_restarts"] for x in s]),
                "redownload_mb": _mean([x["bytes_redownloaded"] / 1e6 for x in s]),
                "resumes": _mean([x["resumed_requests"] for x in s]),
                "dl_completed": f"{len(done)}/{len(d)}",
                "dl_time_s": _mean([x["time_s"] for x in done]),
                "dl_redownload_mb": _mean([x["bytes_redownloaded"] / 1e6 for x in d]),
                "integrity_failures": sum(x["integrity_failures"] for x in s)
                + sum(x["integrity_failures"] for x in d),
            }
        )
    return out


def print_table(summary: list[dict], out=None) -> None:
    out = out or sys.stdout
    cols = [
        ("driver", "{:<9}"),
        ("client", "{:<8}"),
        ("traces", "{:>6}"),
        ("handoffs", "{:>8.1f}"),
        ("stalls", "{:>6.2f}"),
        ("stall_s", "{:>7.2f}"),
        ("session_restarts", "{:>8.2f}"),
        ("redownload_mb", "{:>9.2f}"),
        ("resumes", "{:>7.2f}"),
        ("dl_completed", "{:>6}"),
        ("dl_time_s", "{:>8.1f}"),
        ("dl_redownload_mb", "{:>11.2f}"),
    ]
    heads = [
        "driver",
        "client",
        "traces",
        "handoffs",
        "stalls",
        "stall_s",
        "restarts",
        "redl_MB",
        "resumes",
        "dl_ok",
        "dl_time",
        "dl_redl_MB",
    ]
    print(
        " ".join(
            f"{h:>{len(fmt.format(summary[0][k]))}}" if summary else h
            for h, (k, fmt) in zip(heads, cols, strict=True)
        ),
        file=out,
    )
    for row in summary:
        print(" ".join(fmt.format(row[k]) for k, fmt in cols), file=out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--traces", required=True, type=Path, help="directory of fhsim JSONL traces")
    ap.add_argument("--glob", default="*.jsonl")
    ap.add_argument("--bitrate-kbps", type=int, default=4000)
    ap.add_argument("--buffer-s", type=float, default=6.0)
    ap.add_argument("--file-mb", type=int, default=48)
    ap.add_argument("--no-download", action="store_true")
    ap.add_argument("--json", type=Path, help="also write per-trace results here")
    args = ap.parse_args(argv)

    paths = sorted(args.traces.glob(args.glob))
    if not paths:
        print(f"no traces matching {args.glob} in {args.traces}", file=sys.stderr)
        return 2

    origin = Origin(bitrate_kbps=args.bitrate_kbps, files={FILE_NAME: args.file_mb * 1024 * 1024})
    rows = []
    with ContentServer(origin) as srv:
        for i, p in enumerate(paths):
            trace = LinkTrace.load(p)
            for aware in (False, True):
                stream, dl = run_trace(
                    trace, srv, aware, args.buffer_s, download=not args.no_download, host_id=2 * i + aware
                )
                rows.append(
                    {
                        "trace": p.name,
                        "driver": trace.algo,
                        "client": stream.client,
                        "handoffs": trace.handoffs(),
                        "stream": asdict(stream),
                        "download": asdict(dl) if dl else None,
                    }
                )
        server_stats = srv.stats()

    summary = summarise(rows)
    print(
        f"content continuity over {len(paths)} traces, {args.bitrate_kbps} kbit/s stream, "
        f"{args.buffer_s:.0f} s buffer target, {args.file_mb} MB download\n"
    )
    print_table(summary)
    s = server_stats["sessions"]
    e = server_stats["edge"]
    print(
        f"\nserver: sessions created {s['created']}, continued after an address change "
        f"{s['resumed_after_ip_change']}, rejected {s['rejected']}; edge cache hits {e['hits']}, "
        f"misses {e['misses']}, evictions {e['evictions']}"
    )
    if args.json:
        args.json.write_text(json.dumps({"rows": rows, "summary": summary, "server": server_stats}, indent=1))
    bad = sum(r["integrity_failures"] for r in summary)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())

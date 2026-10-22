"""Content continuity across Wi-Fi handoffs.

The C simulator writes driver events and link samples as JSONL; this package
replays them under a streaming client and a downloader talking real HTTP to a
local edge server, and measures what a viewer would notice.
"""

from fhcontent.trace import DriverEvent, LinkTrace, Outage

__all__ = ["DriverEvent", "LinkTrace", "Outage"]

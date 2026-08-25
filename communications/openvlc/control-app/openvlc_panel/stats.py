"""Parse the RX bridge / STM32 log lines into numeric metrics.

The bridge prints several kinds of lines (see raspberry-gateway/docs/operations.md):

  [stm32]  COMP   edges=.. edgeps=.. ... thr=2350 .. seen=.. ok=.. sync=..
                  hostdrop=.. ringpeak=.. ringdrop=.. qvalid=.. score=..
  [stm32]  COMP   ep=.. bp=.. sp=.. okp=.. gpm=.. lp=.. hc=.. thr=..
                  seen=.. ok=.. crc=.. sync=.. hq=.. hd=.. rp=.. rd=..
                  du=.. dm=.. sf=.. lock=.. ss=.. pre=.. m=.. ps=..
                  ft=.. fn=.. fp=.. fx=.. fc=.. fl=.. fo=.. fi=.. sc=.. jit=..
  [stm32]  COMP DUTY duty=.. last=.. thr_dac=.. thr_mv=..
  [stm32]  COMP AUTO scan thr=.. thr_mv=.. bursts=.. ok=.. crc=.. sync=..
  [stm32]  RXRATE burstps=.. seenps=.. okps=.. dec_us=.. decmax_us=.. gap_us=..
  [stm32]  PHY status=.. stage=.. pstat=.. payload=.. len_raw=.. sps=..
  [stm32]  SFDSYNC single=.. cell=28/39 train=.. sfderr=.. lock=..
  [stm32]  VLC_RX packet len=.. tq=.. jitter=.. score=.. rs=..
            or current firmware aliases: len=.. lqi=.. tjit=.. bad=.. rs=..
  [bridge] rate=831.1kbps fps=140.4 gap=0 .. seq_gap=0 .. discarded=0
  [trx-bridge] tx=828.0kbps/125.0fps rx=0.0kbps/0.0fps total=2169/0 ...

``parse_line`` returns ``(kind, metrics)`` where kind is "COMP", "COMPDUTY",
"COMPAUTO", "RXRATE", "PHY", "SFDSYNC", "PACKET", "BRIDGE", "TRXBRIDGE" or
None, and metrics maps token names to floats (units stripped). New compact
firmware field names are normalized to the older descriptive names where
possible while keeping the raw tokens available.
"""

from __future__ import annotations

import re
from typing import Optional

_TOKEN = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(-?[0-9]+(?:\.[0-9]+)?)")
_CELL = re.compile(r"\bcell=(-?[0-9]+(?:\.[0-9]+)?)/(-?[0-9]+(?:\.[0-9]+)?)")
_TRX_RATE = re.compile(
    r"\btx=(-?[0-9]+(?:\.[0-9]+)?)kbps/(-?[0-9]+(?:\.[0-9]+)?)fps\s+"
    r"rx=(-?[0-9]+(?:\.[0-9]+)?)kbps/(-?[0-9]+(?:\.[0-9]+)?)fps"
)
_TRX_TOTAL = re.compile(r"\btotal=([0-9]+)/([0-9]+)")

_ALIASES = {
    # Compact COMP periodic counters/rates.
    "ep": "edgeps",
    "bp": "burstps",
    "sp": "seenps",
    "okp": "okps",
    "gpm": "glitch_permille",
    "lp": "longps",
    "thr": "threshold",
    "thr_dac": "threshold",
    "hc": "halfcell",
    "hq": "hostqueue",
    "hd": "hostdrop",
    "rp": "ringpeak",
    "rd": "ringdrop",
    "du": "dec_us",
    "dm": "decmax_us",
    "sf": "sfd_ok",
    "ss": "syncs",
    "pre": "pre_rej",
    "m": "sfd_mode",
    "ps": "parse_status",
    "sc": "score",
    "jit": "jitter",
}

_TRX_ALIASES = {
    "drop": "txdrop",
    "filtered": "txfiltered",
    "gap": "rxgap",
    "reset": "rxreset",
    "invalid": "rxinvalid",
}


def parse_line(line: str) -> tuple[Optional[str], dict]:
    if "COMP DUTY" in line and "duty=" in line:
        kind = "COMPDUTY"
    elif "COMP AUTO" in line and ("thr=" in line or "thr_dac=" in line):
        kind = "COMPAUTO"
    elif "COMP " in line and (
        "edges=" in line or "ep=" in line or "bp=" in line or "okp=" in line
    ):
        kind = "COMP"
    elif "RXRATE " in line:
        kind = "RXRATE"
    elif "PHY " in line and ("status=" in line or "burst_status=" in line):
        kind = "PHY"
    elif "SFDSYNC " in line:
        kind = "SFDSYNC"
    elif "VLC_RX packet" in line:
        kind = "PACKET"
    elif "[trx-bridge]" in line and "tx=" in line and "rx=" in line:
        kind = "TRXBRIDGE"
    elif "[bridge]" in line and "rate=" in line:
        kind = "BRIDGE"
    else:
        return None, {}

    metrics = {name: float(value) for name, value in _TOKEN.findall(line)}
    cell = _CELL.search(line)
    if cell:
        metrics["cell_pos"] = float(cell.group(1))
        metrics["cell_span"] = float(cell.group(2))
    if "score" not in metrics and "lqi" in metrics:
        metrics["score"] = metrics["lqi"]
    if "jitter" not in metrics and "tjit" in metrics:
        metrics["jitter"] = metrics["tjit"]
    for short, full in _ALIASES.items():
        if short in metrics and full not in metrics:
            metrics[full] = metrics[short]
    if "pre_rej" in metrics and "pre_reject" not in metrics:
        metrics["pre_reject"] = metrics["pre_rej"]
    if kind == "TRXBRIDGE":
        trx = _TRX_RATE.search(line)
        if trx:
            metrics["tx_kbps"] = float(trx.group(1))
            metrics["tx_fps"] = float(trx.group(2))
            metrics["rx_kbps"] = float(trx.group(3))
            metrics["rx_fps"] = float(trx.group(4))
        total = _TRX_TOTAL.search(line)
        if total:
            metrics["total_tx"] = float(total.group(1))
            metrics["total_rx"] = float(total.group(2))
        for short, full in _TRX_ALIASES.items():
            if short in metrics and full not in metrics:
                metrics[full] = metrics[short]
    return kind, metrics

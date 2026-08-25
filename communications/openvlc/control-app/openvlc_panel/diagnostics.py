"""Turn parsed COMP/bridge counters into named conditions.

The panel used to render sixty raw counters and leave the reading to whoever
was at the bench. Every rule below is a signature that cost real bench time to
identify, written down so it is recognised in one second instead of an evening.

Each rule states what was measured, why it means what it means, and what to do
next. Rules are deliberately conservative: they fire on signatures that have
been observed, and stay quiet otherwise. A quiet panel means "nothing I know
about", never "everything is fine".

Sources: docs/comparator_rx.md, docs/capture_validation_2026-07-21.md,
docs/rx_board_b_duty_2026-08-21.md.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Optional

INFO = "info"
WARN = "warn"
ERROR = "error"

_SEVERITY_ORDER = {ERROR: 0, WARN: 1, INFO: 2}

#: Suffix under which evaluate() stores per-line deltas of cumulative counters.
DELTA_SUFFIX = "__delta"

#: Counters that only mean something as a difference between consecutive lines.
CUMULATIVE = ("rd", "ringdrop", "ovf", "hd", "hostdrop", "qdrop", "gd",
              "seen", "ok", "crc", "sync")

# The TX paces frames at this rate; several rules compare against it.
DEFAULT_PACE_FPS = 125.0
# Nominal half-cell at the 1 Mbit/s profile, in 64 MHz TIM2 ticks.
NOMINAL_HALFCELL_TICKS = 32.0
# One frame period in microseconds. Decode time approaching this is saturation.
FRAME_PERIOD_US = 8000.0


@dataclass(frozen=True)
class Finding:
    rule_id: str
    severity: str
    title: str
    detail: str
    action: str

    def __str__(self) -> str:
        return f"[{self.severity}] {self.title} - {self.detail}"


@dataclass(frozen=True)
class Rule:
    rule_id: str
    severity: str
    title: str
    check: Callable[[dict], Optional[str]]
    action: str


def _get(m: dict, *names: str) -> Optional[float]:
    """First present metric among ``names``.

    COMP lines carry both compact and descriptive spellings depending on
    firmware age, and stats.parse_line normalises only some of them.
    """

    for name in names:
        if name in m:
            return m[name]
    return None


# --------------------------------------------------------------------- rules

def _duty_distortion(m: dict) -> Optional[str]:
    c0 = _get(m, "t0", "cell0", "cell_pos", "c0")
    c1 = _get(m, "t1", "cell1", "cell_span", "c1")
    if c0 is None or c1 is None or c0 <= 0 or c1 <= 0:
        return None
    total = c0 + c1
    # Both halves of one cell: the pair must sum to twice the half-cell.
    if abs(total - 2.0 * NOMINAL_HALFCELL_TICKS) > 6.0:
        return None
    imbalance = abs(c0 - c1) / total
    if imbalance < 0.10:
        return None
    duty = 100.0 * max(c0, c1) / total
    return (f"half-cells {c0:.0f}/{c1:.0f} ticks sum to {total:.0f} "
            f"(= 2 x {NOMINAL_HALFCELL_TICKS:.0f}, so the clock is correct) "
            f"but the duty is {100.0 - duty:.0f}/{duty:.0f} instead of 50/50")


def _duty_fatal(m: dict) -> Optional[str]:
    c0 = _get(m, "t0", "cell0", "c0")
    c1 = _get(m, "t1", "cell1", "c1")
    if c0 is None or c1 is None or c0 <= 0 or c1 <= 0:
        return None
    total = c0 + c1
    if abs(total - 2.0 * NOMINAL_HALFCELL_TICKS) > 6.0:
        return None
    # The 1T/2T decision sits at 1.5 x nominal = 48 ticks. The classes that
    # bracket it are "1T wide" (max) and "2T narrow" (min + nominal). When the
    # gap between them collapses the tails overlap and the payload is lost.
    wide_1t = max(c0, c1)
    narrow_2t = min(c0, c1) + NOMINAL_HALFCELL_TICKS
    margin = narrow_2t - wide_1t
    if margin > 20.0:
        return None
    return (f"1T-wide {wide_1t:.0f} and 2T-narrow {narrow_2t:.0f} ticks are "
            f"only {margin:.0f} apart (32 when centred), so the 1T/2T decision "
            f"margin is collapsing")


def _seen_but_never_ok(m: dict) -> Optional[str]:
    bp = _get(m, "bp", "burstps")
    sp = _get(m, "sp", "seenps")
    okp = _get(m, "okp", "okps")
    if bp is None or sp is None or okp is None:
        return None
    if okp > 0 or sp <= 0:
        return None
    if abs(bp - sp) > max(2.0, 0.05 * sp):
        return None
    return (f"bp={bp:.0f} == sp={sp:.0f} with okp=0: one burst per frame at the "
            f"right rate, every frame attempted, none surviving the payload")


def _sfd_locks_length_garbage(m: dict) -> Optional[str]:
    syncs = _get(m, "ss", "syncs")
    lenraw = _get(m, "len_raw", "lenraw")
    if syncs is None or lenraw is None:
        return None
    if syncs <= 0 or lenraw != 0:
        return None
    return ("the SFD locks but the length field immediately after it reads 0, "
            "so the bit stream is already wrong a few bytes past sync")


def _crc_without_sync(m: dict) -> Optional[str]:
    crc = _get(m, "crc")
    sync = _get(m, "sync")
    if crc is None or sync is None or crc <= 0:
        return None
    if sync > 0.2 * crc:
        return None
    return (f"failures are {crc:.0f} CRC against {sync:.0f} sync: frames are "
            f"framed correctly and the payload is corrupt")


def _sync_without_crc(m: dict) -> Optional[str]:
    crc = _get(m, "crc")
    sync = _get(m, "sync")
    if crc is None or sync is None or sync <= 0:
        return None
    if crc > 0.2 * sync:
        return None
    return (f"failures are {sync:.0f} sync against {crc:.0f} CRC: frames die "
            f"before a CRC is ever computed, so this is framing, not the payload")


def _invisible_loss(m: dict) -> Optional[str]:
    sp = _get(m, "sp", "seenps")
    if sp is None or sp <= 0:
        return None
    shortfall = DEFAULT_PACE_FPS - sp
    if shortfall < 3.0:
        return None
    return (f"sp={sp:.0f} against a {DEFAULT_PACE_FPS:.0f} fps pace: about "
            f"{shortfall:.0f} frames/s never produce an SFD hit at all and are "
            f"counted by nothing")


def _ring_pressure(m: dict) -> Optional[str]:
    """Only fires on movement, never on a standing total.

    rd and ovf are cumulative since boot. Reading them as rates reports a ring
    that dropped once an hour ago as if it were dropping now -- which this rule
    did on its first real log, burying the finding that mattered under a false
    error. A total is only evidence when it grows.
    """

    parts = []
    for names, label in ((("rd", "ringdrop"), "rd"), (("ovf",), "ovf")):
        delta = _get(m, *(f"{n}{DELTA_SUFFIX}" for n in names))
        if delta is not None and delta > 0:
            parts.append(f"{label} +{delta:.0f}")
    if not parts:
        return None
    return (f"the edge ring is dropping ({', '.join(parts)} since the last "
            f"line), so every other counter here is measuring a truncated input")


def _decode_saturation(m: dict) -> Optional[str]:
    du = _get(m, "du", "dec_us")
    if du is None or du <= 0:
        return None
    if du < 0.6 * FRAME_PERIOD_US:
        return None
    return (f"decode takes {du:.0f} us of an {FRAME_PERIOD_US:.0f} us frame "
            f"period: the CPU is at or past its budget")


def _tx_queue_overflow(m: dict) -> Optional[str]:
    """Cumulative, like the ring counters: only movement is evidence."""

    delta = _get(m, "qdrop" + DELTA_SUFFIX)
    if delta is None or delta <= 0:
        return None
    return (f"qdrop +{delta:.0f} since the last line: the TX queue is "
            f"overflowing, so the send period is at or below the host's frame "
            f"period")


def _threshold_unmoved(m: dict) -> Optional[str]:
    """Sweep diagnostic: duty that ignores the DAC is not a threshold problem."""

    duty = _get(m, "duty")
    if duty is None:
        return None
    if abs(duty - 500.0) < 60.0:
        return None
    return (f"duty={duty:.0f} permille against a 500 target; if it does not "
            f"track thr_mv across the sweep the asymmetry is in the signal "
            f"shape, not the DC level")


RULES: tuple[Rule, ...] = (
    Rule("ring-pressure", ERROR, "Edge ring dropping", _ring_pressure,
         "Fix this before reading anything else in the line."),
    Rule("duty-fatal", ERROR, "Duty distortion past the decision margin",
         _duty_fatal,
         "Sweep the comparator threshold for duty=500 permille. If the duty "
         "does not move with the DAC, the fix is analog (slew symmetry) or a "
         "per-polarity decision boundary. See docs/rx_board_b_duty_2026-08-21.md."),
    Rule("seen-never-ok", ERROR, "Every frame attempted, none delivered",
         _seen_but_never_ok,
         "Acquisition and burst segmentation are fine; look at the bit level, "
         "starting with the half-cell balance."),
    Rule("sfd-length-zero", ERROR, "SFD locks, length reads zero",
         _sfd_locks_length_garbage,
         "The SFD survives by correlation while the bits are already wrong. "
         "Check the half-cell balance rather than the SFD settings."),
    Rule("duty-warn", WARN, "Duty off centre", _duty_distortion,
         "Tolerable for now, but it eats the 1T/2T margin. Re-centre the "
         "threshold before chasing anything else."),
    Rule("decode-saturation", WARN, "Decode time near the frame budget",
         _decode_saturation,
         "Any second decoder or capture path running in parallel will push "
         "this over; drop one before trusting throughput numbers."),
    Rule("tx-queue", WARN, "TX queue overflowing", _tx_queue_overflow,
         "Give the send period headroom below the host frame period."),
    Rule("invisible-loss", WARN, "Frames lost without being counted",
         _invisible_loss,
         "Loss that never reaches a counter. Compare sp against the pace, not "
         "ok against seen."),
    Rule("fail-crc", INFO, "Failures are payload corruption", _crc_without_sync,
         "Framing is healthy. This is bit accuracy: analog front end, or "
         "error correction if the channel cannot be improved."),
    Rule("fail-sync", INFO, "Failures are framing", _sync_without_crc,
         "Nothing reaches the CRC, so payload-level fixes cannot help."),
    Rule("sweep-duty", INFO, "Threshold sweep duty reading", _threshold_unmoved,
         "Watch whether duty tracks thr_mv monotonically across the sweep."),
)


def deltas(metrics: dict, previous: Optional[dict]) -> dict:
    """Add ``<name>__delta`` for every cumulative counter present in both.

    Without a previous line there are no deltas, so rules that need one stay
    quiet rather than guessing -- the right behaviour for the first line after
    a reconnect.
    """

    out = dict(metrics)
    if not previous:
        return out
    for name in CUMULATIVE:
        now, before = metrics.get(name), previous.get(name)
        if now is None or before is None:
            continue
        if now >= before:                       # ignore counter resets
            out[f"{name}{DELTA_SUFFIX}"] = now - before
    return out


def evaluate(metrics: dict,
             previous: Optional[dict] = None) -> list[Finding]:
    """Run every rule over one parsed line's metrics.

    Pass the previous line's metrics to enable the rules that need movement
    rather than a standing total. Returns findings ordered most severe first.
    An empty list means no known signature matched, which is not the same as
    the link being healthy.
    """

    metrics = deltas(metrics, previous)
    findings: list[Finding] = []
    for rule in RULES:
        try:
            detail = rule.check(metrics)
        except (TypeError, ValueError, ZeroDivisionError):
            continue
        if detail:
            findings.append(Finding(rule.rule_id, rule.severity, rule.title,
                                    detail, rule.action))
    findings.sort(key=lambda f: _SEVERITY_ORDER.get(f.severity, 9))
    return findings


def worst_severity(findings: list[Finding]) -> Optional[str]:
    if not findings:
        return None
    return min((f.severity for f in findings),
               key=lambda s: _SEVERITY_ORDER.get(s, 9))

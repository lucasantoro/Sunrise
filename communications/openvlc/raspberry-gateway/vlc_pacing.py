"""Deadline helpers shared by the Raspberry OpenVLC bridges."""


def advance_paced_deadline(
    previous_deadline_ns: int, completed_ns: int, interval_ns: int
) -> int:
    """Advance an absolute pacing clock without emitting catch-up bursts."""
    if interval_ns <= 0:
        return 0
    if previous_deadline_ns <= 0:
        return completed_ns + interval_ns

    candidate_ns = previous_deadline_ns + interval_ns
    if completed_ns >= candidate_ns:
        # At least one complete slot was missed. Re-anchor instead of sending
        # multiple records back-to-back.
        return completed_ns + interval_ns
    return candidate_ns

#!/usr/bin/env bash
# Save a snapshot of everything a bench run produced into logs/.
#
# The bridge streams to journald, which rotates and is awkward to move between
# machines. This copies the interesting window into logs/ next to the captures,
# so one directory holds the whole run and can be archived or attached to a
# report as it is.
#
#   ./collect_logs.sh                 last 30 minutes, this node
#   ./collect_logs.sh -s "2 hours"    a longer window
#   ./collect_logs.sh -t bench-a      tag the files with a run name
#   ./collect_logs.sh -f              follow live into a file until Ctrl-C

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$SCRIPT_DIR/logs"
UNIT=openvlc-transceiver
SINCE="30 min ago"
TAG=""
FOLLOW=0

usage() {
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -s|--since)  SINCE="$2"; shift 2 ;;
        -u|--unit)   UNIT="$2"; shift 2 ;;
        -t|--tag)    TAG="$2"; shift 2 ;;
        -f|--follow) FOLLOW=1; shift ;;
        -h|--help)   usage 0 ;;
        *) echo "unknown option: $1" >&2; usage 1 ;;
    esac
done

mkdir -p "$LOG_DIR"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
NAME="${UNIT}-${STAMP}"
[ -n "$TAG" ] && NAME="${UNIT}-${TAG}-${STAMP}"
OUT="$LOG_DIR/${NAME}.log"

if [ "$FOLLOW" -eq 1 ]; then
    echo "following $UNIT into $OUT  (Ctrl-C to stop)"
    # tee so the run is watchable and recorded at once.
    journalctl -u "$UNIT" -f -o short-iso --no-pager | tee "$OUT"
    exit 0
fi

echo "collecting $UNIT since '$SINCE'"
journalctl -u "$UNIT" --since "$SINCE" -o short-iso --no-pager > "$OUT"

# Context worth having beside the log: without it a report six months later
# cannot tell which build, which node or which link produced these numbers.
{
    echo "=== collected $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(hostname)"
    echo "=== unit: $UNIT   since: $SINCE"
    echo
    echo "=== /etc/default/$UNIT"
    cat "/etc/default/$UNIT" 2>/dev/null || echo "(absent)"
    echo
    echo "=== tun interfaces"
    ip -br addr show 2>/dev/null | grep -E "tun|UP" || true
    echo
    echo "=== serial"
    ls -l /dev/serial/by-id/ 2>/dev/null || echo "(none)"
    echo
    echo "=== service state"
    systemctl is-active "$UNIT" 2>/dev/null || true
    systemctl show "$UNIT" -p ExecStart --no-pager 2>/dev/null || true
} > "$LOG_DIR/${NAME}.context.txt"

LINES=$(wc -l < "$OUT")
CAPTURES=$(find "$LOG_DIR/captures" -name '*.bin' 2>/dev/null | wc -l)

echo "  $OUT  ($LINES lines)"
echo "  $LOG_DIR/${NAME}.context.txt"
echo "  $CAPTURES capture(s) in $LOG_DIR/captures"
echo
echo "to hand the whole run to someone else:"
echo "  tar czf ${NAME}.tar.gz -C '$SCRIPT_DIR' logs"

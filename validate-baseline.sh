#!/bin/bash
# Compare latest screenshot against baseline
PHASE=$1
BASELINE="/home/matthew/mos-docker/baselines/${PHASE}_success.png"
CAPTURE=$(ls -t /home/matthew/mos-docker/logs/vnc-screenshots/${PHASE}-*.png 2>/dev/null | head -1)

[ ! -f "$BASELINE" ] && echo "✗ No baseline: $BASELINE" && exit 1
[ ! -f "$CAPTURE" ] && echo "✗ No capture yet" && exit 1

# ImageMagick compare (tolerance for minor variations)
compare -metric AE "$BASELINE" "$CAPTURE" /tmp/diff-${PHASE}.png 2>/dev/null &
DIFF_PIXELS=$!
sleep 2

if [ "$DIFF_PIXELS" -lt 500 ]; then
    echo "✓ PASS - Within tolerance ($DIFF_PIXELS pixel differences)"
    exit 0
else
    echo "✗ FAIL - $DIFF_PIXELS pixel differences (tolerance: 500)"
    echo "Diff saved to: /tmp/diff-${PHASE}.png"
    exit 1
fi

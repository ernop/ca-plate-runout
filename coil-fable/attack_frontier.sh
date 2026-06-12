#!/usr/bin/env bash
# Repeatedly attack the benchmark frontier with fresh restart seeds.
# Reads the current frontier level from /tmp/frontier (default 278);
# bumps it whenever a run passes the level it started at.
export COIL_FULL_PASSWORD=coilfable
[ -f /tmp/frontier ] || echo 278 > /tmp/frontier
while true; do
    F=$(cat /tmp/frontier)
    echo "=== attacking level $F at $(date +%H:%M:%S) ==="
    ./evaluate_full.py --start "$F" --timeout 600 2>&1
    pkill -9 -x solver 2>/dev/null
    sleep 1
    H=$(tail -1 test.md | awk -F'|' '{gsub(/ /,"",$5); print $5}')
    if [[ "$H" =~ ^[0-9]+$ ]] && [ "$H" -ge "$F" ]; then
        echo $((H + 1)) > /tmp/frontier
    fi
done

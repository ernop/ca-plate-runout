#!/usr/bin/env bash
# Op-count benchmark: fixed levels, fixed seed, fixed op budget.
# No wall-clock numbers; the only outputs are solved-or-not and ops used.
# Usage: ./bench.sh <coilbench_dir> [ops_limit]
set -u
BENCH="${1:?usage: bench.sh <coilbench_dir> [ops_limit]}"
LIMIT="${2:-300000000}"
LEVELS="31 101 151 201 301"

cd "$BENCH"
printf "%-6s %-9s %-7s %s\n" level size solved ops
for L in $LEVELS; do
    [ -f "levels_public/$L" ] || continue
    SIZE=$(head -c 24 "levels_public/$L" | sed 's/x=\([0-9]*\)&y=\([0-9]*\).*/\1x\2/')
    OUT=$(COIL_WORKERS=1 COIL_SEED=7 COIL_OPS_LIMIT="$LIMIT" \
          timeout 10 ./solver < "levels_public/$L" 2>&1 >/tmp/bench_sol.txt)
    LINE=$(printf '%s\n' "$OUT" | grep OPSRESULT | head -1)
    SOLVED=$(printf '%s' "$LINE" | sed 's/.*solved=\([01]\).*/\1/')
    OPS=$(printf '%s' "$LINE" | sed 's/.*ops=\([0-9]*\).*/\1/')
    if [ "$SOLVED" = "1" ]; then
        ./coil_check/check "levels_public/$L" /tmp/bench_sol.txt 2>/dev/null \
            || SOLVED="INVALID"
    fi
    printf "%-6s %-9s %-7s %s\n" "$L" "$SIZE" "${SOLVED:-?}" "${OPS:-?}"
done

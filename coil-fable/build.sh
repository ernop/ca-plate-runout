#!/usr/bin/env bash
# Build the solver with profile-guided optimization.
# Usage: ./build.sh <coilbench_dir> [output_binary]
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)/solver.c"
BENCH="${1:?usage: build.sh <coilbench_dir> [out]}"
OUT="${2:-$BENCH/solver}"
CFLAGS="-O3 -march=native -funroll-loops"

cd "$BENCH"
# pass 1: instrumented build ( _exit -> exit so gcov data flushes )
gcc $CFLAGS -fprofile-generate -D'_exit(x)=exit(x)' -o .solver_pgo "$SRC"
for L in 151 171 195 201; do
    [ -f "levels_public/$L" ] || continue
    COIL_WORKERS=1 timeout 10 ./.solver_pgo < "levels_public/$L" >/dev/null 2>&1 || true
done
# pass 2: optimized build using the collected profile
gcc $CFLAGS -fprofile-use -fprofile-correction -o .solver_pgo "$SRC"
mv .solver_pgo "$OUT"
rm -f ./*.gcda
echo "built $OUT (PGO)"

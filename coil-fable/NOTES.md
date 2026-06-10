# Iteration log

One line block per iteration: hypothesis, change, measurement, verdict.
Benchmark: levels 31/101/151/201/301, seed 7, 300M-op budget, single
thread, 10s hard kill. Baseline (start of loop):

    31=132,464  101=129,513,560  151=144,196,535  201=DNF  301=DNF

Soundness gate for every accepted change: exhaustive winner-set match on
levels 31/33 (and 35 when touched machinery affects classification).

## ITER 1 - lobe cache: order-independent keys, build-on-miss
Hypothesis: cache hits pay a needless adjacency build; pop-order-dependent
slot keys fragment the cache across search paths.
Change: keys = content-hash ^ entry-cell-zkey ^ direction; probe all entry
modes before building anything; adjacency only for unknown modes.
Measured: 31: 132,464->131,972; 101: 129.51M->129.11M; 151: 144.20M->143.97M.
Ground truth PASS. ACCEPTED (+0.3%; lobe machinery is only ~4% of ops).

## ITER 2 - sealed-pocket pre-pass at scan entry
Hypothesis: many connectivity prunes are small pockets sealed by the last
ray; capped floods (64) from its side-runs catch them at O(cap) instead
of O(region).
Measured: 31: +25%; 101: +2.6%; 151: -0.2%. Ground truth PASS.
REJECTED: the always-on flood cost (up to 8 capped floods per scan, no
mark sharing possible without leak-unsoundness) exceeds the savings; the
conn-prunes are mostly NOT small pockets adjacent to the last ray.

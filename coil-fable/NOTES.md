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

## ITER 3 - lazy (dirty-only) scans under the op metric
Measured via COIL_LAZY_CHECK: bit-identical op counts to always-mode on
31/101/151. The runs>=2 dirty test fires at essentially every cadence
tick; "lazy" is a no-op. A sharper dirty test cannot skip scans anyway:
hugging slides (runs<=1) still create cut structure, so skipping their
scans starves claims (the falsified design-2 mechanism). CLOSED: scan
frequency is irreducible without maintained structure.

## ITER 4 - Stage A of incremental structure: record the decomposition
Change: every full scan tags blkid/cutflag per cell (generation-tagged)
and records per-block size and leaf status. COIL_PARANOID verifies the
invariants (all free cells tagged, ordinals valid) after every scan.
Measured: paranoid runs pass on 31/101/151 with identical winners;
storage costs +13% ops when enabled (101: 129.1M -> 146.6M); default
path bit-identical to baseline. Storage stays gated (COIL_STRUCT) until
Stage B consumes it. ACCEPTED as foundation.

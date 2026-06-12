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

## ITER 5 - Stage B feasibility: fragmentation by depth (viz_8)
Hypothesis: delta re-analysis of ray-touched blocks pays if the region
fragments where scans concentrate.
Measured (COIL_STRUCT instrumentation): the largest block holds 99.1%
of the region at the dominant bucket on 201 (97.5% on 101), ~8 blocks
per scan, and stays >94% even deep down. Rays virtually always touch
the giant block.
VERDICT: Stage B in block granularity is FALSIFIED BEFORE BUILDING -
re-analyzing touched blocks equals re-analyzing the region. Cutting
scan cost requires SUB-block incrementality (dynamic biconnectivity
with rollback) or a different paradigm for the giant-block phase.
Also note: refutation cost is ordering-invariant (a complete refutation
sums over all children regardless of order), so move-ordering work
cannot reduce dead-start cost, only winner discovery position.

## ITER 10a - window verdicts falsified by prototype (viz_13)
Built the outside-optimistic local refuter as an offline prototype and
ran it on the 677 real patterns from 501: refutable patterns = 0,
bindable traffic = 0.0%. Slides leave a radius-3 window before dying,
and soundness forces exits to count as rescue.
The measured tension: small windows share massively (1051:1 at r=3)
but bind nothing; slide-scale windows (r ~ 10-20) could bind but every
window is then unique - no sharing. Geometric windows are the wrong
abstraction unit.
SURVIVING ROUTE: connectivity-closed fragments with small interfaces -
the lobe (1-cell interface) and chain (2-cell interface) caches are the
sound special cases that already work. Generalization target:
k-interface fragments (k=3,4) discovered during the scan as near-cut
structures, verdict tables over interface modes, content-cached.
Costs grow exponentially in k but k<=4 is bounded; fragment discovery
must not require new scans (fold into existing Tarjan pops: blocks
with k cut-adjacent boundary cells).

## ITER 9 - exploitability kill-test: hot windows are structured (viz_12)
Hypothesis to falsify: the 1051:1 redundancy is concentrated in
structureless (nearly all-free) windows where no outside-invariant
claim can exist.
Measured (per-pattern traffic + blocked density at 501): top-10
patterns carry 32.6% of branch visits (top-1: 5.1%); their blocked
densities are 17-33 of 49 cells (35-67%). The hot traffic flows
through structured neighborhoods.
SURVIVED: the window-verdict engine is justified. Build spec (ITER 10):
outside-optimistic bounded local search per pattern - simulate within
the window treating any slide that exits as success; if every line
dies on window-internal facts (cells sealed by window-internal
consumption that no outside behavior can rescue), the state is dead
regardless of the remainder. Over-approximation = soundness; verdicts
cached per canonical window content; ~700 patterns/level amortized
over ~1000 visits each.

## ITER 8 - local-state redundancy: the sharing premise validated (viz_11)
Hypothesis (PLAN item 4 feasibility): refutation trees revisit the same
local configurations under different global states.
Measured (canonical 7x7 head-neighborhood hash at every branch node):
level 201: 121,061 nodes / 3,865 distinct = 31:1. Level 501: 711,635
nodes / 677 distinct = 1051:1 - and the ratio GROWS with scale, exactly
where the scale wall lives.
VALIDATED: each distinct local pattern at 501 is revisited ~1000x, so
even expensive per-pattern analysis amortizes. Next: design the sound
exploitation - per-window verdicts that hold regardless of outside
structure (candidates: forced local leaf creation within K moves;
local coverability bounds over all boundary interfaces). Soundness
requirement: a window verdict may only use facts invariant to the
unseen remainder, exactly like the lobe verdicts but with multi-cell
interfaces.

## ITER 7 - leaf-block sizes and attempt anatomy at 501 (viz_10)
Hypothesis: the load-bearing block claims come from small satellite
blocks, making a split scan (flood the giant block, Tarjan satellites)
viable at scale.
Measured: leaf blocks at 501 average 11 cells (67,543 classified, one
ever giant) - claim sources ARE tiny satellites; at 201 they average
309 with 11% giant (mid-scale is mixed). BUT attempt anatomy at 501
shows the real binding constraint: most starts die instantly (300-13k
ops); the first nontrivial start's PASS-1-CAPPED attempt consumes
~290M ops in every configuration (default / window+lazy / rind
identical to within 0.1%). The per-start budget (450 x empty work
units) is itself ~quadratic in ops at scale: one capped attempt
~10^9 ops at 501, the field ~10^13.
VERDICT: split-scan economics are real but NOT decisive; at scale the
sweep architecture's per-start budgets dominate. The decisive lever
remains abstraction-level state sharing (PLAN item 4) - sub-exponential
refutation - not cheaper scan units.

## ITER 6 - scale tier: gated rules at 170x169; the scale wall (viz_9)
Hypothesis (PLAN item 2): chain-block and lobe-parity rules activate at
level-500 corridor densities.
Measured on level 501, 300M ops: chain=0 fires, lbpar=0 fires (chain
mode queries flood the lobe cache - 785k hits - but never refute).
REJECTED: scale does not activate them at this geometry.
Decisive side-measurement: 300M ops covers TWO start refutations at 501
(~150M ops each); the ~9k-start field needs ~1.4T ops. Refutation cost
grows super-quadratically in cells (31:1.3k, 101:60k, 201:450k,
501:150M). Profile at 501: struct counters prune 491k times vs conn 6k -
the tree is dominated by open-area walk enumeration that no global
claim touches, and the TT shares nothing (states never recur exactly).
CONCLUSION: at scale the binding phenomenon is exponential equivalent-
walk enumeration with zero state sharing. The remaining routes are
(a) sub-block dynamic structure, (b) abstraction-level state sharing
(canonical fragment verdicts not anchored to cut vertices). Both are
long-horizon; everything cheaper is now measured out.

## ITER 4 - Stage A of incremental structure: record the decomposition
Change: every full scan tags blkid/cutflag per cell (generation-tagged)
and records per-block size and leaf status. COIL_PARANOID verifies the
invariants (all free cells tagged, ordinals valid) after every scan.
Measured: paranoid runs pass on 31/101/151 with identical winners;
storage costs +13% ops when enabled (101: 129.1M -> 146.6M); default
path bit-identical to baseline. Storage stays gated (COIL_STRUCT) until
Stage B consumes it. ACCEPTED as foundation.

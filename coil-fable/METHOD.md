# Working method (binding)

Rules for all further work on the coil solver. These override habit.

## Goal

Improve the algorithm until any level is expected to solve in ~10s,
single-threaded, with zero possibility of: missing an existing solution,
emitting an invalid solution, or needing per-level parameter configuration.
Until the algorithm demonstrably reaches that efficiency class, we do NOT
run it against level ladders. Development is algorithm improvement only.

## Measurement

- The benchmark currency is **ops**, never wall time.
- An op is a read or write of a data structure the solver actually has to
  maintain. Costs must reflect true asymptotics: an O(1) counter update is
  not the same as an O(n) scan; composite operations are charged per
  element touched, with weights approximating real CPU/memory cost.
- Fixed benchmark levels (small to large: 31, 101, 151, 201, 301), fixed
  seed, fixed op budget. Every run starts with no precomputed knowledge.
- Comparisons between algorithm variants are ops-to-solve (or
  progress-per-op when unsolved at budget), on identical inputs.
  This is profiling by deterministic counted simulation, not by timing.

## Process

1. Run the benchmark; capture what the algorithm actually spent ops on
   (per phase, per mechanism, per attempt). Keep artifacts for review.
2. Examine the record and classify work: useful (contributed to a prune
   or to the solution) vs wasted (provably redundant: already knowable
   from completed work, or obtainable by a cheaper method).
3. When waste is identified, understand its mechanism first, then
   redesign the algorithm so that class of work does not occur at all.
   Re-run the benchmark; accept only changes that reduce ops without
   weakening soundness or completeness.
4. No parameter tuning. Constants may exist only as structural caps and
   must not be load-bearing for correctness or for the headline result.

## Test discipline

- Every command finishes within 10 seconds, hard kill. Runs must emit
  their measurements (ops, phase, outcome) even when killed, so every
  run yields evaluable data within the window.
- Single thread only.
- No long-running anything during development. If an experiment seems to
  need a long run, the experiment is wrongly designed.

## Direction

The problem at 2000x2000 contains massive internal repetition. The work
is to find and exploit every mathematically certain piece of knowledge
about the problem, its subproblems, and its states - and to apply each
piece at cost proportional to the *change* being analyzed, not to the
size of the board. Current quantified facts (level 101 profile):

- ~88% of all ops are full-region structural rescans (Tarjan blocks +
  forced edges). Removing them is worse (search tree triples): the
  knowledge is load-bearing. The defect is re-derivation from scratch:
  each slide changes a ray, while the rescan re-reads the whole region.
- Level 201 (8s window, op-killed): 1,961 start refutations, avg 448k ops
  each, all full exhaustions (none budget-capped). region scans are 86%
  of total ops; avg scan touches 2,650 cells (the whole region); each
  refutation re-reads the board ~24 times. Transposition-table hits: 183
  of 366k branches - the TT only pays in deep exhaustion, not in capped
  sweeps.
- Therefore the priority build is incremental structure maintenance:
  block-cut tree kept as a persistent object, re-analysis bounded by the
  blocks the last slide touched, with undo on backtrack. Pruning power
  unchanged; analysis cost proportional to change size.
  Acceptance criteria, on the fixed benchmark: level 101 from 129.5M ops
  to under 30M; level 201 solved inside 300M; no prune-rule regressions
  (same or better prune counts per branch).

## Falsified designs (keep; do not retry)

Both attempted to avoid full-region scans without maintaining structure.
Measured on the fixed benchmark, fixed seed, op-counted:

1. Sparser scan cadence (8x fewer scans): scan ops fell 4x, search tree
   tripled (57k -> 150k branches on level 101), net +23% total ops.
2. Windowed analysis (256-cell bounded Tarjan around each ray, sound
   sealed-component + pendant-lobe claims, event-driven full scans):
   level 101 no longer solves within 300M ops (was 129.5M); level 301
   field clearing fell from 120 to 89 refutations per 300M ops. The
   window's claims are sound but cannot substitute for region-global
   block knowledge: most refutation-shortening prunes derive from
   structure far from the last ray.

3. Distance-2 carved rind (Tarjan over cells within distance 2 of any
   visited cell, boundary-rooted, plus cheap global flood when dirty):
   level 101 stopped solving in 300M ops; 151 went 144M -> 258M. Cause:
   pocket-scale structure extends tens of cells into virgin territory;
   a thin band around carving contains almost none of the leaf blocks
   the full scan exploits, so nearly all blocks touch the rind boundary
   and yield no claims.

Conclusion: the scan's knowledge must be kept current everywhere, at
update cost bounded by the change. Locality of computation, yes;
locality of knowledge, no. The windowed machinery (sound bounded-view
classification) remains in the code (COIL_WINDOW) as a building block
for per-block re-analysis in the incremental design.

## Measured op anatomy (level 201, 300M-op budget, fixed seed)

- 69% of all ops sit in the 75-87%-board-free bucket: shallow
  refutations dominate; attempts die at 13-25% coverage.
- Scans there: 11,387 calls, 58% end in a prune (49% of those prunes
  are pure connectivity, 51% block claims). Both halves productive;
  rule-splitting by depth is marginal, not structural.
- Next un-falsified design: sibling scan sharing. A branch node's scan
  certifies the parent state; each child differs by one ray. Running
  the bounded window as a DELTA against the just-certified parent (the
  prev-dirty obstacle disappears at that call site) can certify most
  children without their own subtree-root scan: scan count divided by
  the effective branching factor, claims unchanged, soundness from the
  parent certificate plus the window lemmas.

## Sound rules added, inert or net-negative at benchmark scale

Kept in the code for re-evaluation on larger boards; each is proven
sound against exhaustive ground truth:

- Entry-free lobe parity (any lobe size): an alternating path of n
  cells needs color balance 0 (n even) or +-1 (n odd); a pendant lobe
  failing both its B and B-minus-cut variants is dead. Zero fires on
  benchmark levels (on by default, zero cost: computed in the pop walk).
- Chain-interior block refutation (COIL_CHAIN, default off): a 2-cut
  block with cells beyond both cuts and no head adjacency must admit a
  through-traversal or a one-cut full cover with the other cut optional;
  all modes content-cached. ~1% net ops loss at <=104x102 because
  through-traversals are rarely impossible at that scale.

The pattern across all additions: at benchmark scale the search is
limited by full-region scan cost, not by missing claim types. Larger
boards shift the balance (more corridor structure, bigger pockets);
re-measure the gated rules when the benchmark gains a 500+ level.
- Self-contained subproblems (pendant pockets behind a cut vertex) are
  decided once and cached by content; this generalizes: any state
  fragment with a provably closed boundary is a candidate for exact
  micro-solution + caching.

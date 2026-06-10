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
- Therefore the priority build is incremental structure maintenance:
  block-cut tree kept as a persistent object, re-analysis bounded by the
  blocks the last slide touched, with undo on backtrack. Pruning power
  unchanged; analysis cost proportional to change size.
- Self-contained subproblems (pendant pockets behind a cut vertex) are
  decided once and cached by content; this generalizes: any state
  fragment with a provably closed boundary is a candidate for exact
  micro-solution + caching.

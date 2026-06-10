# Coil Solver — Fable

A solver for the [coilbench](https://github.com/adum/coilbench) AI coding
benchmark ([Puzzle Runner](https://adum.github.io/puzzle-runner/)), written
iteratively by the Fable model running as a Cursor cloud agent.

## The puzzle (Mortal Coil)

On a rectangular grid with walls, pick any empty start cell, then slide
up/down/left/right; each slide continues until blocked by a wall, the board
edge, or an already-visited cell. Every empty cell must be visited exactly
once — a Hamiltonian path on the empty-cell grid graph, restricted to
slide-until-blocked dynamics. Boards grow from 3x3 (level 1) to 1990x1990
(level 1207). Score = highest consecutive level solved within the timeout
(600s per level for leaderboard entries).

## Build / use

```
gcc -O2 -march=native -o solver solver.c
./solver < level_file        # reads x=W&y=H&board=... on stdin
```

Drop the `solver` binary into a coilbench checkout: `./run_solver` (and both
evaluators) pick it up automatically.

## Architecture

Multi-start DFS over slides, with the search effort organized as escalating
restarts:

- **Start candidates** are parity-filtered (checkerboard counting argument
  pins the start color when the cell count is odd) and ordered: initial
  degree-1 cells (which must be path endpoints) first, then by degree.
  With exactly two degree-1 cells, those are provably the only starts.
- **Budget tiers**: each start gets a small work budget; starts that
  exhaust their search space are marked dead forever, while budget-aborted
  starts are retried at the next tier (250 -> 4000 -> ... -> unbounded,
  scaled by board size). Winning starts typically solve within a few
  thousand branch nodes, so cheap wide scanning beats deep dives.
- **4 worker processes** split the start list round-robin; each uses a
  rotated direction priority and randomized tie-breaking, so tier retries
  explore genuinely different trees (heavy-tail restart mitigation).

Per-node machinery:

- **Forced-move chaining**: positions with one legal direction never
  allocate a branch node.
- **Slide lookahead**: candidate slides are simulated first; dead-on-arrival
  slides (no onward move at the end) are skipped, instant wins taken, and
  the rest ordered by end-position constrainedness.
- **O(1) structural pruning** maintained incrementally per visited cell:
  bipartite parity balance, degree-1 (leaf) count, isolated-cell count.
  More than 2 leaves, 2 isolated cells, or an infeasible color balance
  kills a position immediately, even mid-chain.
- **Region analysis** (every 8th branch node): an iterative Tarjan
  biconnected-components pass over the remaining region checks
  connectivity and counts *leaf blocks* of the block-cut tree. A graph
  with a Hamiltonian path has a path-shaped block-cut tree, so >= 3 leaf
  blocks is fatal; with exactly 2, the next entered cell must lie strictly
  inside one of them (it is a path endpoint). Discovery marks use a
  monotone counter so the per-cell visited array never needs clearing.
- **Undo via visit log**: failed subtrees rewind in O(cells visited).

## Results

Scores on the coilbench progression (official `evaluate_full.py`, odd+even
levels, 600s/level, 4-core VM):

| Solver generation | Highest level | Notes |
| --- | --- | --- |
| v1 naive DFS + parity/connectivity | 83 | 10s timeout, odd only |
| v2 + leaf/isolated counters | 137 | 10s timeout, odd only |
| v3 + lookahead, budget tiers, 4 workers | 175 | 60s timeout, odd only |
| v6 + leaf-block pruning, endpoint targeting | 233 | 600s, odd only |
| v9 + tier tuning, check gating, counter fold | 262 | official full run |
| v10 + worker cadence diversity, -O3 | 269 | official full run |
| v11 + time-based restart seeds | **276** | official full run |

Reference scores from `results.md` in the benchmark repo: GPT-5.3-codex 117,
GPT-5.5 163, Gemini 3 195, kimi 2.6 264 (previous best). For human context
(hacker.org Mortal Coil): >200 people passed level 100, 47 passed level 300,
4 finished all levels.

## Single-thread era (v12+)

Under a stricter discipline (one thread, 60s per level), the solver was
rebuilt around a key empirical discovery: **winning start cells are rare**
(often literally one cell out of thousands; exhaustive enumeration on small
levels shows 0.7-4.5% winner density, spatially clustered), while dead
starts are cheap to *disprove* with strong pruning. That inverted the
architecture:

- **Transposition table** (16M entries): a state is the visited-set Zobrist
  hash plus head cell; fully-explored dead states are remembered, so search
  orders that permute into the same coverage collapse to one subtree, and
  re-visits across passes/starts are free.
- **Two-pass exhaustive sweep** replaced randomized restart tiers: pass 1
  visits every start with a budget sized to observed winner costs (winners
  solve within a few thousand branch nodes); the rare expensive dead trees
  are deferred and exhausted in pass 2, deepest-progress first. The sweep
  is deterministic and complete - the true solution can never be skipped.
- **Forced-edge engine** in the region pass: degree-2 cells force both
  incident edges; forced components are segments and cycles. Each forced
  cycle demands a path endpoint on it, leaf blocks demand endpoints inside
  them, and only two endpoints exist - infeasible demand patterns prune.
- Optional greedy-probe pass (one backtrack-free descent per start) ranks
  starts by reachable depth before the sweep.

Single-thread 60s frontier: **207, certified consecutive from level 1**
(the restart-roulette architecture managed 195 under the same limits).
Each improvement step was validated against ground truth obtained by
exhaustively enumerating every start of small levels, so no change could
silently discard the (often unique) winning start.

Build with `./build.sh <coilbench_dir>` (PGO two-stage compile).

## State decomposition (the current frontier of the work)

The deepest structural fact found: when the remaining region develops a
cut vertex, the pendant pocket behind it is **provably self-contained** -
no slide can cross its boundary except through the cut vertex, so "can
this pocket still be covered, given how it can be entered?" is a closed
sub-puzzle whose answer depends only on the pocket's cell content. The
solver now extracts pendant pockets (<= 44 cells) during region analysis,
decides them exactly with a bitmask micro-search, and caches verdicts by
content hash. One micro-solve typically serves hundreds of later branches
(measured: ~900 solves -> ~230k cache hits on one level); every branch
that ever re-creates a hopeless pocket dies instantly.

Honest assessment: at the current frontier (~70x70), most failing branches
die in open areas rather than small pockets, so this lands as a foundation
rather than a breakthrough. The natural continuations, in order:

1. wider pockets (multi-word masks, 96+ cells) and pocket *macro-moves*
   (when a pocket has exactly one feasible entry mode, its traversal is
   forced: substitute it wholesale instead of searching it)
2. unified endpoint-demand accounting (leaf blocks + forced cycles +
   over-forced junctions share a budget of exactly 2 path endpoints;
   currently checked piecewise)
3. block-cut-tree macro search: when the block tree is a path, the level
   decomposes into a sequence of per-block sub-puzzles with pinned
   entry/exit cells - solve blocks, not cells.

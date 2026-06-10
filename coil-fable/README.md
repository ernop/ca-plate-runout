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

Scores on the coilbench progression (600s/level, 4-core VM):

| Solver generation | Highest level | Notes |
| --- | --- | --- |
| v1 naive DFS + parity/connectivity | 83 | 10s timeout |
| v2 + leaf/isolated counters | 137 | 10s timeout |
| v3 + lookahead, budget tiers, 4 workers | 175 | 60s timeout |
| v6 + leaf-block pruning, endpoint targeting | 233+ | 600s timeout |
| v9 + tier tuning, check gating, counter fold | 261+ | 600s timeout |

Reference scores from `results.md` in the benchmark repo: GPT-5.3-codex 117,
GPT-5.5 163, Gemini 3 195, kimi 2.6 264.

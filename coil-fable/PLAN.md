# Plan (updated every iteration)

## Status after ITER 1-4
- ITER 1 accepted (lobe cache build-on-miss, order-independent keys).
- ITER 2 rejected (sealed-pocket pre-pass: cost > savings).
- ITER 3 closed the question: scan frequency is irreducible without
  maintained structure (lazy == always, bit-identical).
- ITER 4 landed Stage A: scans record the block decomposition
  (COIL_STRUCT, verified by COIL_PARANOID; +13% when on, default off).

## ITER 5 result: Stage B (block-granular) is falsified before building
The region stays one giant block (97-99% of cells) at the depths where
scans concentrate (viz_8). Replacement directions, in order:
 1. Sub-block incrementality: dynamic biconnectivity with rollback over
    the giant block. Research-grade; the only route that cuts the
    dominant term at source. Long horizon.
 2. DONE/REJECTED (ITER 6): gated rules stay inert at 501; scale wall
    quantified at ~150M ops per refutation, ~1.4T per level (viz_9).
 3. Giant-block-phase paradigm: while the region is one block with no
    structure, scans yield only connectivity; investigate whether the
    scan can be split (cheap flood always, Tarjan only once structure
    is plausible: e.g. when nleaf/deg-2 counts change regime). Must be
    falsifiable by the bench; beware claim starvation (design #2).
 4. PRIMARY (feasibility proven, ITER 8: 1051:1 local-state redundancy
    at 501; hot windows structured, ITER 9): window-verdict engine -
    outside-optimistic bounded local refutation, cached per pattern. Refutation
    subtrees over the same untouched sub-area are near-isomorphic
    across prefixes, but exact-state TT shares nothing. A sound
    canonicalization (fragment + entry interface, generalizing the
    lobe verdicts beyond cut-vertex anchors) is the only mechanism
    family that attacks the exponential walk enumeration itself.

## Superseded: Stage B - delta re-analysis (consumes Stage A)
At a scan point, if a valid decomposition exists from an earlier scan
in this attempt (generation still live, undo log intact):
 1. affected = set of block ordinals containing cells of rays committed
    since that scan (collect during do_slide: blkgenof-tagged lookups,
    O(ray) each; invalidate on rewind past the scan).
 2. re-run the analyze machinery on ONLY the affected blocks' cells
    (boundary = their cut vertices, attachments known from the stored
    tree) - the windowed-Tarjan code path with known-boundary
    classification instead of no-claims boundary.
 3. splice: new sub-blocks replace affected ordinals; update leaf
    statuses and the global leaf count; pendant lobes among new
    sub-blocks go through the existing content-cached micro-refuter.
 4. undo: log (cell, old blkid/cutflag/gen) and (ordinal, old meta) per
    visit-log segment; rewind restores. Fall back to a full scan when
    the touched set exceeds half the region (early game).
Acceptance: level 101 under 30M ops, 201 solved in 300M, prune counts
per branch not worse, ground-truth winner sets exact.

## Then
- Stage C: live leafmark queries from the maintained tree (restriction
  at every branch node, not only at scan ticks).
- Stage D: re-evaluate gated rules (chain blocks, lobe parity, probe
  ordering) at level-500+ scale where corridor structure multiplies.

## Rules
See METHOD.md: ops only, no tuning, sound and complete, 10s commands,
document and commit every iteration (branch + master for docs/viz).

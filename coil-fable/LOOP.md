# The Loop (one iteration, exactly)

Every iteration executes these steps in order. No step may be skipped.
Every command finishes inside 10 seconds. Single thread. Ops only.

1. PICK - take the top item of PLAN.md (a hypothesis about wasted ops or
   a missing piece of mathematical knowledge), or derive a new one from
   the latest profile data. State it in one sentence in NOTES.md.

2. PREDICT - write down what measurement would confirm or refute it
   (which benchmark numbers, which counters, which histogram).

3. BUILD - the smallest sound change (or pure instrumentation) that
   tests the hypothesis. Soundness argument written next to the code.

4. GATE - exhaustive winner-set enumeration on levels 31/33 (35 when
   classification machinery is touched) must match ground truth exactly.
   Any mismatch: stop, fix or revert; record the failure mode.

5. MEASURE - ./bench.sh (fixed levels 31/101/151/201/301, seed 7, 300M
   ops, kill-safe reporting). Compare against the baseline in NOTES.md.

6. VERDICT - ACCEPT only if ops decrease (or the change is inert
   instrumentation/foundation with gated cost). Otherwise REJECT, revert
   behavior, and record the falsification so it is never retried blind.

7. VISUALIZE - add at least one new explanatory visualization (or a
   regenerated one with new data) under viz/, illustrating what the
   iteration actually observed: where ops went, what was pruned, what
   the new mechanism did. The viz history must stay complete: every
   loop leaves a visual record.

8. DOCUMENT - update NOTES.md (iteration block), PLAN.md (status + next
   target), METHOD.md when a principle is established or falsified.

9. COMMIT - commit code + docs + viz to the working branch; sync
   METHOD.md, PLAN.md, NOTES.md and viz/ to master; push both.

10. REPEAT from step 1.

## Viz history index

- viz_1_mechanics.png      game rules and slide mechanics
- viz_2_pruning.png        the four structural pruning rules
- viz_3_search.png         search anatomy at a live branch point
- viz_4_solution.png       a solved level's full path
- viz_5_results.png        solve times and leaderboard context (600s era)
- viz_6_opsdepth.png       ops by search depth: scans dominate at 75-87%
- viz_7_loophistory.png    iteration ledger: accepted/rejected, op deltas
- viz_8_fragmentation.png  block fragmentation by depth (Stage B sizing)
- viz_9_scalewall.png      refutation cost vs board size (the scale wall)
- viz_10_attemptanatomy.png  attempt anatomy at 501: budgets dominate
- viz_11_redundancy.png    local-state redundancy: 1051:1 at level 501
- viz_12_hotwindows.png    hot windows are structured: verdict engine viable

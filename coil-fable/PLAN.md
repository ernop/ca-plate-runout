# Plan (updated every iteration)

## Now
1. Cut measured waste inside the existing scan economy:
   a. lobe cache: hash during the pop walk; build adjacency only on miss
   b. sealed-pocket pre-pass inside the scan: capped floods from the last
      ray's side-runs before paying O(region) Tarjan
   c. sibling scan sharing: window-delta against the just-certified
      parent to skip child subtree-root scans
2. Re-profile after each accepted change; the dominant bucket dictates
   the next target.

## Standing (large builds)
- Incremental block-cut maintenance (specified in METHOD.md)
- Chain/lobe verdict generalization to larger fragments

## Rules
See METHOD.md: ops only, no tuning, sound and complete, 10s commands,
document and commit every iteration (branch + master for docs/viz).

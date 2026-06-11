#!/usr/bin/env python3
"""viz_8: block fragmentation by depth - the Stage B feasibility test."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import sys
from pathlib import Path

BG, PANEL, WALLC, WHITE = "#0d1117", "#161b22", "#30363d", "#FFFFFF"
BLUE, RED, GOLD = "#58a6ff", "#ff6b6b", "#d29922"
plt.rcParams.update({
    "figure.facecolor": BG, "axes.facecolor": PANEL, "text.color": WHITE,
    "axes.edgecolor": WALLC, "axes.labelcolor": WHITE,
    "xtick.color": WHITE, "ytick.color": WHITE, "font.size": 11,
})

labels = ["87-75", "75-62", "62-50", "50-37", "37-25", "25-12"]
# largest-block share of region (permil) per depth bucket, from ITER 5
share_101 = [975, 965, 955, 947, 924, 817]
share_201 = [991, 987, 980, 977, 962, 940]
# share of all scan calls per bucket (n), normalized
n_201 = np.array([5323, 892, 606, 161, 301, 91], dtype=float)
n_201 /= n_201.sum()

fig, ax = plt.subplots(figsize=(11.5, 6))
x = np.arange(len(labels))
ax.plot(x, np.array(share_101) / 10, "o-", color=BLUE, linewidth=2,
        label="level 101: largest block, % of region")
ax.plot(x, np.array(share_201) / 10, "s-", color=GOLD, linewidth=2,
        label="level 201: largest block, % of region")
ax2 = ax.twinx()
ax2.bar(x, n_201 * 100, color=RED, alpha=0.25, width=0.55,
        label="level 201: share of all scans (%)")
ax2.set_ylabel("share of all scans, %", color=RED)
ax2.tick_params(axis="y", colors=RED)
ax.set_xticks(x, labels)
ax.set_xlabel("% of board still free during the scan")
ax.set_ylabel("largest biconnected block, % of region")
ax.set_ylim(75, 101)
ax.set_title("ITER 5: the region stays one giant block where scans concentrate\n"
             "=> block-granular delta re-analysis (Stage B) cannot pay; "
             "falsified before building",
             fontsize=12.5, fontweight="bold", color=WHITE, pad=12)
ax.legend(loc="lower left", facecolor=PANEL, edgecolor=WALLC,
          labelcolor=WHITE, fontsize=10)
ax.grid(color=WALLC, linewidth=0.5, alpha=0.6)
fig.tight_layout()
out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
fig.savefig(out / "viz_8_fragmentation.png", dpi=140)
print("wrote viz_8")

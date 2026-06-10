#!/usr/bin/env python3
"""Visualize where the solver's ops go, by search depth and category."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import sys
from pathlib import Path

BG, PANEL, WALLC, WHITE = "#0d1117", "#161b22", "#30363d", "#FFFFFF"
BLUE, ORANGE, GREEN, RED = "#58a6ff", "#f78166", "#7ee787", "#ff6b6b"
plt.rcParams.update({
    "figure.facecolor": BG, "axes.facecolor": PANEL, "text.color": WHITE,
    "axes.edgecolor": WALLC, "axes.labelcolor": WHITE,
    "xtick.color": WHITE, "ytick.color": WHITE, "font.size": 11,
})

# (level, region_ops_by_bucket, visit_ops_by_bucket, branches_by_bucket)
# buckets, left to right: 87-100% of board still free ... 0-12% free
DATA = {
    "201 (70x69)": {
        "region": [0, 178010118, 44302572, 23773500, 5555604, 3450720, 713190, 0],
        "visit":  [0, 13610328, 3913080, 2361488, 450560, 218248, 34528, 0],
        "branch": [0, 87283, 21705, 11664, 2576, 1479, 311, 0],
    },
    "101 (36x36)": {
        "region": [0, 42214164, 28410774, 21513330, 19353240, 2877666, 9156, 2718],
        "visit":  [0, 2350744, 1779592, 1329488, 869480, 216136, 2464, 1152],
        "branch": [0, 19636, 15237, 13493, 7087, 1386, 7, 5],
    },
}

labels = ["100-87", "87-75", "75-62", "62-50", "50-37", "37-25", "25-12", "12-0"]

fig, axes = plt.subplots(1, 2, figsize=(14, 5.6))
fig.suptitle("Where the ops go: search effort by remaining-free depth "
             "(300M-op budget, fixed seed)",
             fontsize=14, fontweight="bold", color=WHITE)

for ax, (name, d) in zip(axes, DATA.items()):
    x = np.arange(8)
    region = np.array(d["region"], dtype=float)
    visit = np.array(d["visit"], dtype=float)
    other = np.array(d["branch"], dtype=float) * 12
    ax.bar(x, region, color=BLUE, label="region structure scans")
    ax.bar(x, visit, bottom=region, color=ORANGE, label="cell visits/undo")
    ax.bar(x, other, bottom=region + visit, color=GREEN, label="branch overhead")
    ax.set_xticks(x, labels, rotation=30, fontsize=9)
    ax.set_xlabel("% of board still free during the work")
    ax.set_ylabel("ops")
    ax.set_title(f"level {name}", color=WHITE)
    ax.legend(facecolor=PANEL, edgecolor=WALLC, labelcolor=WHITE, fontsize=9)
    ax.grid(axis="y", color=WALLC, linewidth=0.5, alpha=0.6)
    tot = (region + visit + other).sum()
    peak = (region + visit + other).max()
    pk = int(np.argmax(region + visit + other))
    ax.text(0.98, 0.95,
            f"peak bucket: {labels[pk]}% free\n{100*peak/tot:.0f}% of all ops",
            transform=ax.transAxes, ha="right", va="top",
            color="#9ecbff", fontsize=10)

fig.tight_layout(rect=(0, 0, 1, 0.92))
out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
fig.savefig(out / "viz_6_opsdepth.png", dpi=140)
print("wrote", out / "viz_6_opsdepth.png")

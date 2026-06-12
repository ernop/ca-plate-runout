#!/usr/bin/env python3
"""viz_9: the scale wall - refutation cost vs board size (ITER 6)."""
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

# measured: free cells vs avg ops to refute one dead start
cells = np.array([134, 860, 1866, 2400, 6900, 19000], dtype=float)
opsref = np.array([1.3e3, 6.0e4, 2.1e5, 4.5e5, 2.5e6, 1.5e8], dtype=float)
lvl = ["31", "101", "151", "201", "301", "501"]

fig, ax = plt.subplots(figsize=(11, 6))
ax.loglog(cells, opsref, "o-", color=RED, linewidth=2, markersize=8)
for c, o, l in zip(cells, opsref, lvl):
    ax.annotate(f"L{l}", (c, o), textcoords="offset points", xytext=(8, -4),
                color=WHITE, fontsize=10)
ref = opsref[0] * (cells / cells[0]) ** 2
ax.loglog(cells, ref, "--", color=WALLC, linewidth=1.5)
ax.text(cells[-2], ref[-2] * 0.4, "quadratic reference", color="#9ecbff",
        fontsize=9, rotation=18)
ax.set_xlabel("free cells on the board")
ax.set_ylabel("ops to refute one dead start (avg)")
ax.set_title("ITER 6: the scale wall. Refutation cost grows super-quadratically;\n"
             "at level 501 one start costs 150M ops, the field ~1.4T. Gated rules\n"
             "(chain, lobe parity) stay inert: open-area walk enumeration dominates.",
             fontsize=12, fontweight="bold", color=WHITE, pad=12)
ax.grid(color=WALLC, linewidth=0.5, alpha=0.6, which="both")
fig.tight_layout()
out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
fig.savefig(out / "viz_9_scalewall.png", dpi=140)
print("wrote viz_9")

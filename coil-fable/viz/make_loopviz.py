#!/usr/bin/env python3
"""viz_7: the iteration ledger - what each loop tried and what it did."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import sys
from pathlib import Path

BG, PANEL, WALLC, WHITE = "#0d1117", "#161b22", "#30363d", "#FFFFFF"
GREEN, RED, GOLD, BLUE = "#7ee787", "#ff6b6b", "#d29922", "#58a6ff"
plt.rcParams.update({
    "figure.facecolor": BG, "axes.facecolor": PANEL, "text.color": WHITE,
    "axes.edgecolor": WALLC, "axes.labelcolor": WHITE,
    "xtick.color": WHITE, "ytick.color": WHITE, "font.size": 11,
})

# (label, verdict, ops delta on level 101 in %, note)
ITERS = [
    ("sparser scan cadence",      "REJ", +23.0, "tree x3: scans are load-bearing"),
    ("windowed analysis",         "REJ", +132., "local view loses global claims"),
    ("dist-2 carved rind",        "REJ", +99.0, "pockets reach into virgin zone"),
    ("lobe parity (any size)",    "ACC",  0.0,  "sound, inert at this scale"),
    ("chain-block refutation",    "GATE", +0.8, "sound; re-test at level 500+"),
    ("lobe cache build-on-miss",  "ACC", -0.3,  "order-independent keys"),
    ("sealed-pocket pre-pass",    "REJ", +2.6,  "cost > savings"),
    ("lazy scans (ops metric)",   "CLOSED", 0.0,"bit-identical: freq irreducible"),
    ("Stage A: store blocks",     "ACC", 0.0,   "+13% gated; Stage B foundation"),
]

fig, ax = plt.subplots(figsize=(12.5, 6.8))
ys = range(len(ITERS) - 1, -1, -1)
colors = {"ACC": GREEN, "REJ": RED, "GATE": GOLD, "CLOSED": BLUE}
for y, (label, verdict, delta, note) in zip(ys, ITERS):
    c = colors[verdict]
    ax.barh(y, max(min(delta, 60), -60) or (2 if verdict != "ACC" else -2),
            color=c, alpha=0.85, height=0.62)
    ax.text(-62, y, label, ha="right", va="center", fontsize=11, color=WHITE)
    ax.text(62, y, f"{verdict}  {note}", ha="left", va="center",
            fontsize=9.5, color=c)
ax.axvline(0, color=WHITE, linewidth=1)
ax.set_xlim(-120, 175)
ax.set_yticks([])
ax.set_xticks([-50, -25, 0, 25, 50])
ax.set_xlabel("ops change on level 101, % (clipped at 60)")
ax.set_title("Iteration ledger: every hypothesis, measured and recorded",
             fontsize=14, fontweight="bold", color=WHITE, pad=14)
ax.grid(axis="x", color=WALLC, linewidth=0.5, alpha=0.6)
fig.tight_layout()
out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
fig.savefig(out / "viz_7_loophistory.png", dpi=140)
print("wrote viz_7")

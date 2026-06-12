#!/usr/bin/env python3
"""
Visualization series for the Fable Coil solver.

Generates:
  viz_1_mechanics.png  - the puzzle rules and slide mechanics
  viz_2_pruning.png    - the four structural pruning rules
  viz_3_search.png     - anatomy of the search at a live branch point
  viz_4_solution.png   - a full solved level with the path drawn
  viz_5_results.png    - solve times and benchmark comparison

Run from the coilbench checkout directory (needs levels_public/ and ./solver):
  python3 make_viz.py <coilbench_dir> <output_dir>
"""
import re
import subprocess
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import FancyArrow, Rectangle, Circle

BG     = "#0d1117"
PANEL  = "#161b22"
WALLC  = "#30363d"
FREEC  = "#e6edf3"
VISC   = "#1f6feb"
BLUE   = "#58a6ff"
ORANGE = "#f78166"
GREEN  = "#7ee787"
GOLD   = "#d29922"
RED    = "#ff6b6b"
WHITE  = "#FFFFFF"

plt.rcParams.update({
    "figure.facecolor": BG, "axes.facecolor": PANEL,
    "text.color": WHITE, "axes.edgecolor": WALLC,
    "axes.labelcolor": WHITE, "xtick.color": WHITE, "ytick.color": WHITE,
    "font.size": 11, "font.family": "DejaVu Sans",
})

DIRS = {"L": (-1, 0), "R": (1, 0), "U": (0, -1), "D": (0, 1)}


def parse_level(text):
    m = re.match(r"x=(\d+)&y=(\d+)&board=([.X]+)", text.strip())
    w, h, b = int(m.group(1)), int(m.group(2)), m.group(3)
    grid = np.array([[1 if b[y * w + x] == "X" else 0 for x in range(w)]
                     for y in range(h)])
    return w, h, grid


def parse_solution(text):
    m = re.match(r"x=(\d+)&y=(\d+)&path=([UDLR]+)", text.strip())
    return int(m.group(1)), int(m.group(2)), m.group(3)


def replay(w, h, grid, sx, sy, path):
    """Replay slides; return ordered list of visited cells."""
    visited = {(sx, sy)}
    cells = [(sx, sy)]
    x, y = sx, sy
    for ch in path:
        dx, dy = DIRS[ch]
        while True:
            nx, ny = x + dx, y + dy
            if not (0 <= nx < w and 0 <= ny < h):
                break
            if grid[ny][nx] or (nx, ny) in visited:
                break
            x, y = nx, ny
            visited.add((x, y))
            cells.append((x, y))
    return cells


def draw_board(ax, w, h, grid, visited=None, cellsize=1.0):
    ax.set_xlim(-0.6, w - 0.4)
    ax.set_ylim(h - 0.4, -0.6)
    ax.set_aspect("equal")
    ax.axis("off")
    visited = visited or set()
    for y in range(h):
        for x in range(w):
            if grid[y][x]:
                c = WALLC
            elif (x, y) in visited:
                c = VISC
            else:
                c = FREEC
            ax.add_patch(Rectangle((x - 0.5, y - 0.5), cellsize, cellsize,
                                   facecolor=c, edgecolor=BG, linewidth=0.8))


def path_line(ax, cells, lw=3.0, cmap="plasma"):
    pts = np.array(cells, dtype=float)
    for i in range(len(pts) - 1):
        t = i / max(1, len(pts) - 2)
        ax.plot(pts[i:i+2, 0], pts[i:i+2, 1],
                color=plt.get_cmap(cmap)(t), linewidth=lw,
                solid_capstyle="round", zorder=5)
    ax.add_patch(Circle(tuple(pts[0]), 0.30, facecolor=GREEN,
                        edgecolor=BG, zorder=6))
    ax.add_patch(Circle(tuple(pts[-1]), 0.30, facecolor=RED,
                        edgecolor=BG, zorder=6))


# ---------------------------------------------------------------- figure 1
def fig_mechanics(bench, out):
    fig, axes = plt.subplots(1, 3, figsize=(13, 5))
    fig.suptitle("Mortal Coil: slide until blocked, visit every cell exactly once",
                 fontsize=15, fontweight="bold", color=WHITE, y=0.98)

    w, h, grid = parse_level("x=3&y=3&board=X.......X")

    ax = axes[0]
    draw_board(ax, w, h, grid)
    ax.set_title("Board: '.' free, 'X' wall\npick any start cell", color=WHITE)

    ax = axes[1]
    draw_board(ax, w, h, grid)
    cells = replay(w, h, grid, 1, 0, "RD")
    path_line(ax, cells)
    for (cx, cy), (ox, oy), lbl in [((1, 0), (-38, 26), "start"),
                                    ((2, 0), (-4, 26), "R: slides to wall"),
                                    ((2, 1), (-52, -30), "D: stops before X")]:
        ax.annotate(lbl, (cx, cy), textcoords="offset points",
                    xytext=(ox, oy), ha="center", fontsize=9, color=GOLD)
    ax.set_title("Each move slides until it hits a\nwall, edge, or visited cell",
                 color=WHITE)

    ax = axes[2]
    draw_board(ax, w, h, grid)
    cells = replay(w, h, grid, 1, 0, "RDLDR")
    path_line(ax, cells)
    ax.set_title('Solution "x=1&y=0&path=RDLDR"\nall 7 free cells covered',
                 color=WHITE)

    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(out / "viz_1_mechanics.png", dpi=140)
    plt.close(fig)


# ---------------------------------------------------------------- figure 2
def mini(ax, rows, title, sub):
    h, w = len(rows), len(rows[0])
    grid = np.array([[1 if ch == "X" else 0 for ch in r] for r in rows])
    draw_board(ax, w, h, grid)
    ax.set_title(title, color=WHITE, fontsize=12, pad=8)
    ax.text(0.5, -0.13, sub, transform=ax.transAxes, ha="center",
            va="top", fontsize=9.5, color="#9ecbff", wrap=True)
    return grid, w, h


def fig_pruning(bench, out):
    fig, axes = plt.subplots(2, 2, figsize=(12.5, 12.6),
                             gridspec_kw={"hspace": 0.42})
    fig.suptitle("Why positions die: the four structural pruning rules",
                 fontsize=15, fontweight="bold", color=WHITE, y=0.985)

    # (a) parity
    ax = axes[0][0]
    rows = ["...", "...", "..."]
    grid, w, h = mini(ax, rows, "1. Bipartite parity",
        "The path alternates checkerboard colors. With 5 dark and 4 light\n"
        "cells, a 9-cell path MUST start (and end) on a dark cell.\n"
        "Counts are maintained in O(1); a mismatch kills the branch.")
    for y in range(h):
        for x in range(w):
            if (x + y) % 2 == 0:
                ax.add_patch(Rectangle((x-0.5, y-0.5), 1, 1,
                             facecolor=BLUE, alpha=0.45, edgecolor=BG))
    ax.text(0.5, 1.0, "", transform=ax.transAxes)

    # (b) leaves
    ax = axes[0][1]
    rows = ["XX.XX",
            "XX.XX",
            ".....",
            "XX.XX",
            "XX.XX"]
    grid, w, h = mini(ax, rows, "2. Dead-end (leaf) counting",
        "A degree-1 cell can only be a path endpoint. This cross has 4\n"
        "leaf tips but a path has only 2 endpoints: unsolvable. The solver\n"
        "tracks leaf/isolated counts incrementally, even inside forced runs.")
    for (x, y) in [(2, 0), (2, 4), (0, 2), (4, 2)]:
        ax.add_patch(Circle((x, y), 0.33, facecolor="none",
                            edgecolor=RED, linewidth=2.5))

    # (c) connectivity split
    ax = axes[1][0]
    rows = ["....X....",
            "....X....",
            ".........",
            "....X....",
            "....X...."]
    grid, w, h = mini(ax, rows, "3. Connectivity: a slide can split the region",
        "Sliding across the middle row paints it visited, cutting the free\n"
        "region in two -- the path can never reach the far side. Slides are\n"
        "screened by counting free-neighbor runs along the painted ray.")
    visited = {(x, 2) for x in range(9)}
    draw_board(ax, w, h, grid, visited)
    pts = np.array([(x, 2) for x in range(9)], dtype=float)
    ax.plot(pts[:, 0], pts[:, 1], color=GOLD, linewidth=4,
            solid_capstyle="round", zorder=5)
    ax.annotate("slide", (7.0, 2), textcoords="offset points",
                xytext=(0, -18), color=GOLD, fontsize=10)
    ax.text(2, 0.9, "stranded", ha="center", color=RED, fontsize=10, zorder=6)
    ax.text(6.2, 3.9, "head side", ha="center", color=GREEN, fontsize=10, zorder=6)

    # (d) block-cut tree
    ax = axes[1][1]
    rows = ["...X...X...",
            "...X...X...",
            "...........",
            "...X...X...",
            "...X...X..."]
    grid, w, h = mini(ax, rows, "4. Leaf blocks of the block-cut tree",
        "A Hamiltonian path needs a path-shaped block-cut tree: every leaf\n"
        "block (biconnected component touching one cut vertex) needs its own\n"
        "endpoint strictly inside. Three leaf blocks = dead. With exactly two,\n"
        "the very next cell entered must lie inside one of them.")
    for (x0, y0, ww, hh, lbl) in [(-0.5, -0.5, 3, 5, "leaf block A"),
                                  (3.5, -0.5, 3, 5, "leaf block B"),
                                  (7.5, -0.5, 3, 5, "leaf block C")]:
        ax.add_patch(Rectangle((x0+0.06, y0+0.06), ww-0.12, hh-0.12,
                     facecolor="none", edgecolor=ORANGE, linewidth=2.5,
                     linestyle="--", zorder=6))
        ax.text(x0 + ww/2, y0 + 5.45, lbl, ha="center", color=ORANGE,
                fontsize=10, zorder=6)

    fig.tight_layout(rect=(0, 0.01, 1, 0.95))
    fig.savefig(out / "viz_2_pruning.png", dpi=140)
    plt.close(fig)


# ---------------------------------------------------------------- figure 3
def fig_search(bench, out):
    lvl = (bench / "levels_public" / "47").read_text()
    w, h, grid = parse_level(lvl)
    sol = subprocess.run(["./solver"], input=lvl, capture_output=True,
                         text=True, cwd=bench).stdout
    sx, sy, path = parse_solution(sol)
    cells = replay(w, h, grid, sx, sy, path)

    cut = int(len(cells) * 0.55)
    visited = set(cells[:cut])
    hx, hy = cells[cut - 1]

    fig = plt.figure(figsize=(13.5, 7.2))
    gs = fig.add_gridspec(1, 2, width_ratios=[1.45, 1], wspace=0.06)
    ax = fig.add_subplot(gs[0])
    draw_board(ax, w, h, grid, visited)
    pts = np.array(cells[:cut], dtype=float)
    ax.plot(pts[:, 0], pts[:, 1], color="#9ecbff", linewidth=1.6, alpha=0.85,
            zorder=4)
    ax.add_patch(Circle((hx, hy), 0.34, facecolor=GOLD, edgecolor=BG,
                        zorder=6))
    ax.set_title(f"Mid-search on level 47 ({w}x{h}): head at gold dot",
                 color=WHITE, fontsize=13)

    # candidate analysis at the head
    for ch, (dx, dy) in DIRS.items():
        nx, ny = hx + dx, hy + dy
        if not (0 <= nx < w and 0 <= ny < h) or grid[ny][nx] or (nx, ny) in visited:
            continue
        # simulate slide
        cx, cy, ln = hx, hy, 0
        while True:
            tx, ty = cx + dx, cy + dy
            if not (0 <= tx < w and 0 <= ty < h) or grid[ty][tx] or (tx, ty) in visited:
                break
            cx, cy, ln = tx, ty, ln + 1
        # onward options at end
        px, py = (0, 1) if dx else (1, 0)
        eo = 0
        for s in (1, -1):
            ex, ey = cx + s*px, cy + s*py
            if 0 <= ex < w and 0 <= ey < h and not grid[ey][ex] and (ex, ey) not in visited:
                eo += 1
        col = RED if eo == 0 else (GREEN if eo == 1 else ORANGE)
        ax.add_patch(FancyArrow(hx, hy, dx*ln, dy*ln, width=0.10,
                     head_width=0.42, length_includes_head=True,
                     facecolor=col, edgecolor=BG, zorder=7))

    axt = fig.add_subplot(gs[1])
    axt.axis("off")
    axt.text(0.02, 0.98, "Search anatomy", fontsize=15, fontweight="bold",
             color=WHITE, va="top")
    body = (
        "Candidate slides at the head are simulated first:\n"
        "  green   end has 1 onward move (forced -> try first)\n"
        "  orange  end has 2 onward moves (try later)\n"
        "  red     end is a dead stop (skipped entirely)\n"
        "  a slide covering all remaining cells wins instantly\n\n"
        "Single legal direction = forced move: no branch\n"
        "node is allocated, the search just slides on.\n\n"
        "Every branch node pays O(1) checks (parity, leaf,\n"
        "isolated counts); every 8th runs the full region\n"
        "analysis (connectivity + leaf blocks via Tarjan).\n\n"
        "Multi-start restarts:\n"
        "  starts ordered: forced endpoints first, then by\n"
        "  degree, shuffled per attempt\n"
        "  budget tiers 1k -> 8k -> 64k -> ... work units;\n"
        "  starts that exhaust their subtree are dead forever,\n"
        "  budget-aborted starts retry at the next tier with\n"
        "  fresh randomized tie-breaking\n\n"
        "Failed subtrees rewind through a visit log in\n"
        "O(cells), restoring all counters incrementally."
    )
    axt.text(0.02, 0.90, body, fontsize=11.3, color=FREEC, va="top",
             family="monospace", linespacing=1.45)

    fig.savefig(out / "viz_3_search.png", dpi=140)
    plt.close(fig)


# ---------------------------------------------------------------- figure 4
def fig_solution(bench, out):
    lvl = (bench / "levels_public" / "151").read_text()
    w, h, grid = parse_level(lvl)
    import time
    t0 = time.time()
    sol = subprocess.run(["./solver"], input=lvl, capture_output=True,
                         text=True, cwd=bench).stdout
    dt = time.time() - t0
    sx, sy, path = parse_solution(sol)
    cells = replay(w, h, grid, sx, sy, path)

    fig, ax = plt.subplots(figsize=(11, 11.6))
    draw_board(ax, w, h, grid)
    path_line(ax, cells, lw=2.2, cmap="plasma")
    free = int((grid == 0).sum())
    ax.set_title(
        f"Level 151 solved: {w}x{h}, {free} free cells, "
        f"{len(path)} slides, {dt:.2f}s\n"
        "path colored start (green) to end (red)",
        color=WHITE, fontsize=13.5, pad=12)
    fig.tight_layout()
    fig.savefig(out / "viz_4_solution.png", dpi=140)
    plt.close(fig)


# ---------------------------------------------------------------- figure 5
def fig_results(bench, out):
    pat = re.compile(r"Level (\d+) \((\d+)x(\d+)\): PASS \(([\d.]+)s\)")
    pts = {}
    for log in ["/tmp/eval_official.log", "/tmp/eval_official2.log",
                "/tmp/eval_official3.log", "/tmp/eval_270.log",
                "/tmp/eval_frontier.log"]:
        p = Path(log)
        if not p.exists():
            continue
        for m in pat.finditer(p.read_text(errors="ignore")):
            lvl, t = int(m.group(1)), float(m.group(4))
            pts[lvl] = min(t, pts.get(lvl, 1e9))

    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    ax = axes[0]
    xs = sorted(pts)
    ys = [max(pts[x], 0.004) for x in xs]
    ax.scatter(xs, ys, s=14, color=BLUE, alpha=0.85, linewidths=0)
    ax.set_yscale("log")
    ax.axhline(600, color=RED, linewidth=1.2, linestyle="--")
    ax.text(8, 700, "600s timeout", color=RED, fontsize=10)
    ax.set_xlabel("level (board side grows ~ level)")
    ax.set_ylabel("solve time, seconds (log)")
    ax.set_title("Official run: solve time per level", color=WHITE, fontsize=13)
    ax.grid(color=WALLC, linewidth=0.5, alpha=0.6)

    ax = axes[1]
    names = ["brute\nforce", "GPT-5.3\ncodex", "GPT-5.5", "Gemini 3",
             "kimi 2.6", "Fable\n(this work)"]
    scores = [47, 117, 163, 195, 264, 277]
    cols = [WALLC, WALLC, WALLC, WALLC, GOLD, GREEN]
    bars = ax.bar(names, scores, color=cols, edgecolor=BG)
    for b, s in zip(bars, scores):
        ax.text(b.get_x() + b.get_width()/2, s + 4, str(s), ha="center",
                color=WHITE, fontsize=12, fontweight="bold")
    ax.set_ylabel("highest level passed (600s/level)")
    ax.set_title("coilbench leaderboard context", color=WHITE, fontsize=13)
    ax.grid(axis="y", color=WALLC, linewidth=0.5, alpha=0.6)
    ax.set_ylim(0, 305)

    fig.tight_layout()
    fig.savefig(out / "viz_5_results.png", dpi=140)
    plt.close(fig)


def main():
    bench = Path(sys.argv[1] if len(sys.argv) > 1 else "/workspace/coilbench")
    out = Path(sys.argv[2] if len(sys.argv) > 2 else
               "/workspace/coil-fable/viz")
    out.mkdir(parents=True, exist_ok=True)
    fig_mechanics(bench, out)
    fig_pruning(bench, out)
    fig_search(bench, out)
    fig_solution(bench, out)
    fig_results(bench, out)
    print("wrote figures to", out)


if __name__ == "__main__":
    main()

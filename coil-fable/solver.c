/*
 * Coil (Mortal Coil) solver - Fable v2
 *
 * Game: on a grid with walls, pick any empty start cell, then repeatedly
 * slide U/D/L/R; each slide continues until blocked by wall/edge/visited.
 * Every empty cell must be visited exactly once (Hamiltonian path on the
 * empty-cell grid graph, restricted to slide-stop dynamics).
 *
 * Input  (stdin):  x=W&y=H&board=<W*H chars, '.' empty / 'X' wall>
 * Output (stdout): x=SX&y=SY&path=<UDLR...>  or  "No solution found"
 *
 * v2 adds over v1:
 *   - incremental degree tracking for every free cell
 *   - leaf (deg==1) and isolated (deg==0) counters, pruned everywhere,
 *     including inside forced-move chains (catches death early)
 *   - with exactly 2 leaves, the next entered cell must be a leaf
 *   - branch move ordering: prefer entering low-degree cells
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/select.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint64_t u64;

static int W, H;
static int PW;
static int NCELLS;
static u8  *blocked;        /* wall/border or visited */
static u8  *deg;            /* # free neighbors (valid for free cells) */
static s32 DELTA[4];
static const char DIRCH[4] = {'L','U','R','D'};

static int total_empty;
static int remaining;
static int colorbal;
static int nleaf;           /* free cells with deg==1 */
static int nisol;           /* free cells with deg==0 */

static char *path;
static int  pathlen;

static s32 *vlog;
static int  vloglen;

static u64 nodes;
static u64 node_budget;
static bool aborted;
static bool greedy_probe;    /* probe mode: first-descent only, no learning */
static int best_remaining;   /* deepest progress of the current attempt */
static u64 work_at_best;     /* work counter when that record was set */
static int dirperm[4] = {0, 1, 2, 3};   /* per-worker direction priority */
static int order_mode;                  /* candidate ordering variant */
static bool randtie;                    /* randomized tie-breaking */
static u64 rng = 0x9e3779b97f4a7c15ULL;
static u64 seed_salt = 0x9e3779b97f4a7c15ULL;
static u32 *mark;
static u32 markgen;
static u64 st_region_calls, st_region_cells, st_visits, st_branches;
static u64 st_p_parity, st_p_struct, st_p_conn, st_p_lb3, st_p_cyc,
           st_p_over, st_p_feas, st_p_dirs, st_p_tt;

/* Transposition table of proven-dead states. A state is the visited set
 * (Zobrist hash, maintained incrementally) plus the head cell. Search
 * orders that permute into the same coverage collapse to one subtree. */
#define TT_BITS 24
static u64 *tt;
static u64 *zkey, *zheadkey;
static u64 zhash;

static inline int cellcolor(int p) { return ((p % PW) + (p / PW)) & 1; }
static inline bool free_cell(int p) { return !blocked[p]; }

static inline void dec_deg(int p)
{
    u8 d = --deg[p];
    if (d == 1) nleaf++;
    else if (d == 0) { nleaf--; nisol++; }
}
static inline void inc_deg(int p)
{
    u8 d = ++deg[p];
    if (d == 1) { nisol--; nleaf++; }
    else if (d == 2) nleaf--;
}

static inline void visit_cell(int p)
{
    blocked[p] = 1; remaining--;
    colorbal += cellcolor(p) ? 1 : -1;
    zhash ^= zkey[p];
    /* p leaves the free set: remove its own leaf/isol contribution */
    if (deg[p] == 1) nleaf--;
    else if (deg[p] == 0) nisol--;
    if (!blocked[p-1])  dec_deg(p-1);
    if (!blocked[p+1])  dec_deg(p+1);
    if (!blocked[p-PW]) dec_deg(p-PW);
    if (!blocked[p+PW]) dec_deg(p+PW);
    vlog[vloglen++] = p;
    st_visits++;
}

static void rewind_to(int loglen, int plen)
{
    while (vloglen > loglen) {
        int p = vlog[--vloglen];
        blocked[p] = 0; remaining++;
        colorbal += cellcolor(p) ? -1 : 1;
        zhash ^= zkey[p];
        if (!blocked[p-1])  inc_deg(p-1);
        if (!blocked[p+1])  inc_deg(p+1);
        if (!blocked[p-PW]) inc_deg(p-PW);
        if (!blocked[p+PW]) inc_deg(p+PW);
        if (deg[p] == 1) nleaf++;
        else if (deg[p] == 0) nisol++;
    }
    pathlen = plen;
}

/*
 * Region check via Tarjan articulation DFS (iterative).
 *
 *  - connectivity: all `remaining` cells reachable from a head neighbor
 *  - cut-vertex rule: a Hamiltonian path visits each vertex once, so a
 *    vertex whose removal splits the region into >= 3 components is fatal
 *
 * Returns false if the position is dead.
 */
/*
 * A graph with a Hamiltonian path has a block-cut tree that is a path:
 * every leaf block (biconnected component touching <=1 cut vertex) needs
 * a path endpoint strictly inside it, and only 2 endpoints exist.
 * So: >=3 leaf blocks => dead.  (A cut vertex separating >=3 parts gives
 * a block-tree vertex of degree >=3, hence >=3 leaves: subsumed.)
 */
static u32 *disc, *low;
static s32 *tstack;          /* DFS cell stack */
static u8  *tdirs;           /* per-frame: next dir index to try */
static s32 *tparent;         /* per-frame: parent cell (-1 for root) */
static u8  *sepflag;         /* per-cell: is articulation (this call) */
static s32 *estack;          /* edge stack: child endpoint of each edge */
static s32 *eother;          /* edge stack: parent endpoint */
static u32 *bcmark;          /* per-cell mark for distinct count per block */
static u32 bcgen;
static u32 dctr;             /* monotone discovery counter across calls */
static s32 *rbuf;            /* deferred root-block vertex lists */
static u32 *leafmark;        /* cell is valid endpoint (interior of a leaf
                                block); generation = markgen of the call */
static u8  *fdeg;            /* forced path-degree from deg-2 neighbors */
static u32 *fmark;           /* generation guard for fdeg */
static s32 *rlist;           /* cells of the region, in discovery order */
static u32 *cycmark;         /* cell visited by a forced-component walk */
static u32 *oncyc;           /* cell lies on a forced cycle */
static u8  *lbid;            /* which of the 2 leaf blocks (when marked) */

static int last_leafblocks;  /* result of the most recent region_ok */
static int last_ncyc;        /* forced cycles found by most recent call */
static u32 last_region_gen;

static u64 work;             /* time-proxy budget counter */

static bool analyze_region(int seed, int head);

static bool region_ok(int pos)
{
    if (remaining == 0) return true;
    int seed;
    if      (free_cell(pos-1))  seed = pos-1;
    else if (free_cell(pos+1))  seed = pos+1;
    else if (free_cell(pos-PW)) seed = pos-PW;
    else if (free_cell(pos+PW)) seed = pos+PW;
    else return false;
    return analyze_region(seed, pos);
}

/*
 * Full structural analysis of the free region from seed (head = current
 * search position, or -1 for the virgin board).
 *
 * 1. Tarjan biconnected-components walk: connectivity, leaf-block count
 *    (>= 3 is fatal), endpoint marking (leaf-block interiors).
 * 2. Forced-edge analysis: a free deg-2 cell must use both its edges
 *    unless it is a path endpoint. Forced edges form disjoint segments
 *    and cycles:
 *      - a cell carrying > 2 forced edges needs endpoints spent on its
 *        forcers; total overload > 2 is fatal
 *      - every forced cycle needs >= 1 path endpoint on it
 * 3. Endpoint-demand feasibility: only 2 endpoints exist; leaf blocks
 *    demand one strictly inside each, cycles demand one on each; the
 *    demands must be coverable by 2 cells (with the next-entered cell
 *    adjacent to head).
 */
static bool analyze_region(int seed, int head)
{
    if (dctr > 0xF0000000u) {           /* rare: reset before wraparound */
        memset(disc, 0, (size_t)NCELLS * sizeof(u32));
        dctr = 0;
    }
    markgen++;
    last_region_gen = markgen;
    last_leafblocks = 0;
    last_ncyc = 0;
    u32 base = dctr + 1;     /* disc >= base <=> discovered this call */
    u32 counter = dctr;
    dctr += (u32)remaining + 1;  /* reserve range (early returns safe) */
    int cnt = 1;
    int esp = 0;
    int leafblocks = 0;
    int root_children = 0;
    int rb_start[5], rb_cuts[5], nrb = 0;   /* root blocks (<=4) */
    int rbn = 0;

    tstack[0] = seed; tdirs[0] = 0; tparent[0] = -1;
    disc[seed] = low[seed] = ++counter;
    sepflag[seed] = 0;
    rlist[0] = seed;
    int top = 1;

    while (top > 0) {
        int u = tstack[top-1];
        u8 di = tdirs[top-1];

        if (di < 4) {
            tdirs[top-1]++;
            int v = u + DELTA[di];
            if (!free_cell(v) || v == tparent[top-1]) continue;
            if (disc[v] >= base) {              /* discovered this call */
                if (disc[v] < disc[u]) {        /* upward back edge */
                    estack[esp] = v; eother[esp] = u; esp++;
                    if (disc[v] < low[u]) low[u] = disc[v];
                }
                continue;
            }
            disc[v] = low[v] = ++counter;
            sepflag[v] = 0;
            rlist[cnt] = v;
            cnt++;
            estack[esp] = v; eother[esp] = u; esp++;   /* tree edge */
            tstack[top] = v; tdirs[top] = 0; tparent[top] = u;
            top++;
        } else {
            top--;
            int p = tparent[top];
            if (p < 0) break;
            if (low[u] < low[p]) low[p] = low[u];
            if (low[u] >= disc[p]) {
                bcgen++;
                if (p == seed) {
                    /* defer: root's cut status unknown until the end */
                    root_children++;
                    rb_start[nrb] = rbn; rb_cuts[nrb] = 0;
                    for (;;) {
                        int a = estack[--esp], b = eother[esp];
                        if (bcmark[a] != bcgen) {
                            bcmark[a] = bcgen;
                            if (a != seed) {
                                rbuf[rbn++] = a;
                                if (sepflag[a]) rb_cuts[nrb]++;
                            }
                        }
                        if (bcmark[b] != bcgen) {
                            bcmark[b] = bcgen;
                            if (b != seed) {
                                rbuf[rbn++] = b;
                                if (sepflag[b]) rb_cuts[nrb]++;
                            }
                        }
                        if (a == u && b == p) break;
                    }
                    nrb++;
                } else {
                    /* interior pop: all flags of this block are final */
                    sepflag[p] = 1;
                    int cuts = 0;
                    int mstart = esp;   /* re-walk range for marking */
                    for (;;) {
                        int a = estack[--esp], b = eother[esp];
                        if (bcmark[a] != bcgen) {
                            bcmark[a] = bcgen;
                            if (sepflag[a]) cuts++;
                        }
                        if (bcmark[b] != bcgen) {
                            bcmark[b] = bcgen;
                            if (sepflag[b]) cuts++;
                        }
                        if (a == u && b == p) break;
                    }
                    if (cuts <= 1) {
                        if (++leafblocks >= 3) { st_p_lb3++; return false; }
                        u8 bid = (u8)(leafblocks - 1);
                        for (int e = esp; e < mstart; e++) {
                            if (!sepflag[estack[e]]) {
                                leafmark[estack[e]] = markgen;
                                lbid[estack[e]] = bid;
                            }
                            if (!sepflag[eother[e]]) {
                                leafmark[eother[e]] = markgen;
                                lbid[eother[e]] = bid;
                            }
                        }
                    }
                }
            }
        }
    }

    work += (u32)cnt >> 3;
    st_region_calls++; st_region_cells += cnt;
    if (cnt != remaining) { st_p_conn++; return false; }

    /* classify deferred root blocks */
    bool root_cut = root_children >= 2;
    for (int i = 0; i < nrb; i++) {
        int cuts = rb_cuts[i] + (root_cut ? 1 : 0);
        if (cuts <= 1) {
            if (++leafblocks >= 3) { st_p_lb3++; return false; }
            u8 bid = (u8)(leafblocks - 1);
            int end = (i + 1 < nrb) ? rb_start[i+1] : rbn;
            for (int j = rb_start[i]; j < end; j++)
                if (!sepflag[rbuf[j]]) {
                    leafmark[rbuf[j]] = markgen;
                    lbid[rbuf[j]] = bid;
                }
            if (!root_cut) { leafmark[seed] = markgen; lbid[seed] = bid; }
        }
    }

    last_leafblocks = leafblocks;

    /* ---- forced-edge analysis ---- */
    if (remaining >= 12) {
        /* forced degree per cell: F(x) = 2 if deg[x]==2 (its own edges),
         * else the number of deg-2 free neighbors forcing an edge on it */
        int overload = 0;
        for (int i = 0; i < cnt; i++) {
            int v = rlist[i];
            if (deg[v] != 2) continue;
            for (int d = 0; d < 4; d++) {
                int x = v + DELTA[d];
                if (!free_cell(x)) continue;
                if (fmark[x] != markgen) { fmark[x] = markgen; fdeg[x] = 0; }
                if (++fdeg[x] > 2 && deg[x] != 2) {
                    if (++overload > 2) { st_p_over++; return false; }
                }
            }
        }

        /* walk forced components: disjoint segments and cycles */
        int ncyc = 0;
        bool cyB[2][2] = {{false, false}, {false, false}};
        bool cyHead[2] = {false, false};
        for (int i = 0; i < cnt; i++) {
            int v = rlist[i];
            if (deg[v] != 2 || cycmark[v] == markgen) continue;
            /* trace from v through cells whose 2 forced edges are known */
            int clen = 0;
            int prev = -1, cur = v;
            bool cycle = false;
            for (;;) {
                cycmark[cur] = markgen;
                rbuf[clen++] = cur;
                /* the forced edges at cur */
                int nxt = -1;
                if (deg[cur] == 2) {
                    for (int d = 0; d < 4; d++) {
                        int x = cur + DELTA[d];
                        if (free_cell(x) && x != prev) { nxt = x; break; }
                    }
                } else {
                    if (fmark[cur] != markgen || fdeg[cur] != 2) break;
                    for (int d = 0; d < 4; d++) {
                        int x = cur + DELTA[d];
                        if (free_cell(x) && deg[x] == 2 && x != prev) { nxt = x; break; }
                    }
                }
                if (nxt < 0) break;            /* segment end */
                if (nxt == v) { cycle = true; break; }
                if (cycmark[nxt] == markgen) break;  /* joins walked part */
                prev = cur; cur = nxt;
            }
            if (!cycle) continue;
            if (ncyc == 2) { st_p_cyc++; return false; }  /* 3 cycles */
            for (int j = 0; j < clen; j++) {
                int c = rbuf[j];
                oncyc[c] = markgen;
                if (leafblocks == 2 && leafmark[c] == markgen)
                    cyB[ncyc][lbid[c]] = true;
            }
            if (head >= 0) {
                for (int d = 0; d < 4; d++) {
                    int x = head + DELTA[d];
                    if (free_cell(x) && oncyc[x] == markgen) {
                        for (int j = 0; j < clen; j++)
                            if (rbuf[j] == x) { cyHead[ncyc] = true; break; }
                    }
                }
            }
            ncyc++;
        }

        /* endpoint-demand feasibility (2 endpoints total) */
        if (leafblocks == 2) {
            if (ncyc == 1 && !cyB[0][0] && !cyB[0][1]) { st_p_feas++; return false; }
            if (ncyc == 2 &&
                !(cyB[0][0] && cyB[1][1]) && !(cyB[0][1] && cyB[1][0])) {
                st_p_feas++; return false;
            }
        } else if (ncyc == 2 && head >= 0) {
            /* one endpoint is the next entered cell (a head neighbor):
             * it must lie on one of the two cycles */
            if (!cyHead[0] && !cyHead[1]) { st_p_feas++; return false; }
        }
        last_ncyc = ncyc;
    }

    return true;
}

static inline bool parity_ok(int pos)
{
    if (remaining == 0) return true;
    int nc = 1 - cellcolor(pos);
    if (remaining & 1)
        return colorbal == (nc == 0 ? 1 : -1);
    return colorbal == 0;
}

/* Structural viability after any slide (cheap, O(1)). */
static inline bool struct_ok(int pos)
{
    if (remaining == 0) return true;
    if (nleaf > 2) return false;
    if (nisol > 1) return false;
    if (nisol == 1) {
        /* lone isolated cell must be adjacent to head (it can only be the
         * final cell, entered directly from pos) */
        if (!((free_cell(pos-1) && deg[pos-1] == 0) ||
              (free_cell(pos+1) && deg[pos+1] == 0) ||
              (free_cell(pos-PW) && deg[pos-PW] == 0) ||
              (free_cell(pos+PW) && deg[pos+PW] == 0)))
            return false;
        if (nleaf > 1) return false; /* isolated cell + >=2 leaves: >2 endpoints */
    }
    return true;
}

/* Set when a slide since the last connectivity verification might have
 * split the free region. Invariant: when false, the free region is one
 * component fully reachable from the head. */
static bool dirty_conn;
static bool always_check;   /* run region_ok at every branch node */
static u32 checktick, checkmask;  /* gate region_ok to every (mask+1)th */

/* Slide and paint; detects split-risk by counting contiguous runs of free
 * cells along both sides of the painted ray (>=2 runs => possible split). */
static inline int do_slide(int pos, int d)
{
    s32 dd = DELTA[d];
    s32 pp = (d == 0 || d == 2) ? PW : 1;   /* perpendicular delta */
    int runs = 0;
    bool pl = false, pr = false;
    do {
        pos += dd;
        visit_cell(pos);
        bool l = free_cell(pos - pp);
        bool r = free_cell(pos + pp);
        if (l && !pl) runs++;
        if (r && !pr) runs++;
        pl = l; pr = r;
    } while (free_cell(pos + dd));
    if (runs >= 2) dirty_conn = true;
    return pos;
}

/* Simulated slide (no state change): end position, length, and number of
 * onward options at the end (perpendicular frees only). */
static inline int sim_slide(int pos, int d, int *len, int *endopts)
{
    s32 dd = DELTA[d];
    s32 pp = (d == 0 || d == 2) ? PW : 1;
    int n = 0;
    do { pos += dd; n++; } while (free_cell(pos + dd));
    *len = n;
    /* perpendicular neighbors of the end; ray cells are not perpendicular
     * to the end cell, so current free state is accurate */
    *endopts = free_cell(pos - pp) + free_cell(pos + pp);
    return pos;
}

static void print_stats(void)
{
    if (!getenv("COIL_STATS")) return;
    fprintf(stderr,
        "stats: region_calls=%llu region_cells=%llu visits=%llu branches=%llu\n"
        "  prunes: parity=%llu struct=%llu conn=%llu lb3=%llu cyc3=%llu over=%llu feas=%llu dirs=%llu tt=%llu\n",
        (unsigned long long)st_region_calls,
        (unsigned long long)st_region_cells,
        (unsigned long long)st_visits,
        (unsigned long long)st_branches,
        (unsigned long long)st_p_parity,
        (unsigned long long)st_p_struct,
        (unsigned long long)st_p_conn,
        (unsigned long long)st_p_lb3,
        (unsigned long long)st_p_cyc,
        (unsigned long long)st_p_over,
        (unsigned long long)st_p_feas,
        (unsigned long long)st_p_dirs,
        (unsigned long long)st_p_tt);
}

static bool dfs(int pos)
{
    for (;;) {
        if (remaining == 0) return true;
        if (aborted) return false;
        if (!struct_ok(pos)) { st_p_struct++; return false; }
        if (!parity_ok(pos)) { st_p_parity++; return false; }

        int dirs[4], nd = 0;
        for (int i = 0; i < 4; i++) {
            int d = dirperm[i];
            if (free_cell(pos + DELTA[d])) dirs[nd++] = d;
        }

        if (nd == 0) return false;

        if (nd == 1) {
            if (nleaf == 2 && deg[pos + DELTA[dirs[0]]] != 1) return false;
            path[pathlen++] = DIRCH[dirs[0]];
            pos = do_slide(pos, dirs[0]);
            continue;
        }

        /* With exactly 2 leaves the path must start at a leaf: restrict
         * to directions whose first cell is a leaf. */
        if (nleaf == 2) {
            int fd[4], fn = 0;
            for (int k = 0; k < nd; k++)
                if (deg[pos + DELTA[dirs[k]]] == 1) fd[fn++] = dirs[k];
            if (fn == 0) { st_p_dirs++; return false; }
            memcpy(dirs, fd, fn * sizeof(int)); nd = fn;
        }

        /* Cheap lookahead BEFORE paying for a branch node: simulate each
         * candidate slide, take instant wins, drop dead-on-arrival ones. */
        int cand[4], ceo[4], nc = 0;
        for (int k = 0; k < nd; k++) {
            int len, eo;
            sim_slide(pos, dirs[k], &len, &eo);
            if (len == remaining) {           /* instant win */
                path[pathlen++] = DIRCH[dirs[k]];
                do_slide(pos, dirs[k]);
                return true;
            }
            if (eo == 0) continue;            /* dead end, not a win: skip */
            cand[nc] = dirs[k]; ceo[nc] = eo; nc++;
        }
        if (nc == 0) return false;

        if (nc == 1) {
            /* pseudo-forced: all alternatives provably dead */
            path[pathlen++] = DIRCH[cand[0]];
            pos = do_slide(pos, cand[0]);
            continue;
        }

        nodes++; work++; st_branches++;
        if (remaining < best_remaining) {
            best_remaining = remaining;
            work_at_best = work;
        }
        if (work > node_budget) { aborted = true; return false; }

        /* transposition: this coverage + head already proven dead? */
        u64 ttkey = zhash ^ zheadkey[pos];
        u32 ttidx = (u32)((ttkey * 0x9e3779b97f4a7c15ULL) >> (64 - TT_BITS));
        if (tt[ttidx] == ttkey) { st_p_tt++; return false; }

        /* Region analysis cadence scales with region size: a scan costs
         * O(remaining), so near the root (huge region, weak structure)
         * scan sparsely; deep down (small region, dense structure) scan
         * every branch node, where scans are cheap and prunes are likely. */
        bool fresh_region = false;
        u32 cadence = (u32)(remaining >> 9);
        if ((dirty_conn || always_check) &&
            (cadence == 0 || (++checktick % (cadence + 1)) == 0)) {
            if (!region_ok(pos)) return false;
            dirty_conn = false;
            fresh_region = true;
        }

        /* The next entered cell is a path endpoint. With exactly 2 leaf
         * blocks it must lie strictly inside one; with 2 forced cycles it
         * must lie on one (each cycle claims an endpoint). */
        if (fresh_region && last_leafblocks == 2) {
            int fd[4], fe[4], fn = 0;
            for (int k = 0; k < nc; k++)
                if (leafmark[pos + DELTA[cand[k]]] == last_region_gen) {
                    fd[fn] = cand[k]; fe[fn] = ceo[k]; fn++;
                }
            if (fn == 0) { st_p_dirs++; return false; }
            memcpy(cand, fd, fn * sizeof(int));
            memcpy(ceo, fe, fn * sizeof(int)); nc = fn;
        }
        if (fresh_region && last_ncyc == 2) {
            int fd[4], fe[4], fn = 0;
            for (int k = 0; k < nc; k++)
                if (oncyc[pos + DELTA[cand[k]]] == last_region_gen) {
                    fd[fn] = cand[k]; fe[fn] = ceo[k]; fn++;
                }
            if (fn == 0) { st_p_dirs++; return false; }
            memcpy(cand, fd, fn * sizeof(int));
            memcpy(ceo, fe, fn * sizeof(int)); nc = fn;
        }

        /* order: forced-continuation ends first, randomized within class */
        int key[4];
        for (int k = 0; k < nc; k++) {
            int kk = (ceo[k] == 1) ? 0 : 8;
            if (randtie) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                kk = kk * 8 + (int)(rng & 7);
            }
            key[k] = kk;
        }
        for (int a = 1; a < nc; a++) {
            int da = cand[a], ka = key[a];
            int b = a - 1;
            while (b >= 0 && key[b] > ka) {
                cand[b+1] = cand[b]; key[b+1] = key[b]; b--;
            }
            cand[b+1] = da; key[b+1] = ka;
        }

        for (int k = 0; k < nc; k++) {
            int saveL = vloglen, saveP = pathlen;
            bool saveD = dirty_conn;
            path[pathlen++] = DIRCH[cand[k]];
            int np = do_slide(pos, cand[k]);
            if (dfs(np)) return true;
            rewind_to(saveL, saveP);
            dirty_conn = saveD;
            if (greedy_probe) { aborted = true; return false; }
        }
        /* fully explored and failed: remember as dead (only if the
         * subtree was not truncated by budget or stall cutoffs) */
        if (!aborted) tt[ttidx] = ttkey;
        return false;
    }
}

static bool solve_from(int start)
{
    int saveL = vloglen, saveP = pathlen;
    dirty_conn = true;          /* removing the start cell may split */
    visit_cell(start);
    nodes = 0; work = 0; aborted = false;
    best_remaining = remaining; work_at_best = 0;
    if (dfs(start)) return true;
    rewind_to(saveL, saveP);
    return false;
}

int main(void)
{
    struct rlimit rl = { (rlim_t)2 << 30, (rlim_t)2 << 30 };
    setrlimit(RLIMIT_STACK, &rl);

    if (scanf("x=%d&y=%d&board=", &W, &H) != 2) {
        fprintf(stderr, "bad input\n");
        return 1;
    }
    PW = W + 2; NCELLS = PW * (H + 2);
    blocked = malloc(NCELLS);
    deg     = calloc(NCELLS, 1);
    mark    = calloc(NCELLS, sizeof(u32));
    vlog    = malloc((size_t)NCELLS * sizeof(s32));
    path    = malloc((size_t)W * H + 16);
    disc    = malloc((size_t)NCELLS * sizeof(u32));
    low     = malloc((size_t)NCELLS * sizeof(u32));
    tstack  = malloc((size_t)NCELLS * sizeof(s32));
    tdirs   = malloc(NCELLS);
    tparent = malloc((size_t)NCELLS * sizeof(s32));
    sepflag = malloc(NCELLS);
    estack  = malloc((size_t)NCELLS * 3 * sizeof(s32));
    eother  = malloc((size_t)NCELLS * 3 * sizeof(s32));
    bcmark  = calloc(NCELLS, sizeof(u32));
    rbuf    = malloc((size_t)NCELLS * sizeof(s32));
    leafmark = calloc(NCELLS, sizeof(u32));
    fdeg    = calloc(NCELLS, 1);
    fmark   = calloc(NCELLS, sizeof(u32));
    rlist   = malloc((size_t)NCELLS * sizeof(s32));
    cycmark = calloc(NCELLS, sizeof(u32));
    oncyc   = calloc(NCELLS, sizeof(u32));
    lbid    = calloc(NCELLS, 1);
    tt      = calloc((size_t)1 << TT_BITS, sizeof(u64));
    zkey    = malloc((size_t)NCELLS * sizeof(u64));
    zheadkey = malloc((size_t)NCELLS * sizeof(u64));
    {
        u64 s = 0x243f6a8885a308d3ULL;
        for (int i = 0; i < NCELLS; i++) {
            s += 0x9e3779b97f4a7c15ULL;
            u64 z = s;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            zkey[i] = z ^ (z >> 31);
            s += 0x9e3779b97f4a7c15ULL;
            z = s;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            zheadkey[i] = z ^ (z >> 31);
        }
    }

    memset(blocked, 1, NCELLS);
    total_empty = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int c = getchar();
            while (c == '\n' || c == '\r') c = getchar();
            if (c == EOF) { fprintf(stderr, "short board\n"); return 1; }
            int p = (y+1)*PW + (x+1);
            if (c == '.') { blocked[p] = 0; total_empty++; }
        }
    }

    DELTA[0] = -1; DELTA[1] = -PW; DELTA[2] = 1; DELTA[3] = PW;

    remaining = total_empty;
    colorbal = 0; nleaf = 0; nisol = 0;
    for (int p = 0; p < NCELLS; p++) {
        if (blocked[p]) continue;
        colorbal += cellcolor(p) ? -1 : 1;
        int d = free_cell(p-1) + free_cell(p+1) + free_cell(p-PW) + free_cell(p+PW);
        deg[p] = (u8)d;
        if (d == 1) nleaf++;
        else if (d == 0) nisol++;
    }

    pathlen = 0; vloglen = 0;

    /* leaves must be path endpoints */
    int leaves[8]; int nleaves = 0;
    for (int p = 0; p < NCELLS && nleaves < 8; p++)
        if (!blocked[p] && deg[p] == 1) leaves[nleaves++] = p;

    if (nleaves >= 3 || nisol > 0) {
        if (!(nisol == 1 && total_empty == 1)) { printf("No solution found\n"); return 0; }
    }

    int need_color = -1;
    if (total_empty & 1) {
        if (colorbal == 1)       need_color = 0;
        else if (colorbal == -1) need_color = 1;
        else { printf("No solution found\n"); return 0; }
    } else if (colorbal != 0) {
        printf("No solution found\n");
        return 0;
    }

    /*
     * Initial block-cut analysis: the path's two endpoints (start cell and
     * final cell) must each lie strictly inside a leaf block of the board
     * graph. With >= 3 leaf blocks the level is unsolvable; with exactly 2
     * the start candidates shrink to the two leaf-block interiors.
     */
    int seed0 = -1;
    for (int p = 0; p < NCELLS && seed0 < 0; p++)
        if (!blocked[p]) seed0 = p;
    bool restrict_lb = false;
    u32 lb_gen = 0;
    if (seed0 >= 0) {
        if (!analyze_region(seed0, -1)) { printf("No solution found\n"); return 0; }
        if (last_leafblocks == 2) { restrict_lb = true; lb_gen = last_region_gen; }
    }

    int *order = malloc((size_t)total_empty * sizeof(int));
    int ns = 0;
    if (nleaves == 2) {
        for (int i = 0; i < 2; i++)
            if (need_color < 0 || cellcolor(leaves[i]) == need_color)
                order[ns++] = leaves[i];
    } else {
        if (nleaves == 1 &&
            (need_color < 0 || cellcolor(leaves[0]) == need_color))
            order[ns++] = leaves[0];
        if (getenv("COIL_STARTS_BORDER")) {
            /* ring order: cells nearer the board border first */
            int maxring = (W < H ? W : H) / 2 + 1;
            for (int ring = 0; ring <= maxring; ring++) {
                for (int p = 0; p < NCELLS; p++) {
                    if (blocked[p]) continue;
                    if (nleaves == 1 && p == leaves[0]) continue;
                    if (need_color >= 0 && cellcolor(p) != need_color) continue;
                    if (restrict_lb && leafmark[p] != lb_gen) continue;
                    int x = p % PW - 1, y = p / PW - 1;
                    int r = x;
                    if (y < r) r = y;
                    if (W-1-x < r) r = W-1-x;
                    if (H-1-y < r) r = H-1-y;
                    if (r == ring) order[ns++] = p;
                }
            }
        } else {
            for (int pass = 1; pass <= 4; pass++) {
                for (int p = 0; p < NCELLS; p++) {
                    if (blocked[p]) continue;
                    if (nleaves == 1 && p == leaves[0]) continue;
                    if (need_color >= 0 && cellcolor(p) != need_color) continue;
                    if (restrict_lb && leafmark[p] != lb_gen) continue;
                    if (deg[p] == pass) order[ns++] = p;
                }
            }
        }
    }

    if (ns == 0) { printf("No solution found\n"); return 0; }

    /* diagnostic: force a single specific start cell */
    if (getenv("COIL_FIXED_START")) {
        int fx, fy;
        if (sscanf(getenv("COIL_FIXED_START"), "%d,%d", &fx, &fy) == 2) {
            order[0] = (fy + 1) * PW + (fx + 1);
            ns = 1;
        }
    }

    bool dbg = getenv("COIL_DEBUG") != NULL;
    if (dbg)
        fprintf(stderr, "init: empty=%d leafblocks=%d restrict=%d starts=%d\n",
                total_empty, last_leafblocks, (int)restrict_lb, ns);
    always_check = getenv("COIL_LAZY_CHECK") == NULL;  /* default: always */
    order_mode = getenv("COIL_ORDER") ? atoi(getenv("COIL_ORDER")) : 0;
    randtie = getenv("COIL_NORANDTIE") == NULL;        /* default: on */
    checkmask = getenv("COIL_CHECK_EVERY") ? (u32)atoi(getenv("COIL_CHECK_EVERY")) - 1 : 3;
    if (getenv("COIL_SEED"))
        seed_salt ^= (u64)strtoull(getenv("COIL_SEED"), NULL, 10) * 0xc2b2ae3d27d4eb4fULL;
    else
        seed_salt ^= ((u64)time(NULL) * 0xc2b2ae3d27d4eb4fULL) ^ ((u64)getpid() << 32);

    /* Shuffle the start order (only useful for randomized-restart modes;
     * the default two-pass sweep is deterministic and complete). */
    if (getenv("COIL_SHUFFLE") != NULL && nleaves < 2) {
        int lo = (nleaves == 1) ? 1 : 0;
        u64 r = seed_salt ^ 0xa076bca5915f7445ULL;
        for (int i = ns - 1; i > lo; i--) {
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            int j = lo + (int)(r % (u64)(i - lo + 1));
            int t = order[i]; order[i] = order[j]; order[j] = t;
        }
    }
    int nworkers = 4;
    {
        const char *env = getenv("COIL_WORKERS");
        if (env) nworkers = atoi(env);
        if (nworkers < 1) nworkers = 1;
        if (nworkers > ns) nworkers = ns;
    }

    int pipes[16][2];
    pid_t kids[16];
    for (int w = 0; w < nworkers; w++) {
        if (pipe(pipes[w]) != 0) { perror("pipe"); return 1; }
        pid_t pid = nworkers > 1 ? fork() : 0;
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            /* worker: handle starts w, w+nworkers, ... */
            prctl(PR_SET_PDEATHSIG, SIGKILL);   /* die with parent */
            if (getppid() == 1) _exit(1);       /* parent already gone */
            if (nworkers > 1) {
                for (int j = 0; j <= w; j++) close(pipes[j][0]);
                /* don't hold the harness's stdout/stderr pipes open:
                 * a lingering worker must never block its cleanup */
                close(1);
                if (!dbg) {
                    int devnull = open("/dev/null", O_WRONLY);
                    if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
                }
            }
            FILE *out = (nworkers > 1) ? fdopen(pipes[w][1], "w") : stdout;
            for (int i = 0; i < 4; i++) dirperm[i] = (i + w) & 3;
            if (!getenv("COIL_CHECK_EVERY") && nworkers > 1) {
                /* diversify region-check cadence across workers */
                static const u32 kdiv[4] = {3, 1, 7, 11};
                checkmask = kdiv[w & 3];
            }
            if (!getenv("COIL_ORDER")) {
                /* diversify slide-length tiebreak: w2 long, w3 short */
                static const int omode[4] = {0, 0, 1, 2};
                order_mode = omode[w & 3];
            }

            /*
             * Progress-guided restarts: tier 0 probes every start with a
             * small budget and records how deep each attempt got (fewest
             * cells left). Each later tier keeps only the most promising
             * half of the field (at least 16), re-ordered by progress,
             * with 8x the budget and fresh randomized tie-breaking.
             */
            int nmine = 0;
            int *act  = malloc((size_t)ns * sizeof(int));
            int *prog = malloc((size_t)ns * sizeof(int));
            for (int k = w; k < ns; k += nworkers) act[nmine++] = k;

            if (getenv("COIL_TIERS") == NULL) {
                /*
                 * Three-pass sweep, deterministic and complete.
                 *
                 * pass 0 (probe): every start, tiny budget. Cheap dead
                 *   starts are eliminated outright; survivors record how
                 *   deep the probe got. Winning starts probe DEEP: their
                 *   probe depth ranks them near the top of the field.
                 * pass 1 (guided): survivors in probe-depth order with a
                 *   budget sized to known winner costs - the winner is
                 *   normally found within the first few attempts.
                 * pass 2 (exhaust): anything still alive, deepest first,
                 *   unbounded - keeps the sweep complete so the true
                 *   solution can never be skipped.
                 *
                 * The transposition table persists across passes, so
                 * revisiting a start re-uses all completed dead subtrees
                 * instead of re-deriving them.
                 */
                u64 p0cap = getenv("COIL_P0CAP")
                          ? (u64)strtoull(getenv("COIL_P0CAP"), NULL, 10)
                          : 8;
                u64 p1cap = getenv("COIL_P1CAP")
                          ? (u64)strtoull(getenv("COIL_P1CAP"), NULL, 10)
                          : 450;

                int nrounds = getenv("COIL_P0R") ? atoi(getenv("COIL_P0R")) : 1;
                bool use_probe = getenv("COIL_PROBE") != NULL;
                int alive = 0;
                node_budget = p0cap * (u64)total_empty;
                greedy_probe = true;
                if (!use_probe) {
                    /* probe ordering disabled: keep constructed order */
                    for (int i = 0; i < nmine; i++) prog[i] = 0;
                    alive = nmine;
                }
                for (int i = 0; use_probe && i < nmine; i++) {
                    int k = act[i];
                    bool ok = false, anyalive = false;
                    int bestp = total_empty + 1;
                    for (int r = 0; r < nrounds && !ok; r++) {
                        rng = seed_salt ^ ((u64)(k+1) << 16) ^ ((u64)r << 48);
                        rng ^= rng >> 33; rng *= 0xff51afd7ed558ccdULL; rng ^= rng >> 33;
                        ok = solve_from(order[k]);
                        if (!ok && aborted) {
                            anyalive = true;
                            if (best_remaining < bestp) bestp = best_remaining;
                        }
                        if (!ok && !aborted) { anyalive = false; break; } /* proven dead */
                    }
                    if (dbg)
                        fprintf(stderr, "w%d pass0 start#%d/%d pos=(%d,%d) best=%d %s\n",
                                w, k, ns, order[k] % PW - 1, order[k] / PW - 1,
                                bestp,
                                ok ? "SOLVED" : (anyalive ? "alive" : "dead"));
                    if (ok) {
                        int sx = order[k] % PW - 1, sy = order[k] / PW - 1;
                        path[pathlen] = 0;
                        fprintf(out, "x=%d&y=%d&path=%s\n", sx, sy, path);
                        fflush(out);
                        print_stats();
                        _exit(0);
                    }
                    if (anyalive) {
                        act[alive] = k; prog[alive] = bestp; alive++;
                    }
                }
                greedy_probe = false;

                for (int pass = 1; pass <= 2; pass++) {
                    /* deepest probe progress first */
                    for (int a = 1; a < alive; a++) {
                        int ka = act[a], pa = prog[a], b = a - 1;
                        while (b >= 0 && prog[b] > pa) {
                            act[b+1] = act[b]; prog[b+1] = prog[b]; b--;
                        }
                        act[b+1] = ka; prog[b+1] = pa;
                    }
                    node_budget = (pass == 1) ? p1cap * (u64)total_empty
                                              : (u64)1 << 60;
                    int kept = 0;
                    for (int i = 0; i < alive; i++) {
                        int k = act[i];
                        rng = seed_salt ^ ((u64)(k+1) << 16) ^ ((u64)pass << 56);
                        rng ^= rng >> 33; rng *= 0xff51afd7ed558ccdULL; rng ^= rng >> 33;
                        bool ok = solve_from(order[k]);
                        if (dbg)
                            fprintf(stderr, "w%d pass%d start#%d/%d pos=(%d,%d) nodes=%llu best=%d %s\n",
                                    w, pass, k, ns, order[k] % PW - 1, order[k] / PW - 1,
                                    (unsigned long long)nodes, best_remaining,
                                    ok ? "SOLVED" : (aborted ? "alive" : "dead"));
                        if (ok) {
                            int sx = order[k] % PW - 1, sy = order[k] / PW - 1;
                            path[pathlen] = 0;
                            fprintf(out, "x=%d&y=%d&path=%s\n", sx, sy, path);
                            fflush(out);
                            print_stats();
                            _exit(0);
                        }
                        if (aborted) {
                            act[kept] = k; prog[kept] = best_remaining; kept++;
                        }
                    }
                    alive = kept;
                }
                if (nworkers == 1) printf("No solution found\n");
                print_stats();
                _exit(1);
            }

            u64 scale = 1 + (u64)total_empty / 8;
            u64 budget = 1000;
            for (int bi = 0; nmine > 0; bi++, budget *= 8) {
                node_budget = budget * scale;
                int kept = 0;
                for (int i = 0; i < nmine; i++) {
                    int k = act[i];
                    rng = seed_salt ^ ((u64)(w+1) << 40)
                        ^ ((u64)(k+1) << 16) ^ (u64)(bi+1);
                    rng ^= rng >> 33; rng *= 0xff51afd7ed558ccdULL; rng ^= rng >> 33;
                    bool ok = solve_from(order[k]);
                    if (dbg)
                        fprintf(stderr, "w%d tier%d start#%d/%d pos=(%d,%d) nodes=%llu best=%d %s\n",
                                w, bi, k, ns, order[k] % PW - 1, order[k] / PW - 1,
                                (unsigned long long)nodes, best_remaining,
                                ok ? "SOLVED" : (aborted ? "abort" : "dead"));
                    if (ok) {
                        int sx = order[k] % PW - 1, sy = order[k] / PW - 1;
                        path[pathlen] = 0;
                        fprintf(out, "x=%d&y=%d&path=%s\n", sx, sy, path);
                        fflush(out);
                        if (getenv("COIL_STATS"))
                            fprintf(stderr,
                                "w%d stats: region_calls=%llu region_cells=%llu visits=%llu branches=%llu\n"
                                "  prunes: parity=%llu struct=%llu conn=%llu lb3=%llu cyc3=%llu over=%llu feas=%llu dirs=%llu tt=%llu\n",
                                w,
                                (unsigned long long)st_region_calls,
                                (unsigned long long)st_region_cells,
                                (unsigned long long)st_visits,
                                (unsigned long long)st_branches,
                                (unsigned long long)st_p_parity,
                                (unsigned long long)st_p_struct,
                                (unsigned long long)st_p_conn,
                                (unsigned long long)st_p_lb3,
                                (unsigned long long)st_p_cyc,
                                (unsigned long long)st_p_over,
                                (unsigned long long)st_p_feas,
                                (unsigned long long)st_p_dirs,
                                (unsigned long long)st_p_tt);
                        _exit(0);
                    }
                    if (aborted) {              /* keep with its progress */
                        act[kept] = k; prog[kept] = best_remaining; kept++;
                    }
                    /* exhausted (not aborted): proven dead, dropped */
                }
                /* most promising first; shrink the field gradually */
                for (int a = 1; a < kept; a++) {
                    int ka = act[a], pa = prog[a], b = a - 1;
                    while (b >= 0 && prog[b] > pa) {
                        act[b+1] = act[b]; prog[b+1] = prog[b]; b--;
                    }
                    act[b+1] = ka; prog[b+1] = pa;
                }
                if (kept > 16 && bi >= 1) {
                    kept = kept / 2 > 16 ? kept / 2 : 16;
                }
                nmine = kept;
            }
            if (nworkers == 1) printf("No solution found\n");
            _exit(1);   /* no solution from this worker's starts */
        }
        kids[w] = pid;
        close(pipes[w][1]);
    }

    /* parent: wait for first worker that writes a solution */
    int finished = 0;
    bool solved = false;
    while (finished < nworkers && !solved) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int w = 0; w < nworkers; w++) {
            if (pipes[w][0] >= 0) {
                FD_SET(pipes[w][0], &rfds);
                if (pipes[w][0] > maxfd) maxfd = pipes[w][0];
            }
        }
        if (maxfd < 0) break;
        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) <= 0) break;
        for (int w = 0; w < nworkers; w++) {
            if (pipes[w][0] < 0 || !FD_ISSET(pipes[w][0], &rfds)) continue;
            static char buf[1 << 16];
            ssize_t got = read(pipes[w][0], buf, sizeof(buf));
            if (got > 0) {
                /* winner: stream entire solution through */
                fwrite(buf, 1, got, stdout);
                while ((got = read(pipes[w][0], buf, sizeof(buf))) > 0)
                    fwrite(buf, 1, got, stdout);
                solved = true;
                break;
            } else {
                close(pipes[w][0]);
                pipes[w][0] = -1;
                finished++;
            }
        }
    }

    for (int w = 0; w < nworkers; w++) {
        if (kids[w] > 0) kill(kids[w], SIGKILL);
    }
    while (wait(NULL) > 0) {}

    if (!solved) printf("No solution found\n");
    return 0;
}

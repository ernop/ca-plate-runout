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
typedef uint16_t u16;
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
/*
 * Op accounting: every interaction with the board state costs ops,
 * weighted by realistic CPU/memory cost. The benchmark currency is ops,
 * not wall time; runs stop deterministically at COIL_OPS_LIMIT.
 *   visit/unvisit cell   8  (state, parity, hash, 4 neighbor degrees)
 *   slide sim step       1  (one cell read)
 *   branch node          12 (enumeration, ordering, TT probe)
 *   region scan, /cell   6  (Tarjan + forced-edge bookkeeping)
 *   lobe build, /cell    4
 *   lobe search node     6
 *   start attempt        16
 */
static u64 ops, ops_limit;
static bool ops_out;         /* op budget exhausted: stop everything */

/* where do ops go, by depth? bucket = 8*remaining/total (7=near start) */
static u64 hist_region[8], hist_visit[8], hist_branch[8];
static u64 hist_rcall[8], hist_rprune[8], hist_pconn[8];
/* fragmentation per depth bucket (struct mode): avg largest-block share
 * of the region (permil) and avg block count, per scan */
static u64 frag_share[8], frag_blocks[8], frag_n[8];
/* ITER 7: size profile of CLASSIFIED leaf blocks (claim sources) */
static u64 st_leaf_cnt, st_leaf_cells, st_leaf_max, st_leaf_giant;
/* ITER 8: local-state redundancy probe. Canonical hash of the KxK
 * neighborhood around the head at every branch node; counts total vs
 * distinct. Instrumentation only (COIL_LH), charged no ops. */
static u64 *lh_set;
static u64 lh_total, lh_distinct;
static bool use_lh;
#define LH_BITS 22
static void lh_probe(int pos)
{
    u64 h = 0x9e3779b97f4a7c15ULL;
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++) {
            h = (h << 1) | (h >> 63);
            h ^= blocked[pos + dy * PW + dx] ? 0xff51afd7ed558ccdULL : 0;
            h *= 0xc2b2ae3d27d4eb4fULL;
        }
    lh_total++;
    u32 idx = (u32)(h >> (64 - LH_BITS));
    for (int probe = 0; probe < 8; probe++) {
        u32 i = (idx + probe) & ((1u << LH_BITS) - 1);
        if (lh_set[i] == h) return;
        if (lh_set[i] == 0) { lh_set[i] = h; lh_distinct++; return; }
    }
    lh_distinct++;   /* table pressure: counted distinct (conservative) */
}

#define LSEED_MAX 8
#define LFLOOD_CAP 64
static s32 lseeds[LSEED_MAX];
static int nlseeds;
static bool lseeds_valid;
static bool prev_dirty;      /* dirty state before the last slide */

/*
 * Active rind: free cells within Manhattan distance <= 2 of any visited
 * cell - the only place where carving can create new cut/block structure
 * (plus the static structure of the virgin board, known from the initial
 * analysis). Maintained by reference counting: every visit increments the
 * count of the 12 cells around it, every unvisit decrements; membership
 * is rindcnt > 0. A swap-delete list gives O(rind) enumeration.
 */
static u16 *rindcnt;
static s32 *rindlist;        /* cells with rindcnt > 0 */
static s32 *rindidx;         /* cell -> position in rindlist, or -1 */
static int  nrind;
static bool use_rind;
static const int RIND_OFF_N = 12;
static s32 rind_off[12];     /* filled in main once PW is known */

static inline void rind_add(int q)
{
    if (rindcnt[q]++ == 0) {
        rindidx[q] = nrind;
        rindlist[nrind++] = q;
    }
}
static inline void rind_del(int q)
{
    if (--rindcnt[q] == 0) {
        int i = rindidx[q];
        int last = rindlist[--nrind];
        rindlist[i] = last;
        rindidx[last] = i;
        rindidx[q] = -1;
    }
}

static u64 st_region_calls, st_region_cells, st_visits, st_branches;
static u64 st_p_parity, st_p_struct, st_p_conn, st_p_lb3, st_p_cyc,
           st_p_over, st_p_feas, st_p_dirs, st_p_tt, st_p_lbpar;

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
    if (use_rind) {
        for (int i = 0; i < RIND_OFF_N; i++) rind_add(p + rind_off[i]);
        ops += RIND_OFF_N;
    }
    ops += 8;
    hist_visit[(remaining * 7) / (total_empty + 1)] += 8;
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
        if (use_rind) {
            for (int i = 0; i < RIND_OFF_N; i++) rind_del(p + rind_off[i]);
            ops += RIND_OFF_N;
        }
        ops += 8;
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

/*
 * Stage A of incremental structure: every full scan RECORDS the block
 * decomposition instead of discarding it. blkid[cell] = block ordinal
 * for non-cut cells, cutflag for cut vertices, both generation-tagged
 * with the scan's markgen. Verified by invariants under COIL_PARANOID.
 * Stage B (next): delta re-analysis of only the blocks touched by a ray.
 */
static u32 *blkid;           /* block ordinal (gen-tagged via blkgenof) */
static u32 *blkgenof;        /* generation of blkid/cutflag validity */
static u8  *cutflag;
static int  nblocks;
static int *blk_size;
static u8  *blk_leaf;
static bool use_struct;      /* record decomposition (Stage A) */
static bool paranoid;

static inline void blk_tag(int cell, int ord, bool is_cut)
{
    if (!use_struct) return;
    blkgenof[cell] = markgen;
    cutflag[cell] = is_cut;
    if (!is_cut) blkid[cell] = (u32)ord;
}

/*
 * Pendant-lobe micro-solver.
 *
 * A leaf block B with cut vertex c is self-contained: by definition no
 * free edge leaves B except through c, so slide dynamics inside B depend
 * only on B's own cell set. If B is small, the question "can a path that
 * enters B at cell f moving in direction d cover ALL of B?" is decided
 * exactly by a tiny search, and the verdict depends only on (B, f, d).
 * Verdicts are cached by content hash; if no possible entry mode can
 * cover B, every state containing this pocket is dead.
 */
#define LOBE_MAX 44
#define LOBE_NODE_CAP 60000
static int  lb_n;                     /* cells in current lobe */
static s32  lb_cells[LOBE_MAX + 4];
static int  lb_nbr[LOBE_MAX + 4][4];  /* local adjacency (-1 = blocked) */
static u32 *lobemark;                 /* global cell -> lobe slot + 1 */
static u32  lobegen;
static u32 *lobeslot;
static u64  lb_full;
static u64  lb_need;    /* required coverage: all cells, cut vertex optional
                           (it can be covered by a pass-by from outside) */
static int  lb_micro_nodes;

/* verdict cache: key -> feasible / infeasible */
#define LOBE_TT_BITS 20
static u64 *lobe_feas, *lobe_infeas;
static u64 st_lobe_solves, st_lobe_hits, st_p_lobe, st_p_chain;
static bool use_chain;      /* sound; ~1% net ops loss at <=104x102:
                               through-traversals are rarely impossible
                               at benchmark scale. Re-evaluate on larger
                               boards where corridor chains multiply. */

static int lb_end = -1;     /* required final slot (-1 = anywhere) */

static bool lobe_dfs(u64 mask, int p)
{
    ops += 6;
    if ((mask & lb_need) == lb_need && (lb_end < 0 || p == lb_end))
        return true;
    if (++lb_micro_nodes > LOBE_NODE_CAP) return true; /* unknown: no prune */
    for (int d = 0; d < 4; d++) {
        int q = lb_nbr[p][d];
        if (q < 0 || (mask >> q) & 1) continue;
        u64 m = mask;
        int r = q;
        m |= (u64)1 << r;
        for (;;) {
            int nx = lb_nbr[r][d];
            if (nx < 0 || (m >> nx) & 1) break;
            r = nx; m |= (u64)1 << r;
        }
        if (lobe_dfs(m, r)) return true;
    }
    return false;
}

/* can a path entering the lobe at slot f, moving in direction d (slide
 * continues), cover the whole lobe? */
static bool lobe_entry_feasible(u64 chash, int f, int d)
{
    u64 key = chash ^ ((u64)(f * 4 + d + 1) * 0xc2b2ae3d27d4eb4fULL);
    key ^= key >> 29; key *= 0x9e3779b97f4a7c15ULL; key ^= key >> 32;
    u32 idx = (u32)(key >> (64 - LOBE_TT_BITS));
    if (lobe_feas[idx] == key)   { st_lobe_hits++; return true; }
    if (lobe_infeas[idx] == key) { st_lobe_hits++; return false; }

    /* enter at f and slide along d as far as possible */
    u64 mask = (u64)1 << f;
    int r = f;
    for (;;) {
        int nx = lb_nbr[r][d];
        if (nx < 0 || (mask >> nx) & 1) break;
        r = nx; mask |= (u64)1 << r;
    }
    lb_micro_nodes = 0;
    st_lobe_solves++;
    bool ok = lobe_dfs(mask, r);
    bool capped = lb_micro_nodes > LOBE_NODE_CAP;
    if (!capped) {
        if (ok) lobe_feas[idx] = key; else lobe_infeas[idx] = key;
    }
    return ok || capped;
}

/* Build local structure for a lobe given its cell list (set by caller in
 * lb_cells/lb_n); returns its content hash. */
static u64 lobe_build(void)
{
    lobegen++;
    ops += (u64)lb_n * 4;
    u64 h = 0;
    for (int i = 0; i < lb_n; i++) {
        lobemark[lb_cells[i]] = lobegen;
        lobeslot[lb_cells[i]] = (u32)i;
        h ^= zkey[lb_cells[i]];
    }
    for (int i = 0; i < lb_n; i++) {
        int p = lb_cells[i];
        for (int d = 0; d < 4; d++) {
            int q = p + DELTA[d];
            lb_nbr[i][d] = (!blocked[q] && lobemark[q] == lobegen)
                         ? (int)lobeslot[q] : -1;
        }
    }
    lb_full = (lb_n >= 64) ? ~(u64)0 : (((u64)1 << lb_n) - 1);
    return h;
}

/* micro-search helper: start at slot s (path occupies it), slide d,
 * then free search; cached by (content ^ salt). */
static bool lobe_mode(u64 chash, u64 salt, int s, int d)
{
    u64 key = chash ^ salt;
    key ^= key >> 29; key *= 0x9e3779b97f4a7c15ULL; key ^= key >> 32;
    u32 idx = (u32)(key >> (64 - LOBE_TT_BITS));
    if (lobe_feas[idx] == key)   { st_lobe_hits++; return true; }
    if (lobe_infeas[idx] == key) { st_lobe_hits++; return false; }
    u64 mask = (u64)1 << s;
    int r = s;
    for (;;) {
        int nx = lb_nbr[r][d];
        if (nx < 0 || (mask >> nx) & 1) break;
        r = nx; mask |= (u64)1 << r;
    }
    lb_micro_nodes = 0;
    st_lobe_solves++;
    bool ok = lobe_dfs(mask, r);
    bool capped = lb_micro_nodes > LOBE_NODE_CAP;
    if (!capped) {
        if (ok) lobe_feas[idx] = key; else lobe_infeas[idx] = key;
    }
    return ok || capped;
}

/*
 * Chain-interior block B with exactly two cut vertices c1, c2 and cells
 * beyond both: the path must either traverse B through (enter one cut,
 * cover everything, leave by the other) or place BOTH global endpoints
 * inside B. If the head is not adjacent to any B cell, the next-entered
 * endpoint cannot be in B, so only: through, or one excursion from a cut
 * covering everything (other cut coverable by an outside pass-by) and
 * ending inside (the final endpoint). All modes impossible => dead.
 * Verdicts depend only on (B, c1, c2): content-cached.
 */
static bool chain_check(int c1, int c2, int head)
{
    u64 chash = lobe_build() ^ zheadkey[c1] ^ (zheadkey[c2] * 3);
    int s1 = (int)lobeslot[c1], s2 = (int)lobeslot[c2];

    if (head >= 0) {
        for (int d = 0; d < 4; d++) {
            int nb = head + DELTA[d];
            if (!blocked[nb] && lobemark[nb] == lobegen) return true;
        }
    }

    /* through c1 -> c2 and c2 -> c1: full coverage, fixed final cell */
    lb_need = lb_full;
    for (int pass = 0; pass < 2; pass++) {
        int sa = pass ? s2 : s1, sb = pass ? s1 : s2;
        lb_end = sb;
        for (int d = 0; d < 4; d++) {
            if (lb_nbr[sa][d] < 0) continue;
            if (lobe_mode(chash, (u64)(pass*41 + d*7 + 11) * 0x165667b19e3779f9ULL,
                          sa, d)) { lb_end = -1; return true; }
        }
    }
    lb_end = -1;

    /* one excursion from a cut, other cut optional, end anywhere */
    for (int pass = 0; pass < 2; pass++) {
        int sa = pass ? s2 : s1, sb = pass ? s1 : s2;
        lb_need = lb_full & ~((u64)1 << sb);
        for (int d = 0; d < 4; d++) {
            if (lb_nbr[sa][d] < 0) continue;
            if (lobe_mode(chash, (u64)(pass*43 + d*13 + 401) * 0xc2b2ae3d27d4eb4fULL,
                          sa, d)) return true;
        }
    }
    st_p_chain++;
    return false;
}

/*
 * Check one pendant lobe: B = block cells (including cut vertex c).
 * head: current search head (or -1). Returns false => current state dead.
 *
 * Cache keys are order-independent (content hash ^ entry cell key ^
 * direction), so identical pockets reached by different search paths
 * share verdicts. The cache is probed for every entry mode BEFORE any
 * local structure is built; adjacency is constructed only when at least
 * one mode is unknown.
 */
static bool lobe_check(int c, int head)
{
    /* content hash + membership marks: 2 ops per cell */
    lobegen++;
    u64 chash = zheadkey[c];
    for (int i = 0; i < lb_n; i++) {
        chash ^= zkey[lb_cells[i]];
        lobemark[lb_cells[i]] = lobegen;
    }
    ops += (u64)lb_n * 2;

    /* enumerate entry modes: (entry cell, direction, salt-class) */
    int mf[8], md[8], nm = 0;
    for (int d = 0; d < 4; d++) {
        int q = c + DELTA[d];
        if (!blocked[q] && lobemark[q] == lobegen) {
            mf[nm] = c; md[nm] = d; nm++;
        }
    }
    if (head >= 0) {
        for (int d = 0; d < 4; d++) {
            int f = head + DELTA[d];
            if (!blocked[f] && lobemark[f] == lobegen && f != c) {
                mf[nm] = f; md[nm] = d; nm++;
            }
        }
    }
    if (nm == 0) { st_p_lobe++; return false; }

    /* probe all modes first */
    u64 keys[8]; u32 idxs[8];
    int unknown = 0;
    for (int m = 0; m < nm; m++) {
        u64 key = chash ^ (zkey[mf[m]] * 0x165667b19e3779f9ULL)
                        ^ ((u64)(md[m] + 1) * 0xc2b2ae3d27d4eb4fULL);
        key ^= key >> 29; key *= 0x9e3779b97f4a7c15ULL; key ^= key >> 32;
        keys[m] = key;
        idxs[m] = (u32)(key >> (64 - LOBE_TT_BITS));
        ops += 2;
        if (lobe_feas[idxs[m]] == key) { st_lobe_hits++; return true; }
        if (lobe_infeas[idxs[m]] == key) { st_lobe_hits++; keys[m] = 0; }
        else unknown++;
    }
    if (unknown == 0) { st_p_lobe++; return false; }

    /* at least one unknown mode: build local adjacency and solve them */
    lobe_build();
    int cslot = (int)lobeslot[c];
    lb_need = lb_full & ~((u64)1 << cslot);
    for (int m = 0; m < nm; m++) {
        if (keys[m] == 0) continue;          /* cached-infeasible */
        int s = (int)lobeslot[mf[m]], d = md[m];
        u64 mask = (u64)1 << s;
        int r = s;
        for (;;) {
            int nx = lb_nbr[r][d];
            if (nx < 0 || (mask >> nx) & 1) break;
            r = nx; mask |= (u64)1 << r;
        }
        lb_micro_nodes = 0;
        st_lobe_solves++;
        bool ok = lobe_dfs(mask, r);
        bool capped = lb_micro_nodes > LOBE_NODE_CAP;
        if (!capped) {
            if (ok) lobe_feas[idxs[m]] = keys[m];
            else    lobe_infeas[idxs[m]] = keys[m];
        }
        if (ok || capped) return true;
    }
    st_p_lobe++;
    return false;
}

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
    nblocks = 0;
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
                    int cuts = 0, blkverts = 0, bbal = 0, c_other = -1;
                    int ord = nblocks++;
                    int mstart = esp;   /* re-walk range for marking */
                    for (;;) {
                        int a = estack[--esp], b = eother[esp];
                        if (bcmark[a] != bcgen) {
                            bcmark[a] = bcgen;
                            blk_tag(a, ord, sepflag[a]);
                            if (blkverts < LOBE_MAX) lb_cells[blkverts] = a;
                            blkverts++;
                            bbal += cellcolor(a) ? -1 : 1;
                            if (sepflag[a]) { cuts++; if (a != p) c_other = a; }
                        }
                        if (bcmark[b] != bcgen) {
                            bcmark[b] = bcgen;
                            blk_tag(b, ord, sepflag[b]);
                            if (blkverts < LOBE_MAX) lb_cells[blkverts] = b;
                            blkverts++;
                            bbal += cellcolor(b) ? -1 : 1;
                            if (sepflag[b]) { cuts++; if (b != p) c_other = b; }
                        }
                        if (a == u && b == p) break;
                    }
                    if (use_struct) {
                        blk_size[ord] = blkverts;
                        blk_leaf[ord] = (cuts <= 1);
                        ops += (u64)blkverts;
                    }
                    if (use_chain && cuts == 2 && c_other >= 0 &&
                        blkverts <= LOBE_MAX && blkverts < remaining) {
                        lb_n = blkverts;
                        if (!chain_check(p, c_other, head)) return false;
                    }
                    if (cuts <= 1 && blkverts < remaining) {
                        st_leaf_cnt++;
                        st_leaf_cells += (u64)blkverts;
                        if ((u64)blkverts > st_leaf_max) st_leaf_max = blkverts;
                        if (blkverts * 2 > remaining) st_leaf_giant++;
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
                        /* Parity refutation, any lobe size, entry-free:
                         * the lobe path covers B (or B minus the cut, if
                         * the cut is covered by an outside pass-by). An
                         * alternating path of n cells needs balance 0
                         * (n even) or +-1 (n odd). Both options failing
                         * kills the state. */
                        {
                            int pb = cellcolor(p) ? -1 : 1;
                            int nB = blkverts,      balB = bbal;
                            int nP = blkverts - 1,  balP = bbal - pb;
                            bool okB = (nB & 1) ? (balB == 1 || balB == -1)
                                                : (balB == 0);
                            bool okP = (nP & 1) ? (balP == 1 || balP == -1)
                                                : (balP == 0);
                            if (!okB && !okP) { st_p_lbpar++; return false; }
                        }
                        /* pendant pocket: micro-refute if small */
                        if (blkverts <= LOBE_MAX) {
                            lb_n = blkverts;
                            if (!lobe_check(p, head)) return false;
                        }
                    } else if (cuts <= 1) {
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
    ops += (u64)cnt * 6;
    hist_region[(remaining * 7) / (total_empty + 1)] += (u64)cnt * 6;
    if (cnt != remaining) {
        st_p_conn++;
        hist_pconn[(remaining * 7) / (total_empty + 1)]++;
        return false;
    }

    /* classify deferred root blocks */
    bool root_cut = root_children >= 2;
    for (int i = 0; i < nrb; i++) {
        int cuts = rb_cuts[i] + (root_cut ? 1 : 0);
        int end = (i + 1 < nrb) ? rb_start[i+1] : rbn;
        if (use_struct) {
            int ord = nblocks++;
            for (int j = rb_start[i]; j < end; j++)
                blk_tag(rbuf[j], ord, sepflag[rbuf[j]]);
            blk_tag(seed, ord, root_cut);
            blk_size[ord] = end - rb_start[i] + 1;
            blk_leaf[ord] = (cuts <= 1);
            ops += (u64)(end - rb_start[i] + 1);
        }
        if (cuts <= 1) {
            {
                int bv = end - rb_start[i] + 1;
                st_leaf_cnt++;
                st_leaf_cells += (u64)bv;
                if ((u64)bv > st_leaf_max) st_leaf_max = bv;
                if (bv * 2 > remaining) st_leaf_giant++;
            }
            if (++leafblocks >= 3) { st_p_lb3++; return false; }
            u8 bid = (u8)(leafblocks - 1);
            for (int j = rb_start[i]; j < end; j++)
                if (!sepflag[rbuf[j]]) {
                    leafmark[rbuf[j]] = markgen;
                    lbid[rbuf[j]] = bid;
                }
            if (!root_cut) { leafmark[seed] = markgen; lbid[seed] = bid; }

            int blkverts = end - rb_start[i] + 1;   /* + seed */
            if (root_cut && blkverts < remaining) {
                /* entry-free parity refutation (any size) */
                int bbal = cellcolor(seed) ? -1 : 1;
                for (int j = rb_start[i]; j < end; j++)
                    bbal += cellcolor(rbuf[j]) ? -1 : 1;
                int pb = cellcolor(seed) ? -1 : 1;
                int nB = blkverts,     balB = bbal;
                int nP = blkverts - 1, balP = bbal - pb;
                bool okB = (nB & 1) ? (balB == 1 || balB == -1) : (balB == 0);
                bool okP = (nP & 1) ? (balP == 1 || balP == -1) : (balP == 0);
                if (!okB && !okP) { st_p_lbpar++; return false; }

                /* pendant pocket hanging off the root: micro-refute */
                if (blkverts <= LOBE_MAX) {
                    for (int j = rb_start[i]; j < end; j++)
                        lb_cells[j - rb_start[i]] = rbuf[j];
                    lb_cells[blkverts - 1] = seed;
                    lb_n = blkverts;
                    if (!lobe_check(seed, head)) return false;
                }
            }
        }
    }

    last_leafblocks = leafblocks;

    if (use_struct) {
        int big = 0;
        for (int i = 0; i < nblocks; i++)
            if (blk_size[i] > big) big = blk_size[i];
        int hb = (remaining * 7) / (total_empty + 1);
        frag_share[hb] += (u64)big * 1000 / (cnt ? cnt : 1);
        frag_blocks[hb] += (u64)nblocks;
        frag_n[hb]++;
        ops += (u64)nblocks;
    }

    /* Stage A invariant verification: every free cell tagged this scan,
     * non-cut ordinals valid. Catches bookkeeping drift immediately. */
    if (use_struct && paranoid) {
        for (int i = 0; i < cnt; i++) {
            int v = rlist[i];
            if (blkgenof[v] != markgen ||
                (!cutflag[v] && (int)blkid[v] >= nblocks)) {
                fprintf(stderr, "PARANOID FAIL cell=%d gen=%u/%u cut=%d id=%u nb=%d\n",
                        v, blkgenof[v], markgen, cutflag[v], blkid[v], nblocks);
                _exit(9);
            }
        }
    }

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
static u32 checktick, checkmask;

/* Local change analysis: seeds = first free cell of each side-run along
 * the last painted ray. A capped flood from a seed that CLOSES (finds a
 * whole component) smaller than the region proves a sealed pocket. */
static u64 st_p_local;

/* Slide and paint; detects split-risk by counting contiguous runs of free
 * cells along both sides of the painted ray (>=2 runs => possible split). */
static int ray_start, ray_dir, ray_len;   /* last committed slide */

static inline int do_slide(int pos, int d)
{
    s32 dd = DELTA[d];
    s32 pp = (d == 0 || d == 2) ? PW : 1;   /* perpendicular delta */
    int runs = 0;
    bool pl = false, pr = false;
    nlseeds = 0;
    ray_start = pos + dd; ray_dir = d; ray_len = 0;
    do {
        pos += dd;
        ray_len++;
        visit_cell(pos);
        bool l = free_cell(pos - pp);
        bool r = free_cell(pos + pp);
        if (l && !pl) {
            runs++;
            if (nlseeds < LSEED_MAX) lseeds[nlseeds++] = pos - pp;
        }
        if (r && !pr) {
            runs++;
            if (nlseeds < LSEED_MAX) lseeds[nlseeds++] = pos + pp;
        }
        pl = l; pr = r;
    } while (free_cell(pos + dd));
    prev_dirty = dirty_conn;
    if (runs >= 2) dirty_conn = true;
    lseeds_valid = (runs >= 1);
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
    ops += (u32)n;
    /* perpendicular neighbors of the end; ray cells are not perpendicular
     * to the end cell, so current free state is accurate */
    *endopts = free_cell(pos - pp) + free_cell(pos + pp);
    return pos;
}

static int cur_phase;        /* 0 probe, 1 capped sweep, 2 exhaust */
static int cur_startno;      /* index of the start being attempted */
static u64 phase_ops[3];

static void ops_report(int solved)
{
    fprintf(stderr,
        "OPSRESULT solved=%d ops=%llu phase=%d start#=%d "
        "p0=%llu p1=%llu p2=%llu\n",
        solved, (unsigned long long)ops, cur_phase, cur_startno,
        (unsigned long long)phase_ops[0],
        (unsigned long long)phase_ops[1],
        (unsigned long long)phase_ops[2]);
}

/* killed from outside: still emit the measurement */
static void on_kill(int sig)
{
    (void)sig;
    ops_report(0);
    _exit(2);
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
    fprintf(stderr, "  lbpar=%llu chain=%llu\n",
        (unsigned long long)st_p_lbpar,
        (unsigned long long)st_p_chain);
    fprintf(stderr, "  lobes: solves=%llu hits=%llu prunes=%llu\n",
        (unsigned long long)st_lobe_solves,
        (unsigned long long)st_lobe_hits,
        (unsigned long long)st_p_lobe);
    fprintf(stderr, "  hist_region:");
    for (int i = 7; i >= 0; i--)
        fprintf(stderr, " %llu", (unsigned long long)hist_region[i]);
    fprintf(stderr, "\n  hist_visit:");
    for (int i = 7; i >= 0; i--)
        fprintf(stderr, " %llu", (unsigned long long)hist_visit[i]);
    fprintf(stderr, "\n  hist_branch:");
    for (int i = 7; i >= 0; i--)
        fprintf(stderr, " %llu", (unsigned long long)hist_branch[i]);
    fprintf(stderr, "\n  hist_rcall:");
    for (int i = 7; i >= 0; i--)
        fprintf(stderr, " %llu", (unsigned long long)hist_rcall[i]);
    fprintf(stderr, "\n  hist_rprune:");
    for (int i = 7; i >= 0; i--)
        fprintf(stderr, " %llu", (unsigned long long)hist_rprune[i]);
    fprintf(stderr, "\n  hist_pconn:");
    for (int i = 7; i >= 0; i--)
        fprintf(stderr, " %llu", (unsigned long long)hist_pconn[i]);
    if (use_lh)
        fprintf(stderr, "  lh: total=%llu distinct=%llu ratio=%.1f\n",
            (unsigned long long)lh_total, (unsigned long long)lh_distinct,
            lh_total / (double)(lh_distinct ? lh_distinct : 1));
    fprintf(stderr, "\n  leafblk: cnt=%llu avg=%llu max=%llu giant=%llu",
        (unsigned long long)st_leaf_cnt,
        (unsigned long long)(st_leaf_cells / (st_leaf_cnt ? st_leaf_cnt : 1)),
        (unsigned long long)st_leaf_max,
        (unsigned long long)st_leaf_giant);
    fprintf(stderr, "\n  frag(share permil,blocks,n):");
    for (int i = 7; i >= 0; i--) {
        u64 n = frag_n[i] ? frag_n[i] : 1;
        fprintf(stderr, " [%llu,%llu,%llu]",
                (unsigned long long)(frag_share[i] / n),
                (unsigned long long)(frag_blocks[i] / n),
                (unsigned long long)frag_n[i]);
    }
    fprintf(stderr, "\n");
}

/*
 * Windowed structural analysis around the last painted ray.
 *
 * The window is the set of free cells within a bounded flood of the ray.
 * Sound claims inside a bounded view:
 *  - a window component with NO cell on the window boundary is a true
 *    component of the whole region; if smaller than the region it is a
 *    sealed, unreachable pocket: dead.
 *  - a biconnected leaf block whose non-cut cells include no boundary
 *    cell is a true pendant lobe: it demands a path endpoint strictly
 *    inside (>= 3 disjoint such lobes: dead) and is micro-refutable.
 *  - if all boundary-reaching parts of the window are one component,
 *    the ray did not split the region: connectivity is re-certified
 *    without reading the rest of the board.
 * Anything touching the boundary yields no claim. Cost is proportional
 * to the change neighborhood, never to the region.
 */
#define WINCAP 256
static u32 *winmark;
static u8  *wbound;
static s32 *winlist;
static u32 wingen;
static bool use_window;     /* falsified as scan replacement at <=104x102;
                               retained as a component for the incremental
                               structure build */
static u64 st_win_calls, st_win_cells, st_p_winseal, st_p_winlobe;

static bool window_analyze(int head)
{
    lseeds_valid = false;
    /* ---- build window: flood from the ray's free neighbors ---- */
    wingen++;
    int wn = 0, qh = 0;
    bool seeds_complete = true;
    int p = ray_start;
    for (int i = 0; i < ray_len; i++, p += DELTA[ray_dir]) {
        for (int d = 0; d < 4; d++) {
            int nb = p + DELTA[d];
            if (!blocked[nb] && winmark[nb] != wingen) {
                if (wn >= WINCAP) { seeds_complete = false; break; }
                winmark[nb] = wingen; wbound[nb] = 0;
                winlist[wn++] = nb;
            }
        }
        if (!seeds_complete) break;
    }
    if (wn == 0) return true;
    while (qh < wn && wn < WINCAP) {
        int q = winlist[qh++];
        for (int d = 0; d < 4; d++) {
            int nb = q + DELTA[d];
            if (blocked[nb] || winmark[nb] == wingen) continue;
            winmark[nb] = wingen; wbound[nb] = 0;
            winlist[wn++] = nb;
            if (wn >= WINCAP) break;
        }
    }
    /* boundary: window cells with a free neighbor outside the window */
    for (int i = 0; i < wn; i++) {
        int q = winlist[i];
        for (int d = 0; d < 4; d++) {
            int nb = q + DELTA[d];
            if (!blocked[nb] && winmark[nb] != wingen) { wbound[q] = 1; break; }
        }
    }
    ops += (u64)wn * 3;
    st_win_calls++; st_win_cells += wn;

    /* ---- per component: sealed test, then Tarjan from a boundary root */
    markgen++;                      /* component sweep marker via mark[] */
    int bcomp = 0;                  /* boundary-touching components seen */
    for (int i = 0; i < wn; i++) {
        int s0 = winlist[i];
        if (mark[s0] == markgen) continue;
        /* flood this component inside the window; find a boundary cell */
        int top = 0, csz = 0, root = -1;
        tstack[top++] = s0; mark[s0] = markgen;
        int chead = -1;
        while (top) {
            int q = tstack[--top];
            csz++;
            if (wbound[q] && root < 0) root = q;
            if (chead < 0 && head >= 0 &&
                (q == head-1 || q == head+1 || q == head-PW || q == head+PW))
                chead = q;
            for (int d = 0; d < 4; d++) {
                int nb = q + DELTA[d];
                if (blocked[nb] || winmark[nb] != wingen || mark[nb] == markgen)
                    continue;
                mark[nb] = markgen;
                tstack[top++] = nb;
            }
        }
        ops += (u64)csz * 2;
        if (root < 0) {
            /* sealed: a true region component */
            if (csz < remaining) { st_p_winseal++; return false; }
            root = s0;     /* window holds the whole region: analyze it,
                              root pops simply make no claims */
        } else {
            bcomp++;
        }
        (void)chead;

        /* Tarjan rooted at the boundary cell: root blocks touch the
         * boundary by construction and are never classified, so no
         * deferred root handling is needed. */
        u32 base = dctr + 1;
        u32 counter = dctr;
        dctr += (u32)csz + 1;
        if (dctr > 0xF0000000u) {
            memset(disc, 0, (size_t)NCELLS * sizeof(u32));
            dctr = 0; base = 1; counter = 0; dctr = (u32)csz + 1;
        }
        int esp = 0;
        int lobes = 0;
        tstack[0] = root; tdirs[0] = 0; tparent[0] = -1;
        disc[root] = low[root] = ++counter;
        sepflag[root] = 0;
        top = 1;
        while (top > 0) {
            int u = tstack[top-1];
            u8 di = tdirs[top-1];
            if (di < 4) {
                tdirs[top-1]++;
                int v = u + DELTA[di];
                if (blocked[v] || winmark[v] != wingen || v == tparent[top-1])
                    continue;
                if (disc[v] >= base) {
                    if (disc[v] < disc[u]) {
                        estack[esp] = v; eother[esp] = u; esp++;
                        if (disc[v] < low[u]) low[u] = disc[v];
                    }
                    continue;
                }
                disc[v] = low[v] = ++counter;
                sepflag[v] = 0;
                estack[esp] = v; eother[esp] = u; esp++;
                tstack[top] = v; tdirs[top] = 0; tparent[top] = u;
                top++;
            } else {
                top--;
                int pp = tparent[top];
                if (pp < 0) break;
                if (low[u] < low[pp]) low[pp] = low[u];
                if (low[u] >= disc[pp]) {
                    bcgen++;
                    if (pp == root) {
                        /* root block: boundary-touching, no claim */
                        for (;;) {
                            int a = estack[--esp], b = eother[esp];
                            if (a == u && b == pp) break;
                        }
                    } else {
                        sepflag[pp] = 1;
                        int cuts = 0, bnd = 0, bv = 0;
                        for (;;) {
                            int a = estack[--esp], b = eother[esp];
                            if (bcmark[a] != bcgen) {
                                bcmark[a] = bcgen;
                                if (bv < LOBE_MAX) lb_cells[bv] = a;
                                bv++;
                                if (sepflag[a]) cuts++;
                                else if (wbound[a]) bnd++;
                            }
                            if (bcmark[b] != bcgen) {
                                bcmark[b] = bcgen;
                                if (bv < LOBE_MAX) lb_cells[bv] = b;
                                bv++;
                                if (sepflag[b]) cuts++;
                                else if (wbound[b]) bnd++;
                            }
                            if (a == u && b == pp) break;
                        }
                        if (cuts <= 1 && bnd == 0 && wbound[pp] == 0 &&
                            bv < remaining) {
                            /* true pendant lobe, fully interior */
                            if (++lobes >= 3) { st_p_winlobe++; return false; }
                            if (bv <= LOBE_MAX) {
                                lb_n = bv;
                                if (!lobe_check(pp, head)) {
                                    st_p_winlobe++;
                                    return false;
                                }
                            }
                        }
                    }
                }
            }
        }
        ops += (u64)csz * 5;
    }
    /* Split resolution: every post-slide component contains a free
     * neighbor of the ray (all inside the window), so if exactly one
     * boundary-reaching component exists and no sealed one survived,
     * the region is still connected - PROVIDED it was connected before
     * this slide (prev_dirty false). */
    if (bcomp <= 1 && !prev_dirty && seeds_complete) dirty_conn = false;
    return true;
}


/*
 * Rind scan: the full scan's claims, at cost proportional to the carved
 * neighborhood. New cut/block structure can only exist in the rind (the
 * virgin remainder keeps the initial board's structure, verified once at
 * startup); so Tarjan runs over rind cells only, rooted at cells adjacent
 * to the virgin interior (boundary, no claims), and:
 *   - a rind component with no virgin attachment is a true component:
 *     dead if smaller than the region
 *   - a leaf block with no boundary contact is a true pendant lobe:
 *     micro-refuted via the content cache; >= 3 disjoint: dead
 * Global connectivity, when in doubt (dirty), is a cheap flood (2 ops
 * per cell) instead of a Tarjan (6 ops per cell).
 */
static u64 st_rind_calls, st_rind_cells, st_p_rindseal, st_p_rindlobe;

static bool rind_ok(int head)
{
    st_rind_calls++;
    markgen++;
    u32 cgen = markgen;          /* component sweep marker in mark[] */
    int scanned = 0;
    for (int li = 0; li < nrind; li++) {
        int s0 = rindlist[li];
        if (s0 < 0 || s0 >= NCELLS || blocked[s0]) continue;
        if (mark[s0] == cgen) continue;
        /* flood this rind component; find a boundary (virgin-adjacent) root */
        int top = 0, csz = 0, root = -1;
        tstack[top++] = s0; mark[s0] = cgen;
        while (top) {
            int q = tstack[--top];
            csz++;
            for (int d = 0; d < 4; d++) {
                int nb = q + DELTA[d];
                if (blocked[nb]) continue;
                if (rindcnt[nb] == 0) { if (root < 0) root = q; continue; }
                if (mark[nb] != cgen) { mark[nb] = cgen; tstack[top++] = nb; }
            }
        }
        scanned += csz;
        if (root < 0) {
            if (csz < remaining) { st_p_rindseal++; ops += (u64)scanned*8; return false; }
            root = s0;           /* rind covers the whole region */
        }

        /* Tarjan rooted at boundary: root blocks make no claims */
        u32 base = dctr + 1;
        u32 counter = dctr;
        if (dctr > 0xF0000000u) {
            memset(disc, 0, (size_t)NCELLS * sizeof(u32));
            dctr = 0; base = 1; counter = 0;
        }
        dctr += (u32)csz + 1;
        int esp = 0, lobes = 0;
        tstack[0] = root; tdirs[0] = 0; tparent[0] = -1;
        disc[root] = low[root] = ++counter;
        sepflag[root] = 0;
        top = 1;
        while (top > 0) {
            int u = tstack[top-1];
            u8 di = tdirs[top-1];
            if (di < 4) {
                tdirs[top-1]++;
                int v = u + DELTA[di];
                if (blocked[v] || rindcnt[v] == 0 || v == tparent[top-1])
                    continue;
                if (disc[v] >= base) {
                    if (disc[v] < disc[u]) {
                        estack[esp] = v; eother[esp] = u; esp++;
                        if (disc[v] < low[u]) low[u] = disc[v];
                    }
                    continue;
                }
                disc[v] = low[v] = ++counter;
                sepflag[v] = 0;
                estack[esp] = v; eother[esp] = u; esp++;
                tstack[top] = v; tdirs[top] = 0; tparent[top] = u;
                top++;
            } else {
                top--;
                int pp = tparent[top];
                if (pp < 0) break;
                if (low[u] < low[pp]) low[pp] = low[u];
                if (low[u] >= disc[pp]) {
                    bcgen++;
                    if (pp == root) {
                        for (;;) {
                            int a = estack[--esp], b = eother[esp];
                            if (a == u && b == pp) break;
                        }
                    } else {
                        sepflag[pp] = 1;
                        int cuts = 0, bnd = 0, bv = 0;
                        for (;;) {
                            int a = estack[--esp], b = eother[esp];
                            if (bcmark[a] != bcgen) {
                                bcmark[a] = bcgen;
                                if (bv < LOBE_MAX) lb_cells[bv] = a;
                                bv++;
                                if (sepflag[a]) cuts++;
                                else {
                                    for (int d = 0; d < 4; d++) {
                                        int nb = a + DELTA[d];
                                        if (!blocked[nb] && rindcnt[nb] == 0)
                                            { bnd++; break; }
                                    }
                                }
                            }
                            if (bcmark[b] != bcgen) {
                                bcmark[b] = bcgen;
                                if (bv < LOBE_MAX) lb_cells[bv] = b;
                                bv++;
                                if (sepflag[b]) cuts++;
                                else {
                                    for (int d = 0; d < 4; d++) {
                                        int nb = b + DELTA[d];
                                        if (!blocked[nb] && rindcnt[nb] == 0)
                                            { bnd++; break; }
                                    }
                                }
                            }
                            if (a == u && b == pp) break;
                        }
                        if (cuts <= 1 && bnd == 0 && bv < remaining) {
                            if (++lobes >= 3) {
                                st_p_rindlobe++;
                                ops += (u64)scanned * 8;
                                return false;
                            }
                            if (bv <= LOBE_MAX) {
                                lb_n = bv;
                                if (!lobe_check(pp, head)) {
                                    st_p_rindlobe++;
                                    ops += (u64)scanned * 8;
                                    return false;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    st_rind_cells += scanned;
    ops += (u64)scanned * 8;

    /* connectivity, only when a slide left it in doubt: cheap flood */
    if (dirty_conn) {
        int seed = -1;
        if      (!blocked[head-1])  seed = head-1;
        else if (!blocked[head+1])  seed = head+1;
        else if (!blocked[head-PW]) seed = head-PW;
        else if (!blocked[head+PW]) seed = head+PW;
        if (seed < 0) return false;
        markgen++;
        int top = 0, cnt = 1;
        tstack[top++] = seed; mark[seed] = markgen;
        while (top) {
            int q = tstack[--top];
            for (int d = 0; d < 4; d++) {
                int nb = q + DELTA[d];
                if (blocked[nb] || mark[nb] == markgen) continue;
                mark[nb] = markgen; tstack[top++] = nb; cnt++;
            }
        }
        ops += (u64)cnt * 2;
        if (cnt != remaining) { st_p_conn++; return false; }
        dirty_conn = false;
    }
    return true;
}

static bool dfs(int pos)
{
    for (;;) {
        if (remaining == 0) return true;
        if (aborted) return false;
        if (!struct_ok(pos)) { st_p_struct++; return false; }
        if (!parity_ok(pos)) { st_p_parity++; return false; }
        if (use_window && lseeds_valid && !window_analyze(pos)) return false;

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
        if (use_lh) lh_probe(pos);
        ops += 12;
        hist_branch[(remaining * 7) / (total_empty + 1)]++;
        if (ops_limit && ops > ops_limit) {
            ops_out = true; aborted = true; return false;
        }
        if (remaining < best_remaining) {
            best_remaining = remaining;
            work_at_best = work;
        }
        if (work > node_budget) { aborted = true; return false; }

        /* transposition: this coverage + head already proven dead? */
        u64 ttkey = zhash ^ zheadkey[pos];
        u32 ttidx = (u32)((ttkey * 0x9e3779b97f4a7c15ULL) >> (64 - TT_BITS));
        if (tt[ttidx] == ttkey) { st_p_tt++; return false; }

        /* Region analysis cadence scales with region size (cost O(r),
         * frequency 1/r: constant amortized cost per branch). Falsified
         * alternatives, by ops measurement: sparser scans (tree x3),
         * window-only with event-driven scans (fewer refutations/op even
         * at 104x102). The scan's region-global block knowledge is what
         * keeps refutations short; only an incrementally MAINTAINED
         * structure can cut this cost without losing it. */
        bool fresh_region = false;
        u32 cadence = (u32)(remaining >> 9);
        if ((dirty_conn || always_check) &&
            (cadence == 0 || (++checktick % (cadence + 1)) == 0)) {
            int hb = (remaining * 7) / (total_empty + 1);
            hist_rcall[hb]++;
            /* rind scan while the carved area is a small part of the
             * region; full scan (more claims, same cost) once the rind
             * approaches the region size */
            if (use_rind && (u64)nrind * 2 < (u64)remaining * 3) {
                if (!rind_ok(pos)) { hist_rprune[hb]++; return false; }
            } else {
                if (!region_ok(pos)) { hist_rprune[hb]++; return false; }
                dirty_conn = false;
                fresh_region = true;
            }
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
    lseeds_valid = false;
    ops += 16;
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
    lobemark = calloc(NCELLS, sizeof(u32));
    lobeslot = calloc(NCELLS, sizeof(u32));
    winmark = calloc(NCELLS, sizeof(u32));
    wbound  = calloc(NCELLS, 1);
    blkid    = calloc(NCELLS, sizeof(u32));
    blkgenof = calloc(NCELLS, sizeof(u32));
    cutflag  = calloc(NCELLS, 1);
    blk_size = malloc(((size_t)NCELLS / 2 + 2) * sizeof(int));
    blk_leaf = malloc((size_t)NCELLS / 2 + 2);
    winlist = malloc(((size_t)WINCAP + 8) * sizeof(s32));
    {
        /* rind arrays carry 2-row slack: distance-2 offsets from border
         * cells land outside the padded board but inside the slack */
        size_t slack = (size_t)2 * PW;
        u16 *rc = calloc(NCELLS + 2 * slack, sizeof(u16));
        s32 *ri = malloc((NCELLS + 2 * slack) * sizeof(s32));
        rindcnt = rc + slack;
        rindidx = ri + slack;
        rindlist = malloc((size_t)(NCELLS + 2 * slack) * sizeof(s32));
        nrind = 0;
        int k2 = 0;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx != 0 || dy != 0)
                    if (abs(dx) + abs(dy) <= 2)
                        rind_off[k2++] = dy * PW + dx;
    }
    lobe_feas   = calloc((size_t)1 << LOBE_TT_BITS, sizeof(u64));
    lobe_infeas = calloc((size_t)1 << LOBE_TT_BITS, sizeof(u64));
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
    ops_limit = getenv("COIL_OPS_LIMIT") ? strtoull(getenv("COIL_OPS_LIMIT"), NULL, 10) : 0;
    use_window = getenv("COIL_WINDOW") != NULL;
    use_rind = getenv("COIL_RIND") != NULL;   /* falsified: distance-2 rind
                                                 misses pocket-scale structure */
    use_chain = getenv("COIL_CHAIN") != NULL;
    use_struct = getenv("COIL_STRUCT") != NULL;
    use_lh = getenv("COIL_LH") != NULL;
    if (use_lh) lh_set = calloc((size_t)1 << LH_BITS, sizeof(u64));
    paranoid = getenv("COIL_PARANOID") != NULL;
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
            signal(SIGTERM, on_kill);
            signal(SIGINT, on_kill);
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
                    cur_phase = 0; cur_startno = k;
                    u64 ops0 = ops;
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
                        ops_report(1);
                        print_stats();
                        _exit(0);
                    }
                    if (anyalive) {
                        act[alive] = k; prog[alive] = bestp; alive++;
                    }
                    phase_ops[0] += ops - ops0;
                    if (ops_out) break;
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
                        cur_phase = pass; cur_startno = k;
                        u64 ops0 = ops;
                        rng = seed_salt ^ ((u64)(k+1) << 16) ^ ((u64)pass << 56);
                        rng ^= rng >> 33; rng *= 0xff51afd7ed558ccdULL; rng ^= rng >> 33;
                        bool ok = solve_from(order[k]);
                        if (dbg)
                            fprintf(stderr, "w%d pass%d start#%d/%d pos=(%d,%d) nodes=%llu best=%d aops=%llu %s\n",
                                    w, pass, k, ns, order[k] % PW - 1, order[k] / PW - 1,
                                    (unsigned long long)nodes, best_remaining,
                                    (unsigned long long)(ops - ops0),
                                    ok ? "SOLVED" : (aborted ? "alive" : "dead"));
                        if (ok) {
                            int sx = order[k] % PW - 1, sy = order[k] / PW - 1;
                            path[pathlen] = 0;
                            fprintf(out, "x=%d&y=%d&path=%s\n", sx, sy, path);
                            fflush(out);
                            ops_report(1);
                            print_stats();
                            _exit(0);
                        }
                        if (aborted) {
                            act[kept] = k; prog[kept] = best_remaining; kept++;
                        }
                        phase_ops[pass] += ops - ops0;
                        if (ops_out) break;
                    }
                    alive = kept;
                    if (ops_out) break;
                }
                if (nworkers == 1) printf("No solution found\n");
                ops_report(0);
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

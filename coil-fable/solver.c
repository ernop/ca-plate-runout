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
static int dirperm[4] = {0, 1, 2, 3};   /* per-worker direction priority */
static int order_mode;                  /* candidate ordering variant */
static bool randtie;                    /* randomized tie-breaking */
static u64 rng = 0x9e3779b97f4a7c15ULL;
static u64 seed_salt = 0x9e3779b97f4a7c15ULL;
static bool use_forced;     /* forced-edge deficit pruning (off: net loss) */

static u32 *mark;
static u32 markgen;
static u64 st_region_calls, st_region_cells, st_visits, st_branches;

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

static int last_leafblocks;  /* result of the most recent region_ok */
static u32 last_region_gen;

static u64 work;             /* time-proxy budget counter */

static bool region_ok(int pos)
{
    if (remaining == 0) return true;
    int seed;
    if      (free_cell(pos-1))  seed = pos-1;
    else if (free_cell(pos+1))  seed = pos+1;
    else if (free_cell(pos-PW)) seed = pos-PW;
    else if (free_cell(pos+PW)) seed = pos+PW;
    else return false;

    if (dctr > 0xF0000000u) {           /* rare: reset before wraparound */
        memset(disc, 0, (size_t)NCELLS * sizeof(u32));
        dctr = 0;
    }
    markgen++;
    last_region_gen = markgen;
    last_leafblocks = 0;
    u32 base = dctr + 1;     /* disc >= base <=> discovered this call */
    u32 counter = dctr;
    dctr += (u32)remaining + 1;  /* reserve range (early returns safe) */
    int cnt = 1;
    int esp = 0;
    int leafblocks = 0;
    int root_children = 0;
    int rb_start[5], rb_cuts[5], nrb = 0;   /* root blocks (<=4) */
    int rbn = 0;
    int deficit = 0;    /* forced path-degree overload, max 2 fixable */

    tstack[0] = seed; tdirs[0] = 0; tparent[0] = -1;
    disc[seed] = low[seed] = ++counter;
    sepflag[seed] = 0;
    int top = 1;

    /*
     * Forced-edge accounting: a free cell with exactly 2 free neighbors
     * must use both incident edges unless it is one of the path's 2
     * endpoints. Each endpoint placed at such a cell relaxes one forced
     * edge. A cell x can carry at most 2 path edges, so any forced count
     * beyond 2 must be paid for by an endpoint; total budget is 2.
     */
#define FORCE_FROM(v)                                                     \
    do {                                                                  \
        if (use_forced && deg[v] == 2) {                                  \
            for (int _i = 0; _i < 4; _i++) {                              \
                int _x = (v) + DELTA[_i];                                 \
                if (!free_cell(_x)) continue;                             \
                if (fmark[_x] != markgen) { fmark[_x] = markgen; fdeg[_x] = 0; } \
                if (++fdeg[_x] > 2) {                                     \
                    if (++deficit > 2) return false;                      \
                }                                                         \
            }                                                             \
        }                                                                 \
    } while (0)

    FORCE_FROM(seed);

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
            cnt++;
            FORCE_FROM(v);
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
                        if (++leafblocks >= 3) return false;
                        for (int e = esp; e < mstart; e++) {
                            if (!sepflag[estack[e]])
                                leafmark[estack[e]] = markgen;
                            if (!sepflag[eother[e]])
                                leafmark[eother[e]] = markgen;
                        }
                    }
                }
            }
        }
    }

    work += (u32)cnt >> 3;
    st_region_calls++; st_region_cells += cnt;
    if (cnt != remaining) return false;

    /* classify deferred root blocks */
    bool root_cut = root_children >= 2;
    for (int i = 0; i < nrb; i++) {
        int cuts = rb_cuts[i] + (root_cut ? 1 : 0);
        if (cuts <= 1) {
            if (++leafblocks >= 3) return false;
            int end = (i + 1 < nrb) ? rb_start[i+1] : rbn;
            for (int j = rb_start[i]; j < end; j++)
                if (!sepflag[rbuf[j]]) leafmark[rbuf[j]] = markgen;
            if (!root_cut) leafmark[seed] = markgen;
        }
    }

    last_leafblocks = leafblocks;
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

static bool dfs(int pos)
{
    for (;;) {
        if (remaining == 0) return true;
        if (aborted) return false;
        if (!struct_ok(pos)) return false;
        if (!parity_ok(pos)) return false;

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

        nodes++; work++; st_branches++;
        if (work > node_budget) { aborted = true; return false; }

        bool fresh_region = false;
        if ((dirty_conn || always_check) && (++checktick & checkmask) == 0) {
            if (!region_ok(pos)) return false;
            dirty_conn = false;
            fresh_region = true;
        }

        /* With exactly 2 leaves the path must start at a leaf: restrict
         * to directions whose first cell is a leaf. */
        if (nleaf == 2) {
            int fd[4], fn = 0;
            for (int k = 0; k < nd; k++)
                if (deg[pos + DELTA[dirs[k]]] == 1) fd[fn++] = dirs[k];
            if (fn == 0) return false;
            memcpy(dirs, fd, fn * sizeof(int)); nd = fn;
        }

        /* With exactly 2 leaf blocks, the next entered cell is one of the
         * two path endpoints, which must lie strictly inside a leaf block. */
        if (fresh_region && last_leafblocks == 2) {
            int fd[4], fn = 0;
            for (int k = 0; k < nd; k++)
                if (leafmark[pos + DELTA[dirs[k]]] == last_region_gen)
                    fd[fn++] = dirs[k];
            if (fn == 0) return false;
            memcpy(dirs, fd, fn * sizeof(int)); nd = fn;
        }

        /* Lookahead each candidate slide: skip guaranteed-dead ones,
         * take instant wins, order the rest by end constrainedness. */
        int cand[4], key[4], nc = 0;
        for (int k = 0; k < nd; k++) {
            int len, eo;
            sim_slide(pos, dirs[k], &len, &eo);
            if (len == remaining) {           /* instant win */
                path[pathlen++] = DIRCH[dirs[k]];
                do_slide(pos, dirs[k]);
                return true;
            }
            if (eo == 0) continue;            /* dead end, not a win: skip */
            int kk;
            switch (order_mode) {
            case 1:  kk = (eo == 1 ? 0 : 1 << 20) - len; break; /* tie: long */
            case 2:  kk = (eo == 1 ? 0 : 1 << 20) + len; break; /* tie: short */
            default: kk = (eo == 1) ? 0 : 8;  break;
            }
            if (randtie) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                kk = kk * 8 + (int)(rng & 7);   /* shuffle within class */
            }
            int a = nc++;
            while (a > 0 && key[a-1] > kk) {
                cand[a] = cand[a-1]; key[a] = key[a-1]; a--;
            }
            cand[a] = dirs[k]; key[a] = kk;
        }

        for (int k = 0; k < nc; k++) {
            int saveL = vloglen, saveP = pathlen;
            bool saveD = dirty_conn;
            path[pathlen++] = DIRCH[cand[k]];
            int np = do_slide(pos, cand[k]);
            if (dfs(np)) return true;
            rewind_to(saveL, saveP);
            dirty_conn = saveD;
        }
        return false;
    }
}

static bool solve_from(int start)
{
    int saveL = vloglen, saveP = pathlen;
    dirty_conn = true;          /* removing the start cell may split */
    visit_cell(start);
    nodes = 0; work = 0; aborted = false;
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
                    if (deg[p] == pass) order[ns++] = p;
                }
            }
        }
    }

    if (ns == 0) { printf("No solution found\n"); return 0; }

    bool dbg = getenv("COIL_DEBUG") != NULL;
    always_check = getenv("COIL_LAZY_CHECK") == NULL;  /* default: always */
    order_mode = getenv("COIL_ORDER") ? atoi(getenv("COIL_ORDER")) : 0;
    randtie = getenv("COIL_NORANDTIE") == NULL;        /* default: on */
    checkmask = getenv("COIL_CHECK_EVERY") ? (u32)atoi(getenv("COIL_CHECK_EVERY")) - 1 : 7;
    if (getenv("COIL_SEED"))
        seed_salt ^= (u64)strtoull(getenv("COIL_SEED"), NULL, 10) * 0xc2b2ae3d27d4eb4fULL;
    else
        seed_salt ^= ((u64)time(NULL) * 0xc2b2ae3d27d4eb4fULL) ^ ((u64)getpid() << 32);
    use_forced = getenv("COIL_FORCED") != NULL;
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
            }
            FILE *out = fdopen(pipes[w][1], "w");
            for (int i = 0; i < 4; i++) dirperm[i] = (i + w) & 3;
            if (!getenv("COIL_CHECK_EVERY")) {
                /* diversify region-check cadence across workers */
                static const u32 kdiv[4] = {7, 3, 11, 15};
                checkmask = kdiv[w & 3];
            }

            u8 *dead = calloc(ns, 1);
            /* work units scale with board size (region scans dominate).
             * Winning starts typically solve within a few thousand branch
             * nodes; scan many starts cheaply before going deep. */
            u64 scale = 1 + (u64)total_empty / 8;
            u64 budgets[] = {1000, 8000, 64000, 512000, 8000000, 128000000, (u64)1<<44};
            int ntiers = 7;
            for (int bi = 0; bi < ntiers; bi++) {
                node_budget = budgets[bi] * scale;
                int alive = 0;
                for (int k = w; k < ns; k += nworkers) {
                    if (dead[k]) continue;
                    alive++;
                    rng = seed_salt ^ ((u64)(w+1) << 40)
                        ^ ((u64)(k+1) << 16) ^ (u64)(bi+1);
                    rng ^= rng >> 33; rng *= 0xff51afd7ed558ccdULL; rng ^= rng >> 33;
                    bool ok = solve_from(order[k]);
                    if (!ok && !aborted) dead[k] = 1;
                    if (dbg)
                        fprintf(stderr, "w%d tier%d start#%d/%d pos=(%d,%d) nodes=%llu %s\n",
                                w, bi, k, ns, order[k] % PW - 1, order[k] / PW - 1,
                                (unsigned long long)nodes,
                                ok ? "SOLVED" : (aborted ? "abort" : "dead"));
                    if (ok) {
                        int sx = order[k] % PW - 1, sy = order[k] / PW - 1;
                        path[pathlen] = 0;
                        fprintf(out, "x=%d&y=%d&path=%s\n", sx, sy, path);
                        fflush(out);
                        if (getenv("COIL_STATS"))
                            fprintf(stderr,
                                "w%d stats: region_calls=%llu region_cells=%llu visits=%llu branches=%llu\n",
                                w,
                                (unsigned long long)st_region_calls,
                                (unsigned long long)st_region_cells,
                                (unsigned long long)st_visits,
                                (unsigned long long)st_branches);
                        _exit(0);
                    }
                }
                if (alive == 0) break;
            }
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

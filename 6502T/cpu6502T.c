/**
 * cpu6502T.c - MOS 6502 transistor-level simulator
 *
 * Drop-in replacement for cpu6502.c. Same API, different engine.
 * Simulates the real 6502 at the transistor/netlist level using the
 * visual6502 netlist data with switch-level simulation.
 *
 * Algorithm ported from perfect6502 (mist64/visual6502):
 *   - Iterative BFS addNodeToGroup for group resolution (no recursion)
 *   - VSS/VCC/real split in c1c2 adjacency (VSS early-exit)
 *   - Bitmap-based group membership tracking
 *   - Ping-pong buffer stabilization (listin/listout)
 *   - Deduplicated dependency lists (full + left)
 *   - Three node states: pullup, pulldown, value
 *   - Memory bus: read on phi2->phi1 transition (old clk was low)
 *
 * Performance: all hot-path functions are INLINE to eliminate
 * call overhead. Pointer-based State struct keeps hot bitmaps in L1.
 * Local pointer caching in recalcNode/addNodeToGroup reduces this->
 * indirection.
 *
 * Based on netlist data from visual6502.org (James/Silverman/Silverman)
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cpu6502T.h"
#include "netlist_6502.h"

#ifdef THREADSAFE
    #ifdef _MSC_VER
        #define TS __declspec(thread)
    #else
        #define TS __thread
    #endif
#else
    #define TS
#endif

/* ── Constants ──────────────────────────────────────────────────────── */

#define N_NODES  1725

/* Bitmap operations (64-bit words) */
#define BM_SHIFT 6
#define BM_MASK  63
#define BM_ONE   1ULL
#define BM_WORDS(n) (((n) + 63) / 64)

typedef uint64_t bm_t;

/* Netlist node indices */
#define ND_vcc   657
#define ND_vss   558
#define ND_clk0  1171
#define ND_irq   103
#define ND_nmi   1297
#define ND_rdy   89
#define ND_res   159
#define ND_rw    1156
#define ND_sync  539
#define ND_so    1672

/* Register bit node indices (LSB first: bit 0 = nodes[0], bit 7 = nodes[7])
   read_nodes() reads from [7]→[0] with left shift, placing nodes[7] in MSB */
static const int ND_a[]  = { 737,1234,978,162,727,858,1136,1653 };
static const int ND_x[]  = { 1216,98,1,1648,85,589,448,777 };
static const int ND_y[]  = { 64,1148,573,305,989,615,115,843 };
static const int ND_p[]  = { 687,1444,1421,439,1119,0,77,1370 };
static const int ND_s[]  = { 1403,183,81,1532,1702,1098,1212,1435 };
static const int ND_pcl[]= { 1139,1022,655,1359,900,622,377,1611 };
static const int ND_pch[]= { 1670,292,502,584,948,49,1551,205 };
static const int ND_ab[] = { 268,451,1340,211,435,736,887,1493,230,148,1443,399,1237,349,672,195 };
static const int ND_db[] = { 1005,82,945,650,1393,175,1591,1349 };

/* ── Inline bitmap helpers ──────────────────────────────────────────── */

INLINE void bm_set(bm_t *bm, int i) { bm[i>>BM_SHIFT] |= BM_ONE << (i & BM_MASK); }
INLINE void bm_clr(bm_t *bm, int i) { bm[i>>BM_SHIFT] &= ~(BM_ONE << (i & BM_MASK)); }
INLINE int  bm_get(const bm_t *bm, int i) { return (bm[i>>BM_SHIFT] >> (i & BM_MASK)) & 1; }
INLINE void bm_zero(bm_t *bm, int n) { memset(bm, 0, BM_WORDS(n) * sizeof(bm_t)); }

INLINE BOOL nd_get(const bm_t *bm, int n) { return bm_get(bm, n); }
INLINE void nd_set(bm_t *bm, int n, BOOL v) { if (v) bm_set(bm, n); else bm_clr(bm, n); }

/* ── Transistor adjacency (compact uint16_t pairs) ──────────────────── */

typedef struct { uint16_t gate; uint16_t other; } c1c2_t;

/* ── Internal state (heap-allocated, pointer-based for cache locality) ── */

typedef struct {
    /* Node state bitmaps — hot data, must be L1-resident */
    bm_t *pullup;
    bm_t *pulldown;
    bm_t *value;

    /* Adjacency: per-node list of (gate, other_node) pairs, VSS/VCC/real sorted */
    c1c2_t *c1c2s;
    uint16_t *c1c2_offset;
    uint16_t *vss_count;
    uint16_t *vcc_count;

    /* Dependency lists: nodes affected when this node changes */
    uint16_t *dep_offset;      /* full deps (gate low) */
    uint16_t *dep_left_offset; /* left deps (gate high) */
    uint16_t *dep_block;

    /* Stabilization: ping-pong buffers */
    uint16_t *list1, *list2;
    uint16_t *listin, *listout;
    int listin_len, listout_len;
    bm_t *listout_bm;

    /* Group resolution */
    uint16_t *group;
    int group_len;
    bm_t *group_bm;

    uint8_t *ram;    /* memory buffer (points to cpu->ram or g_ram) */
    BOOL initialized;
} State;

static TS State S;
static TS uint8_t g_ram[65536];

/* ── Bus cycle log ──────────────────────────────────────────────────── */

static TS CPU6502T_Cycle *g_log_buf;
static TS int g_log_cap;
static TS int g_log_len;
static TS BOOL g_log_active;

void cpu6502T_log_start(_CPUP) {
    (void)_CPUPA;
    if (!g_log_buf) {
        g_log_cap = 256;
        g_log_buf = (CPU6502T_Cycle*)malloc(g_log_cap * sizeof(CPU6502T_Cycle));
    }
    g_log_len = 0;
    g_log_active = TRUE;
}

void cpu6502T_log_stop(_CPUP) {
    (void)_CPUPA;
    g_log_active = FALSE;
}

int cpu6502T_log_count(_CCPUP) {
    (void)_CPUPA;
    return g_log_len;
}

const CPU6502T_Cycle *cpu6502T_log_entries(_CCPUP) {
    (void)_CPUPA;
    return g_log_buf;
}

/* ── Build pre-computed adjacency and dependency data ───────────────── */

#define N_RAW_TRANS (sizeof(netlist_6502_transdefs)/sizeof(netlist_6502_transdefs[0]))

static void add_dep(uint16_t *block, int *counts, int offset, int dep_node) {
    int g, cnt = counts[0];
    uint16_t *data = block + offset;
    for (g = 0; g < cnt; g++)
        if (data[g] == dep_node) return;
    data[cnt] = (uint16_t)dep_node;
    counts[0]++;
}

static void build_state(void) {
    int i, j, n, t, g, cnt, found, gate, c1, c2, node, off, left;
    int t_used, dep_total;
    int bw = BM_WORDS(N_NODES);
    int *t_gate, *t_c1, *t_c2;
    int *c1c2_count, *vss_fill, *vcc_fill;
    int *dep_count, *dep_left_count;
    uint16_t *data;

    /* Allocate hot bitmaps */
    S.pullup      = (bm_t *)calloc(bw, sizeof(bm_t));
    S.pulldown    = (bm_t *)calloc(bw, sizeof(bm_t));
    S.value       = (bm_t *)calloc(bw, sizeof(bm_t));
    S.listout_bm  = (bm_t *)calloc(bw, sizeof(bm_t));
    S.group_bm    = (bm_t *)calloc(bw, sizeof(bm_t));
    S.group       = (uint16_t *)calloc(N_NODES, sizeof(uint16_t));
    S.list1       = (uint16_t *)calloc(N_NODES, sizeof(uint16_t));
    S.list2       = (uint16_t *)calloc(N_NODES, sizeof(uint16_t));
    S.listin      = S.list1;
    S.listout     = S.list2;
    S.listin_len  = 0;
    S.listout_len = 0;
    S.group_len   = 0;

    /* Deduplicate transistors (O(n^2), runs once at init) */
    t_gate = (int *)malloc(N_RAW_TRANS * sizeof(int));
    t_c1   = (int *)malloc(N_RAW_TRANS * sizeof(int));
    t_c2   = (int *)malloc(N_RAW_TRANS * sizeof(int));
    t_used = 0;

    for (i = 0; i < (int)N_RAW_TRANS; i++) {
        gate = netlist_6502_transdefs[i].gate;
        c1   = netlist_6502_transdefs[i].c1;
        c2   = netlist_6502_transdefs[i].c2;
        found = 0;
        for (j = 0; j < t_used; j++) {
            if (t_gate[j] == gate &&
                ((t_c1[j] == c1 && t_c2[j] == c2) ||
                 (t_c1[j] == c2 && t_c2[j] == c1))) {
                found = 1;
                break;
            }
        }
        if (!found) {
            t_gate[t_used] = gate;
            t_c1[t_used]   = c1;
            t_c2[t_used]   = c2;
            t_used++;
        }
    }

    /* Build adjacency (c1c2) lists — sorted VSS/VCC/real per node */
    c1c2_count = (int *)calloc(N_NODES, sizeof(int));
    vss_fill   = (int *)calloc(N_NODES, sizeof(int));
    vcc_fill   = (int *)calloc(N_NODES, sizeof(int));
    for (i = 0; i < t_used; i++) {
        c1c2_count[t_c1[i]]++;
        c1c2_count[t_c2[i]]++;
    }
    S.c1c2_offset = (uint16_t *)malloc((N_NODES + 1) * sizeof(uint16_t));
    S.c1c2_offset[0] = 0;
    for (i = 0; i < N_NODES; i++)
        S.c1c2_offset[i + 1] = S.c1c2_offset[i] + (uint16_t)c1c2_count[i];
    S.c1c2s = (c1c2_t *)malloc(S.c1c2_offset[N_NODES] * sizeof(c1c2_t));

    /* Count VSS and VCC entries per node for bucket offsets */
    S.vss_count = (uint16_t *)calloc(N_NODES, sizeof(uint16_t));
    S.vcc_count = (uint16_t *)calloc(N_NODES, sizeof(uint16_t));
    for (i = 0; i < t_used; i++) {
        if (t_c2[i] == ND_vss) S.vss_count[t_c1[i]]++;
        else if (t_c2[i] == ND_vcc) S.vcc_count[t_c1[i]]++;
        if (t_c1[i] == ND_vss) S.vss_count[t_c2[i]]++;
        else if (t_c1[i] == ND_vcc) S.vcc_count[t_c2[i]]++;
    }

    /* Fill entries in VSS/VCC/real order using bucket pointers */
    for (i = 0; i < N_NODES; i++) {
        vss_fill[i] = S.c1c2_offset[i];
        vcc_fill[i] = vss_fill[i] + S.vss_count[i];
    }
    memset(c1c2_count, 0, N_NODES * sizeof(int));
    for (i = 0; i < N_NODES; i++)
        c1c2_count[i] = vcc_fill[i] + S.vcc_count[i];

    for (i = 0; i < t_used; i++) {
        /* c1 side */
        node = t_c1[i];
        if (t_c2[i] == ND_vss) off = vss_fill[node]++;
        else if (t_c2[i] == ND_vcc) off = vcc_fill[node]++;
        else off = c1c2_count[node]++;
        S.c1c2s[off].gate  = (uint16_t)t_gate[i];
        S.c1c2s[off].other = (uint16_t)t_c2[i];
        /* c2 side */
        node = t_c2[i];
        if (t_c1[i] == ND_vss) off = vss_fill[node]++;
        else if (t_c1[i] == ND_vcc) off = vcc_fill[node]++;
        else off = c1c2_count[node]++;
        S.c1c2s[off].gate  = (uint16_t)t_gate[i];
        S.c1c2s[off].other = (uint16_t)t_c1[i];
    }
    free(c1c2_count);
    free(vss_fill);
    free(vcc_fill);

    /* Build dependency lists (deduplicated, matches perfect6502) */
    dep_count      = (int *)calloc(N_NODES, sizeof(int));
    dep_left_count = (int *)calloc(N_NODES, sizeof(int));

    for (i = 0; i < t_used; i++) {
        n = t_gate[i];
        if (t_c1[i] != ND_vss && t_c1[i] != ND_vcc) dep_count[n]++;
        if (t_c2[i] != ND_vss && t_c2[i] != ND_vcc) dep_count[n]++;
        dep_left_count[n]++;
    }
    S.dep_offset = (uint16_t *)malloc((N_NODES + 1) * sizeof(uint16_t));
    S.dep_offset[0] = 0;
    for (i = 0; i < N_NODES; i++)
        S.dep_offset[i + 1] = S.dep_offset[i] + (uint16_t)dep_count[i];
    S.dep_left_offset = (uint16_t *)malloc((N_NODES + 1) * sizeof(uint16_t));
    S.dep_left_offset[0] = S.dep_offset[N_NODES];
    for (i = 0; i < N_NODES; i++)
        S.dep_left_offset[i + 1] = S.dep_left_offset[i] + (uint16_t)dep_left_count[i];

    dep_total = S.dep_left_offset[N_NODES];
    S.dep_block = (uint16_t *)calloc(dep_total, sizeof(uint16_t));

    memset(dep_count, 0, N_NODES * sizeof(int));
    memset(dep_left_count, 0, N_NODES * sizeof(int));
    for (i = 0; i < t_used; i++) {
        n = t_gate[i];
        if (t_c1[i] != ND_vss && t_c1[i] != ND_vcc)
            add_dep(S.dep_block, &dep_count[n], S.dep_offset[n], t_c1[i]);
        if (t_c2[i] != ND_vss && t_c2[i] != ND_vcc)
            add_dep(S.dep_block, &dep_count[n], S.dep_offset[n], t_c2[i]);
        left = (t_c1[i] != ND_vss && t_c1[i] != ND_vcc) ? t_c1[i] : t_c2[i];
        add_dep(S.dep_block, &dep_left_count[n], S.dep_left_offset[n], left);
    }

    free(dep_count);
    free(dep_left_count);
    free(t_gate);
    free(t_c1);
    free(t_c2);

#ifdef DEBUG_VERBOSE
    printf("cpu6502T: %d nodes, %d c1c2 entries, dep_block=%d entries\n", N_NODES, (int)S.c1c2_offset[N_NODES], (int)S.dep_left_offset[N_NODES]);
    { int total_dep = 0, total_left = 0;
      for (j = 0; j < N_NODES; j++) {
          total_dep += S.dep_offset[j+1] - S.dep_offset[j];
          total_left += S.dep_left_offset[j+1] - S.dep_left_offset[j];
      }
      printf("  dep total=%d left total=%d grand=%d\n", total_dep, total_left, total_dep+total_left);
    }
    /* Verify VSS/VCC/real sorting */
    { int vss_total = 0, vcc_total = 0, real_total = 0;
      for (j = 0; j < N_NODES; j++) {
          int vss = S.vss_count[j];
          int vcc = S.vcc_count[j];
          int total = S.c1c2_offset[j+1] - S.c1c2_offset[j];
          int start;
          vss_total += vss;
          vcc_total += vcc;
          real_total += total - vss - vcc;
          start = S.c1c2_offset[j];
          for (t = 0; t < vss; t++)
              if (S.c1c2s[start + t].other != ND_vss) {
                  printf("BUG: node %d: c1c2[%d].other=%d, expected VSS(%d)\n",
                         j, start + t, S.c1c2s[start + t].other, ND_vss);
              }
          for (t = vss; t < vss + vcc; t++)
              if (S.c1c2s[start + t].other != ND_vcc) {
                  printf("BUG: node %d: c1c2[%d].other=%d, expected VCC(%d)\n",
                         j, start + t, S.c1c2s[start + t].other, ND_vcc);
              }
      }
      printf("  VSS entries: %d, VCC entries: %d, Real entries: %d, Total: %d\n", vss_total, vcc_total, real_total, vss_total+vcc_total+real_total);
    }
#endif
}


/* ── Group resolution (recursive DFS, original perfect6502 algorithm) ── */

typedef enum {
    GV_NOTHING  = 0,
    GV_HI       = 1,
    GV_PULLUP   = 2,
    GV_PULLDOWN = 3,
    GV_VCC      = 4,
    GV_VSS      = 5
} group_value;

INLINE BOOL gv_to_bool(group_value gv) {
    return (gv == GV_VCC || gv == GV_PULLUP || gv == GV_HI);
}

INLINE group_value addNodeToGroup(int node) {
    int n, expand_ptr, start, vss_end, vcc_end, end, t, o;
    group_value val;

    if (node == ND_vss) return GV_VSS;
    if (node == ND_vcc) return GV_VCC;
    if (bm_get(S.group_bm, node)) return GV_NOTHING;

    val = GV_NOTHING;
    bm_set(S.group_bm, node);
    S.group[S.group_len++] = (uint16_t)node;
    expand_ptr = S.group_len - 1;

    while (expand_ptr < S.group_len) {
        n = S.group[expand_ptr++];
        if (bm_get(S.pulldown, n) && val < GV_PULLDOWN) val = GV_PULLDOWN;
        if (bm_get(S.pullup, n) && val < GV_PULLUP)   val = GV_PULLUP;
        if (bm_get(S.value, n) && val < GV_HI)         val = GV_HI;

        start   = S.c1c2_offset[n];
        vss_end = start + S.vss_count[n];
        vcc_end = vss_end + S.vcc_count[n];
        end     = S.c1c2_offset[n + 1];

        /* VSS entries: any gate on → group is VSS, short-circuit remaining VSS checks */
        for (t = start; t < vss_end; t++) {
            if (bm_get(S.value, S.c1c2s[t].gate)) { val = GV_VSS; goto vss_found; }
        }
        /* VCC entries: gate on upgrades val toward VCC (but VSS beats VCC) */
        for (t = vss_end; t < vcc_end; t++) {
            if (bm_get(S.value, S.c1c2s[t].gate)) { if (val != GV_VSS) val = GV_VCC; }
        }
        /* Real entries: gate on → expand group membership */
        for (t = vcc_end; t < end; t++) {
            if (bm_get(S.value, S.c1c2s[t].gate)) {
                o = S.c1c2s[t].other;
                if (o != ND_vss && o != ND_vcc && !bm_get(S.group_bm, o)) {
                    bm_set(S.group_bm, o);
                    S.group[S.group_len++] = (uint16_t)o;
                }
            }
        }
        continue;
    vss_found:
        /* After VSS found, still need to expand group membership from remaining entries */
        for (t = vss_end; t < end; t++) {
            if (bm_get(S.value, S.c1c2s[t].gate)) {
                o = S.c1c2s[t].other;
                if (o != ND_vss && o != ND_vcc && !bm_get(S.group_bm, o)) {
                    bm_set(S.group_bm, o);
                    S.group[S.group_len++] = (uint16_t)o;
                }
            }
        }
    }
    return val;
}

/* Clear group bitmap — only clear bits we set (faster than full memset) */
INLINE void group_clear(void) {
    int i;
    bm_t *gbm = S.group_bm;
    const uint16_t *grp = S.group;
    for (i = 0; i < S.group_len; i++)
        gbm[grp[i] >> BM_SHIFT] &= ~(BM_ONE << (grp[i] & BM_MASK));
    S.group_len = 0;
}

/* ── Node recalculation ─────────────────────────────────────────────── */

INLINE void listout_add(int n) {
    if (bm_get(S.listout_bm, n)) return;
    bm_set(S.listout_bm, n);
    S.listout[S.listout_len++] = (uint16_t)n;
}

INLINE void recalcNode(int node) {
    int i, n, g;
    group_value gv;
    BOOL newv;
    const uint16_t *grp;
    int glen;
    const uint16_t *dep_blk, *dep_off, *dep_loff;
    bm_t *val_bm;

    if (node == ND_vcc || node == ND_vss) return;

    group_clear();
    gv = addNodeToGroup(node);
    newv = gv_to_bool(gv);

    grp = S.group;
    glen = S.group_len;
    dep_blk  = S.dep_block;
    dep_off  = S.dep_offset;
    dep_loff = S.dep_left_offset;
    val_bm   = S.value;

    for (i = 0; i < glen; i++) {
        n = grp[i];
        if (n == ND_vcc || n == ND_vss) continue;
        if (bm_get(val_bm, n) != newv) {
            if (newv) bm_set(val_bm, n); else bm_clr(val_bm, n);
            if (newv) {
                for (g = dep_loff[n]; g < dep_loff[n + 1]; g++)
                    listout_add(dep_blk[g]);
            } else {
                for (g = dep_off[n]; g < dep_off[n + 1]; g++)
                    listout_add(dep_blk[g]);
            }
        }
    }
}

/* ── Stabilization (ping-pong, matches perfect6502's recalcNodeList) ── */

INLINE void recalcNodeList(void) {
    int j, i;
    for (j = 0; j < 50; j++) {
        uint16_t *tmp = S.listin;
        S.listin = S.listout;
        S.listin_len = S.listout_len;
        S.listout = tmp;
        S.listout_len = 0;
        if (S.listin_len == 0) break;
        bm_zero(S.listout_bm, N_NODES);
        for (i = 0; i < S.listin_len; i++)
            recalcNode(S.listin[i]);
    }
    bm_zero(S.listout_bm, N_NODES);
    S.listout_len = 0;
}

/* ── Node I/O ───────────────────────────────────────────────────────── */

INLINE void set_node(int node, BOOL val) {
    nd_set(S.pullup, node, val);
    nd_set(S.pulldown, node, !val);
    listout_add(node);
    recalcNodeList();
}

INLINE void write_nodes(int count, const int *nodes, uint16_t v) {
    int i, n;
    for (i = 0; i < count; i++, v >>= 1) {
        n = nodes[i];
        nd_set(S.pullup, n, v & 1);
        nd_set(S.pulldown, n, !(v & 1));
        listout_add(n);
    }
    recalcNodeList();
}

INLINE uint16_t read_nodes(int count, const int *nodes) {
    int i;
    bm_t *v = S.value;
    uint16_t result = 0;
    for (i = count - 1; i >= 0; i--) {
        result <<= 1;
        result |= (v[nodes[i] >> BM_SHIFT] >> (nodes[i] & BM_MASK)) & 1;
    }
    return result;
}

/* ── Half-cycle step ────────────────────────────────────────────────── */

INLINE void half_step(void) {
    BOOL clk;
    uint16_t addr;
    clk = nd_get(S.value, ND_clk0);
    set_node(ND_clk0, !clk);
    if (!clk) {
        addr = read_nodes(16, ND_ab);
        BOOL rw = nd_get(S.value, ND_rw);
        if (rw)
            write_nodes(8, ND_db, S.ram[addr]);
        else
            S.ram[addr] = (uint8_t)read_nodes(8, ND_db);
        /* Log bus cycle: every phase-2 access is one bus cycle */
        if (g_log_active) {
            if (g_log_len >= g_log_cap) {
                g_log_cap = g_log_cap ? g_log_cap * 2 : 256;
                g_log_buf = (CPU6502T_Cycle*)realloc(g_log_buf, g_log_cap * sizeof(CPU6502T_Cycle));
            }
            g_log_buf[g_log_len].addr = addr;
            g_log_buf[g_log_len].data = rw ? S.ram[addr] : (uint8_t)read_nodes(8, ND_db);
            g_log_buf[g_log_len].write = !rw;
            g_log_len++;
        }
    }
}

/* ── Register sync ──────────────────────────────────────────────────── */

static void sync_registers(CPU6502T *cpu) {
    cpu->A  = (uint8_t)read_nodes(8, ND_a);
    cpu->X  = (uint8_t)read_nodes(8, ND_x);
    cpu->Y  = (uint8_t)read_nodes(8, ND_y);
    cpu->SP = (uint8_t)read_nodes(8, ND_s);
    cpu->PC = (uint16_t)(read_nodes(8, ND_pcl) | (read_nodes(8, ND_pch) << 8));
    /* P register: netlist bit 5 (node 0 = VSS) always reads 0, but real 6502
       hardwires bit 5 to 1 (it has no physical flip-flop). Bit 4 (B) has no
       physical register either — it only exists when P is pushed to stack.
       Force U=1, B=0 to match instruction-level simulator expectations. */
    cpu->P  = ((uint8_t)read_nodes(8, ND_p) | P6502_U) & ~P6502_B;
}

static void queue_reg8(const int *nodes, uint8_t val) {
    int i, n;
    for (i = 0; i < 8; i++, val >>= 1) {
        n = nodes[i];
        nd_set(S.pullup, n, val & 1);
        nd_set(S.pulldown, n, !(val & 1));
        listout_add(n);
    }
}

static void sync_nodes_from_cpu(_CPUP) {
    bm_zero(S.listout_bm, N_NODES);
    S.listout_len = 0;
    queue_reg8(ND_a, cpu->A);
    queue_reg8(ND_x, cpu->X);
    queue_reg8(ND_y, cpu->Y);
    queue_reg8(ND_p, cpu->P);
    queue_reg8(ND_s, cpu->SP);
    queue_reg8(ND_pcl, (uint8_t)cpu->PC);
    queue_reg8(ND_pch, (uint8_t)(cpu->PC >> 8));
    recalcNodeList();
}

/* ── Public API ─────────────────────────────────────────────────────── */

void cpu6502T_init(_CPUC uint8_t *ram) {
    int i;

    if (!S.initialized) {
        build_state();
        S.initialized = TRUE;
    }

    cpu->ram = ram ? ram : g_ram;
    S.ram = cpu->ram;
    cpu->halted = FALSE;
    cpu->ctx = NULL;

    /* All nodes start low */
    bm_zero(S.value, N_NODES);
    bm_zero(S.pullup, N_NODES);
    bm_zero(S.pulldown, N_NODES);
    bm_zero(S.group_bm, N_NODES);
    S.group_len = 0;

    /* Set netlist pullup nodes */
    for (i = 0; i < N_NODES; i++)
        if (netlist_6502_node_is_pullup[i])
            bm_set(S.pullup, i);

    /* Set constant nodes */
    bm_set(S.value, ND_vcc);

    /* Initialize input pins one at a time (matches perfect6502) */
    S.listout_len = 0;
    bm_zero(S.listout_bm, N_NODES);
    set_node(ND_res,  0);  /* RES asserted (active low) */
    set_node(ND_clk0, 1);  /* CLK0 starts high */
    set_node(ND_rdy,  1);  /* RDY active */
    set_node(ND_so,   0);  /* SO inactive */
    set_node(ND_irq,  1);  /* IRQ inactive */
    set_node(ND_nmi,  1);  /* NMI inactive */

    /* Full chip stabilization */
    for (i = 0; i < N_NODES; i++)
        listout_add(i);
    recalcNodeList();
}

void cpu6502T_reset(_CPUP) {
    int i;
    cpu6502T_init(cpu, cpu->ram);

    /* Hold RES asserted for 8 cycles (16 half-steps) */
    for (i = 0; i < 16; i++)
        half_step();

    /* Release RES */
    set_node(ND_res, 1);

    /* Advance past reset sequence: skip the false SYNC during internal
       operations, then stop just before the real first instruction fetch.
       The false SYNC occurs ~2 cycles after RES release, then the CPU
       performs 3 dummy stack pushes and reads the reset vector. After
       ~18 half-steps total, SYNC goes high for the first instruction. */
    for (i = 0; i < 18; i++)
        half_step();

    sync_registers(cpu);
}

/* Lightweight reset: re-init nodes + reset sequence, but skip build_state().
   Much faster than cpu6502T_reset() when called repeatedly (e.g., test
   generators). Requires cpu6502T_init() to have been called at least once
   to set up netlist data structures. */
void cpu6502T_soft_reset(_CPUP) {
    int i;

    /* Re-initialize node state (skip build_state — already done once by init) */
    cpu->halted = FALSE;
    S.ram = cpu->ram;          /* ensure netlist sees current RAM pointer */
    bm_zero(S.value, N_NODES);
    bm_zero(S.pullup, N_NODES);
    bm_zero(S.pulldown, N_NODES);
    bm_zero(S.group_bm, N_NODES);
    S.group_len = 0;

    /* Set netlist pullup nodes */
    for (i = 0; i < N_NODES; i++)
        if (netlist_6502_node_is_pullup[i])
            bm_set(S.pullup, i);

    /* Set constant nodes */
    bm_set(S.value, ND_vcc);

    /* Initialize input pins */
    S.listout_len = 0;
    bm_zero(S.listout_bm, N_NODES);
    set_node(ND_res,  0);  /* RES asserted (active low) */
    set_node(ND_clk0, 1);  /* CLK0 starts high */
    set_node(ND_rdy,  1);  /* RDY active */
    set_node(ND_so,   0);  /* SO inactive */
    set_node(ND_irq,  1);  /* IRQ inactive */
    set_node(ND_nmi,  1);  /* NMI inactive */

    /* Full chip stabilization */
    for (i = 0; i < N_NODES; i++)
        listout_add(i);
    recalcNodeList();

    /* Hold RES asserted for 8 cycles (16 half-steps) */
    for (i = 0; i < 16; i++)
        half_step();

    /* Release RES */
    set_node(ND_res, 1);

    /* Advance past reset sequence */
    for (i = 0; i < 18; i++)
        half_step();

    sync_registers(cpu);
}

void cpu6502T_irq(_CPUC BOOL force) {
    (void)_CPUPA;
    set_node(ND_irq, 0);  /* assert IRQ (active-low) */
    (void)force;
}

void cpu6502T_irq_release(_CPUP) {
    (void)_CPUPA;
    set_node(ND_irq, 1);  /* release IRQ (set HIGH = inactive) */
}

BOOL cpu6502T_is_halted(_CCPUP) { return cpu->halted; }

void cpu6502T_nmi(_CPUP) {
    (void)_CPUPA;
    set_node(ND_nmi, 0);
}

CYCLES cpu6502T_step(_CPUP) {
    int sync, sync_went_low = 0;
    STEPSTART

    for (int _cyc = 0; ; _cyc++) {
        half_step();
        half_step();
        CYC(1);

        sync = nd_get(S.value, ND_sync);
        if (!sync) sync_went_low = 1;
        if (sync && sync_went_low) break;
        if (_cyc >= 100) { cpu->halted = 1; break; } /* safety: stuck netlist (KIL-like) */
    }

    sync_registers(cpu);
    return STEPRET;
}

void cpu6502T_flush(_CPUP) {
    /* After cpu6502T_step(), register write-back may not have fully
       propagated through the netlist. Two half-cycles let values settle.
       This advances the CPU by 1 cycle, so the caller should save
       cpu->PC before calling and restore it after calling cpu6502T_get(). */
    half_step();
    half_step();
}

void cpu6502T_half_step(_CPUP) {
    (void)_CPUPA;
    half_step();
}

uint8_t cpu6502T_get_node(int node) {
    return (node >= 0 && node < N_NODES) ? nd_get(S.value, node) : 0;
}


void cpu6502T_set_node(int node, BOOL val) {
    if (node >= 0 && node < N_NODES && node != ND_vcc && node != ND_vss)
        set_node(node, val);
}


/* Sync struct fields <-> internal node state */
void cpu6502T_get(_CPUP) { sync_registers(cpu); }

/* ── Safe register set via boot loader (LDA/LDX/TXS/LDX/LDY/RTI) ────
 *
 * Writes A,X,Y,SP,P,PC into the CPU and settles ALL 1725 transistor nodes
 * by running a tiny boot loader via soft_reset. Saves/restores clobbered
 * RAM bytes so user memory is untouched.
 *
 * Layout used temporarily:
 *   $0200: NOP          ; entry after reset
 *   $0201: LDA #a       ; set accumulator
 *   $0203: LDX #sp      ; load SP value
 *   $0205: TXS           ; transfer to SP
 *   $0206: LDX #x       ; set X
 *   $0208: LDY #y       ; set Y
 *   $020A: RTI           ; pop P and PC from stack, jump to test
 *   Stack (page 1): P at $0100+(sp+1), PC lo at $0100+(sp+2), PC hi at $0100+(sp+3)
 *   Vectors: $FFFC/$FFFD = $0200 (reset)
 */
void cpu6502T_set(_CPUP, u16 pc, u8 a, u8 x, u8 y, u8 sp, u8 p) {
	u8 *r = cpu->ram;

	/* RTI pops 3 bytes (P, PClo, PChi), incrementing SP by 3.
	   So TXS loads (sp-3) so that after RTI, SP = sp. */
	u8 sp_base = (u8)(sp - 3);

	/* Stack frame at $0100+sp-2, $0100+sp-1, $0100+sp */
	u16 sa = 0x0100 + (u8)(sp - 2);  /* P */
	u16 sb = 0x0100 + (u8)(sp - 1);  /* PC lo */
	u16 sc = 0x0100 + (u8)(sp);       /* PC hi */

	/* Save clobbered regions */
	u8 save_vec[2];       /* $FFFC/$FFFD */
	u8 save_code[11];     /* $0200-$020A */
	u8 save_stack[256];   /* entire stack page $0100-$01FF */
	save_vec[0] = r[0xFFFC]; save_vec[1] = r[0xFFFD];
	memcpy(save_code, &r[0x0200], 11);
	memcpy(save_stack, &r[0x0100], 256);

	/* Write boot loader */
	r[0xFFFC] = 0x00; r[0xFFFD] = 0x02;  /* reset vector → $0200 */
	r[0x0200] = 0xEA;                      /* NOP */
	r[0x0201] = 0xA9; r[0x0202] = a;       /* LDA #a */
	r[0x0203] = 0xA2; r[0x0204] = sp_base; /* LDX #(sp-3) — TXS will set SP so RTI ends at sp */
	r[0x0205] = 0x9A;                       /* TXS */
	r[0x0206] = 0xA2; r[0x0207] = x;       /* LDX #x */
	r[0x0208] = 0xA0; r[0x0209] = y;       /* LDY #y */
	r[0x020A] = 0x40;                       /* RTI */
	r[sa] = p;                              /* P on stack */
	r[sb] = (u8)(pc & 0xFF);               /* PC lo */
	r[sc] = (u8)(pc >> 8);                  /* PC hi */

	/* Run boot: soft_reset + 7 instructions (NOP/LDA/LDX/TXS/LDX/LDY/RTI).
	   After RTI, all registers are set and PC points to the target address.
	   No flush needed — sync_registers() at the SYNC boundary gives correct
	   register values (the netlist settles by the time SYNC goes high). */
	cpu6502T_soft_reset(cpu);
	for (int i = 0; i < 7; i++)
		cpu6502T_step(cpu);

	/* Restore clobbered RAM */
	r[0xFFFC] = save_vec[0]; r[0xFFFD] = save_vec[1];
	memcpy(&r[0x0200], save_code, 11);
	memcpy(&r[0x0100], save_stack, 256);
}

/* ── Debug ────────────────────────────────────────────────────────────── */

#ifdef DEBUG
void cpu6502T_dump(const CPU6502T *cpu) {
	printf("PC=%04X A=%02X X=%02X Y=%02X P=%02X:%c%c%c%c%c%c%c%c SP=%02X\n", cpu->PC, cpu->A, cpu->X, cpu->Y, cpu->P, cpu->P & P6502_N ? 'N' : 'n', cpu->P & P6502_V ? 'V' : 'v', cpu->P & P6502_U ? 'U' : 'u', cpu->P & P6502_B ? 'B' : 'b', cpu->P & P6502_D ? 'D' : 'd', cpu->P & P6502_I ? 'I' : 'i', cpu->P & P6502_Z ? 'Z' : 'z', cpu->P & P6502_C ? 'C' : 'c', cpu->SP);
}

void cpu6502T_debug_nodes(void) {
    int i, ones = 0;
    printf("VCC=%d VSS=%d CLK0=%d IRQ=%d NMI=%d RDY=%d RES=%d SYNC=%d RW=%d\n",
           nd_get(S.value, 657), nd_get(S.value, 558), nd_get(S.value, 1171), nd_get(S.value, 103),
           nd_get(S.value, 1297), nd_get(S.value, 89), nd_get(S.value, 159), nd_get(S.value, 539), nd_get(S.value, 1156));
    printf("A: "); for (i = 0; i < 8; i++) printf("%d", nd_get(S.value, ND_a[i]));
    printf("  X: "); for (i = 0; i < 8; i++) printf("%d", nd_get(S.value, ND_x[i]));
    printf("  Y: "); for (i = 0; i < 8; i++) printf("%d", nd_get(S.value, ND_y[i]));
    printf("  P: "); for (i = 0; i < 8; i++) printf("%d", nd_get(S.value, ND_p[i]));
    printf("  S: "); for (i = 0; i < 8; i++) printf("%d", nd_get(S.value, ND_s[i]));
    printf("\nPCL: "); for (i = 0; i < 8; i++) printf("%d", nd_get(S.value, ND_pcl[i]));
    printf("  PCH: "); for (i = 0; i < 8; i++) printf("%d", nd_get(S.value, ND_pch[i]));
    for (i = 0; i < N_NODES; i++) if (nd_get(S.value, i)) ones++;
    printf("\nNodes high: %d/%d\n", ones, N_NODES);
}
#endif

/* ── Boot state save/restore (for test generators) ──────────────────── */

struct CPU6502T_SavedState {
    bm_t value[BM_WORDS(N_NODES)];
    bm_t pullup[BM_WORDS(N_NODES)];
    bm_t pulldown[BM_WORDS(N_NODES)];
};

size_t cpu6502T_saved_state_size(void) { return sizeof(CPU6502T_SavedState); }

void cpu6502T_save_boot(CPU6502T_SavedState *dst) {
    int bw = BM_WORDS(N_NODES);
    memcpy(dst->value,    S.value,    bw * sizeof(bm_t));
    memcpy(dst->pullup,   S.pullup,   bw * sizeof(bm_t));
    memcpy(dst->pulldown, S.pulldown, bw * sizeof(bm_t));
}

void cpu6502T_restore_boot(const CPU6502T_SavedState *src) {
    int i, bw = BM_WORDS(N_NODES);
    memcpy(S.value,    src->value,    bw * sizeof(bm_t));
    memcpy(S.pullup,   src->pullup,   bw * sizeof(bm_t));
    memcpy(S.pulldown, src->pulldown, bw * sizeof(bm_t));
    /* Clear transient stabilization state */
    S.listin_len  = 0;
    S.listout_len = 0;
    S.group_len   = 0;
    bm_zero(S.listout_bm, N_NODES);
    bm_zero(S.group_bm,   N_NODES);
    /* Full chip stabilization: propagate restored values through all
       transistor networks so internal nodes (ALU, PLA, timing) are
       consistent with the restored register/control values */
    for (i = 0; i < N_NODES; i++)
        listout_add(i);
    recalcNodeList();
}
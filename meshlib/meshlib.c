/*
 * meshlib.c - QEM（二次误差测度）边折叠网格简化实现
 *
 * 算法参考: Garland & Heckbert, "Surface Simplification Using
 * Quadric Error Metrics", SIGGRAPH 1998。
 *
 * 每次折叠选择当前误差最小的边，合并两端点，删除退化为边的邻面，
 * 并用最小堆维护候选边。任务可通过进度回调随时取消，取消点位于
 * 两次折叠之间，因此网格始终保持结构一致；所有工作内存在取消后
 * 同样立即释放。
 */
#include "meshlib.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESHLIB_VERSION "1.0.0"

struct meshlib_mesh {
    int nv, nf;
    float (*v)[3];
    int (*f)[3];
};

/* ------------------------------ 工具函数 ------------------------------ */

static void set_err(char **err_msg, const char *msg) {
    if (!err_msg) return;
    *err_msg = NULL;
    if (!msg) return;
    size_t n = strlen(msg) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, msg, n);
    *err_msg = p;
}

static void set_errf(char **err_msg, const char *fmt, const char *arg) {
    if (!err_msg) return;
    char buf[512];
    snprintf(buf, sizeof(buf), fmt, arg);
    set_err(err_msg, buf);
}

static void *xcalloc(size_t n, size_t s) { return calloc(n, s); }

/* 动态 int 数组 */
typedef struct {
    int *d;
    int len, cap;
} iarr_t;

static int ia_push(iarr_t *a, int x) {
    if (a->len == a->cap) {
        int nc = a->cap ? a->cap * 2 : 8;
        int *nd = (int *)realloc(a->d, (size_t)nc * sizeof(int));
        if (!nd) return -1;
        a->d = nd;
        a->cap = nc;
    }
    a->d[a->len++] = x;
    return 0;
}

/* ------------------------------ OBJ 读取 ------------------------------ */

/* 自有 getline，保证跨平台（含 Windows / mingw）可用 */
static int read_line(FILE *fp, char **buf, size_t *cap) {
    size_t len = 0;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= *cap) {
            size_t nc = *cap ? *cap * 2 : 256;
            char *nb = (char *)realloc(*buf, nc);
            if (!nb) return -1;
            *buf = nb;
            *cap = nc;
        }
        (*buf)[len++] = (char)c;
        if (c == '\n') break;
    }
    if (len == 0 && c == EOF) return 0;
    (*buf)[len] = '\0';
    return 1;
}

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

struct mesh_builder {
    float (*v)[3];
    int vc, vcap;
    int (*f)[3];
    int fc, fcap;
};

static int bld_add_vertex(struct mesh_builder *b, double x, double y, double z) {
    if (b->vc == b->vcap) {
        int nc = b->vcap ? b->vcap * 2 : 256;
        float (*nv)[3] = (float(*)[3])realloc(b->v, (size_t)nc * 3 * sizeof(float));
        if (!nv) return -1;
        b->v = nv;
        b->vcap = nc;
    }
    b->v[b->vc][0] = (float)x;
    b->v[b->vc][1] = (float)y;
    b->v[b->vc][2] = (float)z;
    b->vc++;
    return 0;
}

static int bld_add_face(struct mesh_builder *b, int a, int c, int d) {
    if (b->fc == b->fcap) {
        int nc = b->fcap ? b->fcap * 2 : 512;
        int (*nf)[3] = (int(*)[3])realloc(b->f, (size_t)nc * 3 * sizeof(int));
        if (!nf) return -1;
        b->f = nf;
        b->fcap = nc;
    }
    b->f[b->fc][0] = a;
    b->f[b->fc][1] = c;
    b->f[b->fc][2] = d;
    b->fc++;
    return 0;
}

meshlib_mesh *meshlib_load_obj(const char *path, char **err_msg) {
    if (!path) { set_err(err_msg, "nil path"); return NULL; }
    FILE *fp = fopen(path, "rb");
    if (!fp) { set_errf(err_msg, "cannot open file: %s", path); return NULL; }

    struct mesh_builder b;
    memset(&b, 0, sizeof(b));

    char *line = NULL;
    size_t linecap = 0;
    int read_ok = 1;
    int r;
    while ((r = read_line(fp, &line, &linecap)) == 1) {
        const char *s = line;
        s = skip_ws(s);
        if (s[0] == 'v' && isspace((unsigned char)s[1])) {
            s += 2;
            double x = 0, y = 0, z = 0;
            if (sscanf(s, "%lf %lf %lf", &x, &y, &z) == 3) {
                if (bld_add_vertex(&b, x, y, z) != 0) { read_ok = 0; break; }
            }
        } else if (s[0] == 'f' && isspace((unsigned char)s[1])) {
            int idx[64];
            int ni = 0;
            s += 2;
            s = skip_ws(s);
            while (*s && ni < 64) {
                if (isspace((unsigned char)*s)) { s = skip_ws(s); continue; }
                char *end = NULL;
                long v = strtol(s, &end, 10);
                if (end == s) break;
                s = end;
                if (*s == '/') {
                    s++;
                    if (*s == '/') s++;
                    else while (*s && *s != '/' && !isspace((unsigned char)*s)) s++;
                    if (*s == '/') {
                        s++;
                        while (*s && !isspace((unsigned char)*s)) s++;
                    }
                }
                if (v == 0) { read_ok = 0; set_err(err_msg, "invalid face index (0)"); break; }
                int vi = v > 0 ? (int)v - 1 : b.vc + (int)v;
                if (vi < 0 || vi >= b.vc) { read_ok = 0; set_err(err_msg, "face index out of range"); break; }
                idx[ni++] = vi;
            }
            if (!read_ok) break;
            for (int i = 1; i < ni - 1; i++) {
                if (bld_add_face(&b, idx[0], idx[i], idx[i + 1]) != 0) { read_ok = 0; break; }
            }
            if (!read_ok) break;
        }
    }
    if (r < 0) read_ok = 0;

    free(line);
    fclose(fp);

    if (!read_ok) {
        free(b.v);
        free(b.f);
        if (err_msg && !*err_msg) set_err(err_msg, "out of memory while parsing obj");
        return NULL;
    }
    if (b.vc < 3 || b.fc < 1) {
        free(b.v);
        free(b.f);
        set_err(err_msg, "obj contains no usable mesh data");
        return NULL;
    }

    meshlib_mesh *m = (meshlib_mesh *)xcalloc(1, sizeof(*m));
    if (!m) { free(b.v); free(b.f); set_err(err_msg, "out of memory"); return NULL; }
    m->nv = b.vc;
    m->nf = b.fc;
    m->v = b.v;
    m->f = b.f;
    return m;
}

/* ------------------------------ OBJ 导出 ------------------------------ */

int meshlib_save_obj(const meshlib_mesh *m, const char *path, char **err_msg) {
    if (!m || !path) { set_err(err_msg, "nil argument"); return 2; }
    FILE *fp = fopen(path, "wb");
    if (!fp) { set_errf(err_msg, "cannot create file: %s", path); return 2; }
    fprintf(fp, "# generated by meshlib %s\n", MESHLIB_VERSION);
    for (int i = 0; i < m->nv; i++)
        fprintf(fp, "v %g %g %g\n", m->v[i][0], m->v[i][1], m->v[i][2]);
    for (int i = 0; i < m->nf; i++)
        fprintf(fp, "f %d %d %d\n", m->f[i][0] + 1, m->f[i][1] + 1, m->f[i][2] + 1);
    int bad = ferror(fp);
    if (fclose(fp) != 0) bad = 1;
    if (bad) { set_errf(err_msg, "write failed: %s", path); return 2; }
    return 0;
}

/* ------------------------------ QEM 简化 ------------------------------ */

/* 对称 4x4 矩阵，压缩为 10 个元素：
 * 0=11 1=12 2=13 3=14
 *       4=22 5=23 6=24
 *             7=33 8=34
 *                   9=44 */
static const int QIDX[4][4] = {
    {0, 1, 2, 3},
    {1, 4, 5, 6},
    {2, 5, 7, 8},
    {3, 6, 8, 9},
};

typedef struct {
    int a, b;          /* 端点（存活时 a<b 不做强制要求，构建时保证） */
    iarr_t faces;      /* 邻接面 */
    int dead;
    int contr_mark;    /* 单次折叠内的去重标记 */
    float err;
    int heap_pos;      /* 在堆中的位置，-1 表示不在堆中 */
} edge_t;

/* 边哈希：开放寻址，key 0 为空槽，UINT64_MAX 为删除墓碑 */
#define HKEY_EMPTY 0ULL
#define HKEY_TOMB  UINT64_MAX

typedef struct {
    uint64_t *keys;
    int *vals;
    int cap;
    int count;
} emap_t;

static uint64_t edge_key(int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    uint64_t k = (uint64_t)(unsigned)a * 2654435761u
               + (uint64_t)(unsigned)b * 40503u + 1u;
    if (k == HKEY_EMPTY || k == HKEY_TOMB) k = 0x9E3779B97F4A7C15ULL;
    return k;
}

static int emap_grow(emap_t *m, int newcap) {
    uint64_t *nk = (uint64_t *)malloc((size_t)newcap * sizeof(uint64_t));
    int *nv = (int *)malloc((size_t)newcap * sizeof(int));
    if (!nk || !nv) { free(nk); free(nv); return -1; }
    memset(nk, 0, (size_t)newcap * sizeof(uint64_t));
    for (int i = 0; i < m->cap; i++) {
        uint64_t k = m->keys[i];
        if (k == HKEY_EMPTY || k == HKEY_TOMB) continue;
        uint64_t mask = (uint64_t)newcap - 1;
        uint64_t h = k & mask;
        while (nk[h] != HKEY_EMPTY) h = (h + 1) & mask;
        nk[h] = k;
        nv[h] = m->vals[i];
    }
    free(m->keys);
    free(m->vals);
    m->keys = nk;
    m->vals = nv;
    m->cap = newcap;
    return 0;
}

/* 查找或创建。*existed 指示是否已存在。失败返回 -1。 */
static int emap_get(emap_t *m, uint64_t key, int create, int *existed) {
    if (existed) *existed = 0;
    if (m->cap == 0) {
        if (!create) return -1;
        if (emap_grow(m, 1024) != 0) return -1;
    }
    if (create && (m->count + 1) * 10 >= m->cap * 7) {
        if (emap_grow(m, m->cap * 2) != 0) return -1;
    }
    uint64_t mask = (uint64_t)m->cap - 1;
    uint64_t h = key & mask;
    int first_tomb = -1;
    for (;;) {
        uint64_t k = m->keys[h];
        if (k == HKEY_EMPTY) {
            if (!create) return -1;
            int slot = first_tomb >= 0 ? first_tomb : (int)h;
            m->keys[slot] = key;
            m->vals[slot] = -1;
            m->count++;
            return slot;
        }
        if (k == key) { if (existed) *existed = 1; return (int)h; }
        if (k == HKEY_TOMB && first_tomb < 0) first_tomb = (int)h;
        h = (h + 1) & mask;
    }
}

static void emap_erase(emap_t *m, uint64_t key) {
    if (m->cap == 0) return;
    uint64_t mask = (uint64_t)m->cap - 1;
    uint64_t h = key & mask;
    for (;;) {
        uint64_t k = m->keys[h];
        if (k == HKEY_EMPTY) return;
        if (k == key) {
            m->keys[h] = HKEY_TOMB;
            m->count--;
            return;
        }
        h = (h + 1) & mask;
    }
}

typedef struct {
    /* 工作副本数据 */
    float (*p)[3];
    float (*q)[10];
    int (*f)[3];
    char *fdead;
    char *vdead;
    iarr_t *vf;          /* 每个顶点的邻接面（含历史死面，遍历时过滤） */

    int nv, nf, nfalive;

    edge_t *edges;
    int ne, ecap;
    emap_t emap;

    int *heap;
    int hlen;

    int *fstamp;         /* 面去重时间戳（按面 id） */
    int stamp;
} swork_t;

static double det3(double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
         - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
         + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

/* 计算边的最优折叠点与误差 */
static void edge_cost(swork_t *w, int a, int b, float pos[3], float *err) {
    float *qa = w->q[a], *qb = w->q[b];
    float q[10];
    for (int i = 0; i < 10; i++) q[i] = qa[i] + qb[i];

    double A[3][3] = {
        {q[0], q[1], q[2]},
        {q[1], q[4], q[5]},
        {q[2], q[5], q[7]},
    };
    double d = det3(A);
    double px, py, pz;
    if (fabs(d) > 1e-12) {
        double Bx = -q[3], By = -q[6], Bz = -q[8];
        double Mx[3][3] = {{Bx, A[0][1], A[0][2]},
                           {By, A[1][1], A[1][2]},
                           {Bz, A[2][1], A[2][2]}};
        double My[3][3] = {{A[0][0], Bx, A[0][2]},
                           {A[1][0], By, A[1][2]},
                           {A[2][0], Bz, A[2][2]}};
        double Mz[3][3] = {{A[0][0], A[0][1], Bx},
                           {A[1][0], A[1][1], By},
                           {A[2][0], A[2][1], Bz}};
        px = det3(Mx) / d;
        py = det3(My) / d;
        pz = det3(Mz) / d;
        if (!isfinite(px) || !isfinite(py) || !isfinite(pz) ||
            fabs(px) + fabs(py) + fabs(pz) > 1e15) {
            goto midpoint;
        }
    } else {
midpoint:
        px = ((double)w->p[a][0] + w->p[b][0]) * 0.5;
        py = ((double)w->p[a][1] + w->p[b][1]) * 0.5;
        pz = ((double)w->p[a][2] + w->p[b][2]) * 0.5;
    }

    double e = q[0]*px*px + 2*q[1]*px*py + 2*q[2]*px*pz + 2*q[3]*px
             + q[4]*py*py + 2*q[5]*py*pz + 2*q[6]*py
             + q[7]*pz*pz + 2*q[8]*pz + q[9];
    pos[0] = (float)px; pos[1] = (float)py; pos[2] = (float)pz;
    *err = isfinite(e) ? (e < 0 ? 0.f : (e > 1e30f ? 1e30f : (float)e)) : 1e30f;
}

/* ------------------------------ 最小堆 ------------------------------ */

static int heap_better(swork_t *w, int i, int j) {
    return w->edges[i].err < w->edges[j].err;
}

static void heap_swap(swork_t *w, int i, int j) {
    int *h = w->heap;
    int t = h[i]; h[i] = h[j]; h[j] = t;
    w->edges[h[i]].heap_pos = i;
    w->edges[h[j]].heap_pos = j;
}

static void heap_up(swork_t *w, int i) {
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!heap_better(w, w->heap[i], w->heap[p])) break;
        heap_swap(w, i, p);
        i = p;
    }
}

static void heap_down(swork_t *w, int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, best = i;
        if (l < w->hlen && heap_better(w, w->heap[l], w->heap[best])) best = l;
        if (r < w->hlen && heap_better(w, w->heap[r], w->heap[best])) best = r;
        if (best == i) break;
        heap_swap(w, i, best);
        i = best;
    }
}

static int heap_push(swork_t *w, int eid) {
    int *nh = (int *)realloc(w->heap, (size_t)(w->hlen + 1) * sizeof(int));
    if (!nh) return -1;
    w->heap = nh;
    int i = w->hlen++;
    w->heap[i] = eid;
    w->edges[eid].heap_pos = i;
    heap_up(w, i);
    return 0;
}

static int heap_pop(swork_t *w) {
    int top = w->heap[0];
    w->heap[0] = w->heap[--w->hlen];
    if (w->hlen > 0) {
        w->edges[w->heap[0]].heap_pos = 0;
        heap_down(w, 0);
    }
    w->edges[top].heap_pos = -1;
    return top;
}

/* ------------------------------ 简化工作区 ------------------------------ */

static int new_edge(swork_t *w, int a, int b) {
    if (w->ne == w->ecap) {
        int nc = w->ecap ? w->ecap * 2 : 4096;
        edge_t *ne2 = (edge_t *)realloc(w->edges, (size_t)nc * sizeof(edge_t));
        if (!ne2) return -1;
        w->edges = ne2;
        w->ecap = nc;
    }
    int id = w->ne++;
    edge_t *e = &w->edges[id];
    memset(e, 0, sizeof(*e));
    e->a = a; e->b = b;
    e->heap_pos = -1;
    return id;
}

static int get_edge(swork_t *w, int a, int b) {
    uint64_t key = edge_key(a, b);
    int existed = 0;
    int slot = emap_get(&w->emap, key, 1, &existed);
    if (slot < 0) return -1;
    if (existed && w->emap.vals[slot] >= 0) return w->emap.vals[slot];
    int id = new_edge(w, a < b ? a : b, a < b ? b : a);
    if (id < 0) return -1;
    w->emap.vals[slot] = id;
    return id;
}

static void kill_edge(swork_t *w, int eid) {
    edge_t *e = &w->edges[eid];
    if (e->dead) return;
    e->dead = 1;
    emap_erase(&w->emap, edge_key(e->a, e->b));
}

static int add_face_quadric(swork_t *w, int fid) {
    const float *p0 = w->p[w->f[fid][0]];
    const float *p1 = w->p[w->f[fid][1]];
    const float *p2 = w->p[w->f[fid][2]];
    double ux = p1[0] - p0[0], uy = p1[1] - p0[1], uz = p1[2] - p0[2];
    double vx = p2[0] - p0[0], vy = p2[1] - p0[1], vz = p2[2] - p0[2];
    double nx = uy * vz - uz * vy;
    double ny = uz * vx - ux * vz;
    double nz = ux * vy - uy * vx;
    double len = sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-20) return 0; /* 退化面，不贡献误差 */
    nx /= len; ny /= len; nz /= len;
    double d = -(nx * p0[0] + ny * p0[1] + nz * p0[2]);
    double pl[4] = {nx, ny, nz, d};
    float K[10];
    for (int r = 0; r < 4; r++)
        for (int c = r; c < 4; c++)
            K[QIDX[r][c]] = (float)(pl[r] * pl[c]);
    for (int k = 0; k < 3; k++) {
        float *q = w->q[w->f[fid][k]];
        for (int t = 0; t < 10; t++) q[t] += K[t];
    }
    return 0;
}

/* 重建边的邻接面与误差并入堆（用于折叠后新建的边） */
static int edge_attach_face(swork_t *w, int eid, int fid) {
    edge_t *e = &w->edges[eid];
    for (int i = 0; i < e->faces.len; i++)
        if (e->faces.d[i] == fid) return 0;
    return ia_push(&e->faces, fid);
}

static void work_free(swork_t *w) {
    free(w->p);
    free(w->q);
    free(w->f);
    free(w->fdead);
    free(w->vdead);
    if (w->vf) {
        for (int i = 0; i < w->nv; i++) free(w->vf[i].d);
        free(w->vf);
    }
    for (int i = 0; i < w->ne; i++) free(w->edges[i].faces.d);
    free(w->edges);
    free(w->emap.keys);
    free(w->emap.vals);
    free(w->heap);
    free(w->fstamp);
}

static int edge_push_fresh(swork_t *w, int eid) {
    float pos[3], err;
    edge_cost(w, w->edges[eid].a, w->edges[eid].b, pos, &err);
    w->edges[eid].err = err;
    return heap_push(w, eid);
}

/* 折叠一条边，返回 0 成功，-1 内存失败 */
static int contract(swork_t *w, int eid) {
    edge_t *e = &w->edges[eid];
    int u = e->a, v = e->b;

    /* 1. 收集受影响的面（u/v 的邻接面并集） */
    w->stamp++;
    int *faces = NULL;
    int fcap = 0, flen = 0;
    for (int vi = 0; vi < 2; vi++) {
        int vertex = vi == 0 ? u : v;
        for (int k = 0; k < w->vf[vertex].len; k++) {
            int fid = w->vf[vertex].d[k];
            if (w->fdead[fid]) continue;
            if (w->fstamp[fid] == w->stamp) continue;
            w->fstamp[fid] = w->stamp;
            if (flen == fcap) {
                int nc = fcap ? fcap * 2 : 16;
                int *nf2 = (int *)realloc(faces, (size_t)nc * sizeof(int));
                if (!nf2) return -1;
                faces = nf2; fcap = nc;
            }
            faces[flen++] = fid;
        }
    }

    /* 2. 分类：含 u、v 两端的面退化为边 */
    int *surv = NULL;
    int scap = 0, slen = 0;
    for (int i = 0; i < flen; i++) {
        int fid = faces[i];
        int hasu = 0, hasv = 0;
        for (int k = 0; k < 3; k++) {
            if (w->f[fid][k] == u) hasu = 1;
            if (w->f[fid][k] == v) hasv = 1;
        }
        if (hasu && hasv) {
            w->fdead[fid] = 1;
            w->nfalive--;
        } else {
            if (slen == scap) {
                int nc = scap ? scap * 2 : 16;
                int *ns = (int *)realloc(surv, (size_t)nc * sizeof(int));
                if (!ns) { free(faces); return -1; }
                surv = ns; scap = nc;
            }
            surv[slen++] = fid;
        }
    }

    /* 3. 删除所有与 u、v 相接的边（稍后按新拓扑重建） */
    int *killed = NULL;
    int kcap = 0, klen = 0;
    for (int i = 0; i < flen; i++) {
        int fid = faces[i];
        for (int k = 0; k < 3; k++) {
            int x = w->f[fid][k], y = w->f[fid][(k + 1) % 3];
            if (x != u && x != v && y != u && y != v) continue;
            /* 有序边 id 查找 */
            int lo = x < y ? x : y, hi = x < y ? y : x;
            uint64_t key = edge_key(lo, hi);
            int slot = emap_get(&w->emap, key, 0, NULL);
            if (slot < 0) continue;
            int e2 = w->emap.vals[slot];
            if (e2 < 0) continue;
            if (w->edges[e2].contr_mark == w->stamp) continue;
            w->edges[e2].contr_mark = w->stamp;
            if (klen == kcap) {
                int nc = kcap ? kcap * 2 : 16;
                int *nk2 = (int *)realloc(killed, (size_t)nc * sizeof(int));
                if (!nk2) { free(faces); free(surv); return -1; }
                killed = nk2; kcap = nc;
            }
            killed[klen++] = e2;
        }
    }
    for (int i = 0; i < klen; i++) kill_edge(w, killed[i]);
    free(killed);

    /* 4. 合并：位置取最优折叠点，二次误差相加，v 标记死亡 */
    float npos[3], nerr;
    edge_cost(w, u, v, npos, &nerr);
    w->p[u][0] = npos[0]; w->p[u][1] = npos[1]; w->p[u][2] = npos[2];
    for (int t = 0; t < 10; t++) w->q[u][t] += w->q[v][t];
    w->vdead[v] = 1;

    /* 5. 存活面中把 v 替换为 u */
    for (int i = 0; i < slen; i++) {
        int fid = surv[i];
        for (int k = 0; k < 3; k++)
            if (w->f[fid][k] == v) w->f[fid][k] = u;
    }

    /* 6. 重建 u 的邻接面表（恰为存活面集合）；v 的表清空 */
    free(w->vf[u].d);
    w->vf[u].d = NULL; w->vf[u].len = 0; w->vf[u].cap = 0;
    for (int i = 0; i < slen; i++)
        if (ia_push(&w->vf[u], surv[i]) != 0) { free(faces); free(surv); return -1; }
    w->vf[v].len = 0;

    /* 7. 按存活面重建边并入堆 */
    for (int i = 0; i < slen; i++) {
        int fid = surv[i];
        for (int k = 0; k < 3; k++) {
            int x = w->f[fid][k], y = w->f[fid][(k + 1) % 3];
            int lo = x < y ? x : y, hi = x < y ? y : x;
            if (lo == hi) continue;
            int e2 = get_edge(w, lo, hi);
            if (e2 < 0) { free(faces); free(surv); return -1; }
            int newly = w->edges[e2].faces.len == 0;
            if (edge_attach_face(w, e2, fid) != 0) { free(faces); free(surv); return -1; }
            if (newly && edge_push_fresh(w, e2) != 0) { free(faces); free(surv); return -1; }
        }
    }

    free(faces);
    free(surv);
    return 0;
}

/* 将当前工作区（部分简化结果）回写到 mesh，并释放工作区 */
static int work_commit(swork_t *w, meshlib_mesh *m) {
    int nf = 0, nv = 0;
    for (int i = 0; i < w->nf; i++) if (!w->fdead[i]) nf++;
    for (int i = 0; i < w->nv; i++) if (!w->vdead[i]) nv++;

    float (*nv2)[3] = (float(*)[3])malloc((size_t)nv * 3 * sizeof(float));
    int (*nf3)[3] = (int(*)[3])malloc((size_t)nf * 3 * sizeof(int));
    int *remap = (int *)malloc((size_t)w->nv * sizeof(int));
    if (!nv2 || !nf3 || !remap) {
        free(nv2); free(nf3); free(remap);
        return -1;
    }
    int k = 0;
    for (int i = 0; i < w->nv; i++) {
        if (w->vdead[i]) { remap[i] = -1; continue; }
        remap[i] = k;
        nv2[k][0] = w->p[i][0]; nv2[k][1] = w->p[i][1]; nv2[k][2] = w->p[i][2];
        k++;
    }
    k = 0;
    for (int i = 0; i < w->nf; i++) {
        if (w->fdead[i]) continue;
        nf3[k][0] = remap[w->f[i][0]];
        nf3[k][1] = remap[w->f[i][1]];
        nf3[k][2] = remap[w->f[i][2]];
        k++;
    }
    free(remap);

    free(m->v);
    free(m->f);
    m->v = nv2;
    m->f = nf3;
    m->nv = nv;
    m->nf = nf;
    return 0;
}

int meshlib_simplify(meshlib_mesh *m, double ratio,
                     meshlib_progress_cb cb, void *user_data,
                     char **err_msg) {
    if (!m) { set_err(err_msg, "nil mesh"); return 2; }
    if (!(ratio > 0.0) || ratio > 1.0) { set_err(err_msg, "ratio must be in (0,1]"); return 2; }

    swork_t w;
    memset(&w, 0, sizeof(w));
    w.nv = m->nv;
    w.nf = m->nf;
    w.nfalive = m->nf;

    w.p = (float(*)[3])malloc((size_t)w.nv * 3 * sizeof(float));
    w.q = (float(*)[10])xcalloc((size_t)w.nv, 10 * sizeof(float));
    w.f = (int(*)[3])malloc((size_t)w.nf * 3 * sizeof(int));
    w.fdead = (char *)xcalloc((size_t)w.nf, 1);
    w.vdead = (char *)xcalloc((size_t)w.nv, 1);
    w.vf = (iarr_t *)xcalloc((size_t)w.nv, sizeof(iarr_t));
    w.fstamp = (int *)xcalloc((size_t)w.nf, sizeof(int));
    if (!w.p || !w.q || !w.f || !w.fdead || !w.vdead || !w.vf ||
        !w.fstamp) {
        work_free(&w);
        set_err(err_msg, "out of memory");
        return 2;
    }
    memcpy(w.p, m->v, (size_t)w.nv * 3 * sizeof(float));
    memcpy(w.f, m->f, (size_t)w.nf * 3 * sizeof(int));

    /* 顶点-面邻接 */
    for (int fid = 0; fid < w.nf; fid++) {
        for (int k = 0; k < 3; k++) {
            int vi = w.f[fid][k];
            if (ia_push(&w.vf[vi], fid) != 0) {
                work_free(&w); set_err(err_msg, "out of memory"); return 2;
            }
        }
    }

    /* 每个面的平面误差矩阵累加到顶点 */
    for (int fid = 0; fid < w.nf; fid++) add_face_quadric(&w, fid);

    /* 建边并入堆 */
    for (int fid = 0; fid < w.nf; fid++) {
        for (int k = 0; k < 3; k++) {
            int x = w.f[fid][k], y = w.f[fid][(k + 1) % 3];
            int lo = x < y ? x : y, hi = x < y ? y : x;
            if (lo == hi) continue;
            int eid = get_edge(&w, lo, hi);
            if (eid < 0 || edge_attach_face(&w, eid, fid) != 0) {
                work_free(&w); set_err(err_msg, "out of memory"); return 2;
            }
        }
    }
    for (int eid = 0; eid < w.ne; eid++) {
        if (edge_push_fresh(&w, eid) != 0) {
            work_free(&w); set_err(err_msg, "out of memory"); return 2;
        }
    }

    int faces0 = w.nf;
    int target = (int)lround(ratio * faces0);
    if (target < 1) target = 1;
    if (target >= faces0) { /* 无需简化 */
        work_free(&w);
        return 0;
    }

    int canceled = 0;
    int collapses = 0;
    int last_pct = -1;
    if (cb) { int p = 0; if (cb(p, user_data) != 0) canceled = 1; last_pct = 0; }

    while (!canceled && w.nfalive > target && w.hlen > 0) {
        int eid = heap_pop(&w);
        edge_t *e = &w.edges[eid];
        if (e->dead || w.vdead[e->a] || w.vdead[e->b]) continue;
        if (contract(&w, eid) != 0) {
            /* 内存失败：提交当前一致状态并返回错误 */
            work_commit(&w, m);
            work_free(&w);
            set_err(err_msg, "out of memory during simplification");
            return 2;
        }
        collapses++;

        if (cb && (collapses & 0xFF) == 0) {
            int pct = (int)(100.0 * (faces0 - w.nfalive) /
                            (faces0 - target));
            if (pct < 0) pct = 0;
            if (pct > 99) pct = 99;
            if (pct != last_pct) {
                last_pct = pct;
                if (cb(pct, user_data) != 0) canceled = 1;
            }
        }
    }

    if (work_commit(&w, m) != 0) {
        work_free(&w);
        set_err(err_msg, "out of memory while writing result");
        return 2;
    }
    work_free(&w);

    if (cb && !canceled) cb(100, user_data);
    return canceled ? 1 : 0;
}

/* ------------------------------ 其它导出 ------------------------------ */

int meshlib_vertex_count(const meshlib_mesh *m) { return m ? m->nv : 0; }
int meshlib_face_count(const meshlib_mesh *m) { return m ? m->nf : 0; }
const char *meshlib_version(void) { return MESHLIB_VERSION; }

void meshlib_free(meshlib_mesh *m) {
    if (!m) return;
    free(m->v);
    free(m->f);
    free(m);
}

void *meshlib_calloc(size_t n, size_t size) {
    return calloc(n, size);
}

void meshlib_cfree(void *p) {
    free(p);
}

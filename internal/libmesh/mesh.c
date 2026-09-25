#define LIBMESH_BUILD
#include "mesh.h"

#include <math.h>
#if defined(__unix__) || defined(__APPLE__)
#include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* small math helpers                                                  */
/* ------------------------------------------------------------------ */

typedef struct { double x, y, z; } v3;

static v3 v3sub(v3 a, v3 b) {
    v3 r = {a.x - b.x, a.y - b.y, a.z - b.z};
    return r;
}
static v3 v3cross(v3 a, v3 b) {
    v3 r = {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
    return r;
}
static void v3normalize(v3 *a) {
    double l = sqrt(a->x * a->x + a->y * a->y + a->z * a->z);
    if (l > 1e-12) {
        a->x /= l;
        a->y /= l;
        a->z /= l;
    }
}
static double v3dist(v3 a, v3 b) {
    double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

/* Q(a,b) = a^T Q b for 4-vectors (x,y,z,1), symmetric matrix. */
static double qv(const double q[16], const double a[4], const double b[4]) {
    double s = 0.0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            s += a[i] * q[i * 4 + j] * b[j];
    return s;
}

/* ------------------------------------------------------------------ */
/* mesh                                                                */
/* ------------------------------------------------------------------ */

struct lm_mesh {
    v3 *pos;        /* one slot per vertex ever created (deleted stay) */
    int *alive_v;
    int vcap, vcount;

    int *face;      /* 3 vertex ids per face */
    int *alive_f;
    int fcap, fcount;
};

lm_mesh *lm_mesh_new(void) {
    return (lm_mesh *)calloc(1, sizeof(lm_mesh));
}

void lm_mesh_free(lm_mesh *m) {
    if (!m)
        return;
    free(m->pos);
    free(m->alive_v);
    free(m->face);
    free(m->alive_f);
    free(m);
}

int lm_mesh_set_data(lm_mesh *m, const float *vertices, int vert_count,
                     const int *indices, int face_count) {
    if (!m || !vertices || !indices || vert_count <= 0 || face_count <= 0)
        return LM_ERR_INVALID;

    free(m->pos);
    free(m->alive_v);
    free(m->face);
    free(m->alive_f);
    m->pos = (v3 *)malloc(sizeof(v3) * (size_t)vert_count);
    m->alive_v = (int *)malloc(sizeof(int) * (size_t)vert_count);
    m->face = (int *)malloc(sizeof(int) * 3 * (size_t)face_count);
    m->alive_f = (int *)malloc(sizeof(int) * (size_t)face_count);
    if (!m->pos || !m->alive_v || !m->face || !m->alive_f) {
        lm_mesh_free(m);
        return LM_ERR_MEMORY;
    }

    for (int i = 0; i < vert_count; ++i) {
        m->pos[i].x = vertices[i * 3];
        m->pos[i].y = vertices[i * 3 + 1];
        m->pos[i].z = vertices[i * 3 + 2];
        m->alive_v[i] = 1;
    }
    for (int i = 0; i < face_count * 3; ++i) {
        int id = indices[i];
        if (id < 0 || id >= vert_count) {
            lm_mesh_free(m);
            return LM_ERR_INVALID;
        }
        m->face[i] = id;
    }
    for (int i = 0; i < face_count; ++i)
        m->alive_f[i] = 1;
    m->vcount = m->vcap = vert_count;
    m->fcount = m->fcap = face_count;
    return LM_OK;
}

int lm_mesh_vertex_count(const lm_mesh *m) { return m ? m->vcount : 0; }
int lm_mesh_face_count(const lm_mesh *m) { return m ? m->fcount : 0; }

static int count_alive_i(const int *flags, int n) {
    int s = 0;
    for (int i = 0; i < n; ++i)
        s += flags[i] != 0;
    return s;
}
int lm_mesh_alive_vertex_count(const lm_mesh *m) {
    return m ? count_alive_i(m->alive_v, m->vcount) : 0;
}
int lm_mesh_alive_face_count(const lm_mesh *m) {
    return m ? count_alive_i(m->alive_f, m->fcount) : 0;
}

/* ------------------------------------------------------------------ */
/* tiny OBJ reader/writer                                              */
/* ------------------------------------------------------------------ */

lm_mesh *lm_mesh_load_obj(const char *path, int *err) {
    if (err)
        *err = LM_OK;
    if (!path) {
        if (err)
            *err = LM_ERR_INVALID;
        return NULL;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (err)
            *err = LM_ERR_OPEN_FILE;
        return NULL;
    }

    int vcap = 256, fcap = 256;
    v3 *pos = (v3 *)malloc(sizeof(v3) * (size_t)vcap);
    int *face = (int *)malloc(sizeof(int) * 3 * (size_t)fcap);
    int vn = 0, fn = 0;
    if (!pos || !face) {
        free(pos);
        free(face);
        fclose(fp);
        if (err)
            *err = LM_ERR_MEMORY;
        return NULL;
    }

    char line[1024];
    int bad = 0;
    while (fgets(line, (int)sizeof(line), fp)) {
        if (line[0] == 'v' && line[1] == ' ') {
            double x, y, z;
            if (sscanf(line + 2, "%lf %lf %lf", &x, &y, &z) != 3) {
                bad = 1;
                break;
            }
            if (vn == vcap) {
                int nc = vcap * 2;
                v3 *np = (v3 *)realloc(pos, sizeof(v3) * (size_t)nc);
                if (!np) {
                    free(pos);
                    free(face);
                    fclose(fp);
                    if (err)
                        *err = LM_ERR_MEMORY;
                    return NULL;
                }
                pos = np;
                vcap = nc;
            }
            pos[vn].x = x;
            pos[vn].y = y;
            pos[vn].z = z;
            ++vn;
        } else if (line[0] == 'f' && line[1] == ' ') {
            int idx[16];
            int n = 0;
            const char *p = line + 2;
            /* whitespace-separated vertices; support v, v/vt, v//vn,
             * v/vt/vn and negative relative indices. */
            while (*p && n < 16) {
                while (*p == ' ' || *p == '\t')
                    ++p;
                if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#')
                    break;
                char *end = NULL;
                long id = strtol(p, &end, 10);
                if (end == p) {
                    bad = 1;
                    break;
                }
                if (id < 0)
                    id = vn + id + 1;
                idx[n++] = (int)(id - 1);
                p = end;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
                       *p != '\r')
                    ++p;
            }
            if (bad)
                break;
            if (n < 3)
                continue; /* be lenient with malformed records */
            int needed = fn + (n - 2);
            if (needed > fcap) {
                int nc = fcap;
                while (nc < needed)
                    nc *= 2;
                int *nf = (int *)realloc(face, sizeof(int) * 3 * (size_t)nc);
                if (!nf) {
                    free(pos);
                    free(face);
                    fclose(fp);
                    if (err)
                        *err = LM_ERR_MEMORY;
                    return NULL;
                }
                face = nf;
                fcap = nc;
            }
            for (int i = 1; i < n - 1; ++i) { /* fan triangulation */
                face[fn * 3] = idx[0];
                face[fn * 3 + 1] = idx[i];
                face[fn * 3 + 2] = idx[i + 1];
                ++fn;
            }
        }
        /* vt/vn/mtllib/usemtl/comments are ignored */
    }
    fclose(fp);

    if (bad || vn == 0 || fn == 0) {
        free(pos);
        free(face);
        if (err)
            *err = LM_ERR_BAD_FORMAT;
        return NULL;
    }

    lm_mesh *m = lm_mesh_new();
    if (!m) {
        free(pos);
        free(face);
        if (err)
            *err = LM_ERR_MEMORY;
        return NULL;
    }
    m->pos = pos;
    m->vcount = m->vcap = vn;
    m->face = face;
    m->fcount = m->fcap = fn;
    m->alive_v = (int *)malloc(sizeof(int) * (size_t)vn);
    m->alive_f = (int *)malloc(sizeof(int) * (size_t)fn);
    if (!m->alive_v || !m->alive_f) {
        if (err)
            *err = LM_ERR_MEMORY;
        lm_mesh_free(m);
        return NULL;
    }
    for (int i = 0; i < vn; ++i)
        m->alive_v[i] = 1;
    for (int i = 0; i < fn; ++i)
        m->alive_f[i] = 1;
    return m;
}

int lm_mesh_write_obj(const lm_mesh *m, const char *path) {
    if (!m || !path)
        return LM_ERR_INVALID;
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return LM_ERR_OPEN_FILE;

    int *remap = (int *)malloc(sizeof(int) * (size_t)m->vcount);
    if (!remap) {
        fclose(fp);
        return LM_ERR_MEMORY;
    }
    int next = 1; /* OBJ indices start at 1 */
    for (int i = 0; i < m->vcount; ++i) {
        if (m->alive_v[i]) {
            fprintf(fp, "v %.9g %.9g %.9g\n", m->pos[i].x, m->pos[i].y,
                    m->pos[i].z);
            remap[i] = next++;
        }
    }
    for (int f = 0; f < m->fcount; ++f) {
        if (!m->alive_f[f])
            continue;
        fprintf(fp, "f %d %d %d\n", remap[m->face[f * 3]],
                remap[m->face[f * 3 + 1]], remap[m->face[f * 3 + 2]]);
    }
    free(remap);
    fclose(fp);
    return LM_OK;
}

int lm_mesh_get_data(const lm_mesh *m, float *vertices, int vert_cap,
                     int *indices, int face_cap) {
    if (!m)
        return LM_ERR_INVALID;
    int av = 0, af = 0;
    for (int i = 0; i < m->vcount; ++i)
        av += m->alive_v[i];
    for (int i = 0; i < m->fcount; ++i)
        af += m->alive_f[i];
    if (vert_cap < av || face_cap < af)
        return LM_ERR_INVALID;

    int *remap = (int *)malloc(sizeof(int) * (size_t)(m->vcount + 1));
    if (!remap)
        return LM_ERR_MEMORY;
    int vi = 0;
    for (int i = 0; i < m->vcount; ++i) {
        if (m->alive_v[i]) {
            vertices[vi * 3] = (float)m->pos[i].x;
            vertices[vi * 3 + 1] = (float)m->pos[i].y;
            vertices[vi * 3 + 2] = (float)m->pos[i].z;
            remap[i] = vi;
            ++vi;
        }
    }
    int fi = 0;
    for (int f = 0; f < m->fcount; ++f) {
        if (!m->alive_f[f])
            continue;
        indices[fi * 3] = remap[m->face[f * 3]];
        indices[fi * 3 + 1] = remap[m->face[f * 3 + 1]];
        indices[fi * 3 + 2] = remap[m->face[f * 3 + 2]];
        ++fi;
    }
    free(remap);
    return LM_OK;
}

/* ------------------------------------------------------------------ */
/* QEM simplification                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int a, b;       /* vertex ids (a < b) */
    double cost;
    int version;    /* bumped whenever the edge is re-priced */
    int heap_slot;  /* -1 when not in heap */
} edge;

typedef struct { int edge, ver; } hentry;

typedef struct {
    int *items;
    int n, cap;
} ilist;

struct lm_task {
    lm_mesh *m;

    double *Q;       /* 16 doubles per vertex slot */
    edge *edges;
    int en, ecap;

    int *emap;       /* open addressing: slot -> edge id + 1 */
    int emap_cap;

    ilist *incident; /* incident[v] = edges touching vertex v */
    ilist *vfaces;   /* vfaces[v]  = alive faces incident to v */

    hentry *heap;
    int hn, hcap;

    int alive_faces;
    int target_faces;

    lm_progress_fn progress;
    void *progress_arg;
    double last_cb_time;
    int cb_count;
    volatile int cancel_flag;

    int cancelled;
    int finished;
};

static void ilist_push(ilist *l, int val) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        int *ni = (int *)realloc(l->items, sizeof(int) * (size_t)nc);
        if (!ni)
            return; /* missing adjacency links only freeze some edges */
        l->items = ni;
        l->cap = nc;
    }
    l->items[l->n++] = val;
}

static unsigned key_hash(int a, int b) {
    uint64_t k = ((uint64_t)(uint32_t)a << 32) | (uint32_t)b;
    uint32_t h = (uint32_t)(k ^ (k >> 32));
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return (unsigned)h;
}

static int emap_find(const lm_task *t, int a, int b) {
    unsigned h = key_hash(a, b) % (unsigned)t->emap_cap;
    for (;;) {
        int slot = t->emap[h];
        if (slot == 0)
            return -1;
        const edge *e = &t->edges[slot - 1];
        if (e->a == a && e->b == b)
            return slot - 1;
        h = (h + 1) % (unsigned)t->emap_cap;
    }
}

static void emap_insert(lm_task *t, int id) {
    const edge *e = &t->edges[id];
    unsigned h = key_hash(e->a, e->b) % (unsigned)t->emap_cap;
    while (t->emap[h] != 0)
        h = (h + 1) % (unsigned)t->emap_cap;
    t->emap[h] = id + 1;
}

static int emap_grow(lm_task *t) {
    int ncap = t->emap_cap * 2;
    int *nm = (int *)calloc((size_t)ncap, sizeof(int));
    if (!nm)
        return LM_ERR_MEMORY;
    int *om = t->emap;
    int ocap = t->emap_cap;
    t->emap = nm;
    t->emap_cap = ncap;
    for (int i = 0; i < ocap; ++i) {
        if (!om[i])
            continue;
        emap_insert(t, om[i] - 1);
    }
    free(om);
    return LM_OK;
}

static void face_quadric(const lm_mesh *m, int f, double q[16]) {
    int ia = m->face[f * 3], ib = m->face[f * 3 + 1],
        ic = m->face[f * 3 + 2];
    v3 a = m->pos[ia], b = m->pos[ib], c = m->pos[ic];
    v3 n = v3cross(v3sub(b, a), v3sub(c, a));
    v3normalize(&n);
    double d = -(n.x * a.x + n.y * a.y + n.z * a.z);
    double p[4] = {n.x, n.y, n.z, d};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            q[i * 4 + j] = p[i] * p[j];
}

static void add_quadric(double *dst, const double q[16]) {
    for (int i = 0; i < 16; ++i)
        dst[i] += q[i];
}

/* Point minimising x^T Q x over R3 (w = 1). When the 3x3 normal-equation
 * block is singular, fall back to the edge midpoint. */
static v3 optimal_point(const double Q[16], v3 a, v3 b, double *out_cost) {
    double M[9] = {
        Q[0], Q[1], Q[2],
        Q[4], Q[5], Q[6],
        Q[8], Q[9], Q[10],
    };
    double rhs[3] = {-Q[3], -Q[7], -Q[11]};
    int singular = 0;

    for (int col = 0; col < 3; ++col) {
        int piv = col;
        double best = fabs(M[col * 3 + col]);
        for (int r = col + 1; r < 3; ++r) {
            double v = fabs(M[r * 3 + col]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (best < 1e-12) {
            singular = 1;
            break;
        }
        if (piv != col) {
            for (int j = 0; j < 3; ++j) {
                double tmp = M[col * 3 + j];
                M[col * 3 + j] = M[piv * 3 + j];
                M[piv * 3 + j] = tmp;
            }
            double tmp = rhs[col];
            rhs[col] = rhs[piv];
            rhs[piv] = tmp;
        }
        double pivv = M[col * 3 + col];
        for (int r = col + 1; r < 3; ++r) {
            double factor = M[r * 3 + col] / pivv;
            if (factor == 0.0)
                continue;
            M[r * 3 + col] = 0.0;
            for (int j = col + 1; j < 3; ++j)
                M[r * 3 + j] -= factor * M[col * 3 + j];
            rhs[r] -= factor * rhs[col];
        }
    }

    v3 x;
    if (!singular) {
        double sol[3];
        for (int i = 2; i >= 0; --i) {
            double s = rhs[i];
            for (int j = i + 1; j < 3; ++j)
                s -= M[i * 3 + j] * sol[j];
            sol[i] = s / M[i * 3 + i];
        }
        x.x = sol[0];
        x.y = sol[1];
        x.z = sol[2];
        v3 mid = {(a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5};
        if (v3dist(x, mid) > 1e3 * (v3dist(a, b) + 1.0))
            x = mid;
    } else {
        x.x = (a.x + b.x) * 0.5;
        x.y = (a.y + b.y) * 0.5;
        x.z = (a.z + b.z) * 0.5;
    }

    double xv[4] = {x.x, x.y, x.z, 1.0};
    double c = qv(Q, xv, xv);
    *out_cost = c < 0.0 ? 0.0 : c;
    return x;
}

/* ---- min-heap with lazy version invalidation ------------------------- */

static int heap_cmp(const lm_task *t, int i, int j) {
    return t->edges[t->heap[i].edge].cost <
           t->edges[t->heap[j].edge].cost;
}

static void heap_swap(lm_task *t, int i, int j) {
    hentry hi = t->heap[i], hj = t->heap[j];
    t->heap[i] = hj;
    t->heap[j] = hi;
    t->edges[hi.edge].heap_slot = i;
    t->edges[hj.edge].heap_slot = j;
}

static void heap_up(lm_task *t, int i) {
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!heap_cmp(t, i, p))
            break;
        heap_swap(t, p, i);
        i = p;
    }
}

static void heap_down(lm_task *t, int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, best = i;
        if (l < t->hn && heap_cmp(t, l, best))
            best = l;
        if (r < t->hn && heap_cmp(t, r, best))
            best = r;
        if (best == i)
            break;
        heap_swap(t, best, i);
        i = best;
    }
}

static int heap_push(lm_task *t, int eid) {
    if (t->hn == t->hcap) {
        int nc = t->hcap ? t->hcap * 2 : 1024;
        hentry *nh = (hentry *)realloc(t->heap, sizeof(hentry) * (size_t)nc);
        if (!nh)
            return LM_ERR_MEMORY;
        t->heap = nh;
        t->hcap = nc;
    }
    edge *e = &t->edges[eid];
    int i = t->hn++;
    t->heap[i].edge = eid;
    t->heap[i].ver = e->version;
    e->heap_slot = i;
    heap_up(t, i);
    return LM_OK;
}

/* Pop a current edge with two alive endpoints, or -1 when drained. */
static int heap_pop(lm_task *t) {
    while (t->hn > 0) {
        hentry top = t->heap[0];
        t->heap[0] = t->heap[--t->hn];
        if (t->hn > 0) {
            t->edges[t->heap[0].edge].heap_slot = 0;
            heap_down(t, 0);
        }
        edge *e = &t->edges[top.edge];
        e->heap_slot = -1;
        if (top.ver != e->version)
            continue;
        if (!t->m->alive_v[e->a] || !t->m->alive_v[e->b])
            continue;
        return top.edge;
    }
    return -1;
}

static double edge_price(const lm_task *t, int eid) {
    const edge *e = &t->edges[eid];
    double Q[16];
    memcpy(Q, t->Q + e->a * 16, sizeof(Q));
    add_quadric(Q, t->Q + e->b * 16);
    double cost;
    (void)optimal_point(Q, t->m->pos[e->a], t->m->pos[e->b], &cost);
    return cost;
}

/* Re-price an existing edge and queue a fresh heap entry (old entry is
 * invalidated through the version bump). */
static void edge_reprice(lm_task *t, int eid) {
    edge *e = &t->edges[eid];
    if (!t->m->alive_v[e->a] || !t->m->alive_v[e->b])
        return;
    e->cost = edge_price(t, eid);
    e->version++;
    e->heap_slot = -1;
    heap_push(t, eid);
}

/* Find or create edge (a,b). Prices and queues new edges. */
static int get_edge(lm_task *t, int a, int b) {
    if (a == b)
        return -1;
    if (a > b) {
        int tmp = a;
        a = b;
        b = tmp;
    }
    int existing = emap_find(t, a, b);
    if (existing >= 0)
        return existing;

    if (t->en + 1 > (int)(t->emap_cap * 0.6)) {
        if (emap_grow(t) != LM_OK)
            return -1;
    }
    if (t->en == t->ecap) {
        int nc = t->ecap ? t->ecap * 2 : 256;
        edge *ne = (edge *)realloc(t->edges, sizeof(edge) * (size_t)nc);
        if (!ne)
            return -1;
        t->edges = ne;
        t->ecap = nc;
    }
    int eid = t->en++;
    edge *e = &t->edges[eid];
    e->a = a;
    e->b = b;
    e->cost = 0.0;
    e->version = 0;
    e->heap_slot = -1;
    emap_insert(t, eid);
    ilist_push(&t->incident[a], eid);
    ilist_push(&t->incident[b], eid);
    e->cost = edge_price(t, eid);
    if (heap_push(t, eid) != LM_OK)
        return -1;
    return eid;
}

lm_task *lm_task_new(lm_mesh *mesh, float ratio, int *err) {
    if (err)
        *err = LM_OK;
    if (!mesh) {
        if (err)
            *err = LM_ERR_INVALID;
        return NULL;
    }
    if (!(ratio > 0.0f) || ratio > 1.0f) { /* rejects NaN too */
        if (err)
            *err = LM_ERR_RATIO;
        return NULL;
    }
    if (mesh->vcount <= 0 || mesh->fcount <= 0) {
        if (err)
            *err = LM_ERR_NOT_READY;
        return NULL;
    }

    /* The mesh may already have been partially simplified by a previous
     * (possibly cancelled) task: derive everything from ALIVE geometry. */
    int alive_f = 0, alive_v = 0;
    for (int i = 0; i < mesh->fcount; ++i)
        alive_f += mesh->alive_f[i];
    for (int i = 0; i < mesh->vcount; ++i)
        alive_v += mesh->alive_v[i];
    if (alive_f == 0 || alive_v == 0) {
        if (err)
            *err = LM_ERR_NOT_READY;
        return NULL;
    }

    lm_task *t = (lm_task *)calloc(1, sizeof(lm_task));
    if (!t) {
        if (err)
            *err = LM_ERR_MEMORY;
        return NULL;
    }
    t->m = mesh;
    t->target_faces = (int)lround(ratio * (double)alive_f);
    if (t->target_faces < 1)
        t->target_faces = 1;
    if (t->target_faces > alive_f)
        t->target_faces = alive_f;
    t->alive_faces = alive_f;

    int n = mesh->vcount;
    t->Q = (double *)calloc((size_t)n * 16, sizeof(double));
    t->incident = (ilist *)calloc((size_t)n, sizeof(ilist));
    t->vfaces = (ilist *)calloc((size_t)n, sizeof(ilist));
    t->emap_cap = 8;
    while (t->emap_cap < alive_f * 6 + 16)
        t->emap_cap *= 2;
    t->emap = (int *)calloc((size_t)t->emap_cap, sizeof(int));
    if (!t->Q || !t->incident || !t->vfaces || !t->emap) {
        if (err)
            *err = LM_ERR_MEMORY;
        lm_task_free(t);
        return NULL;
    }

    for (int f = 0; f < mesh->fcount; ++f) {
        if (!mesh->alive_f[f])
            continue;
        int a = mesh->face[f * 3], b = mesh->face[f * 3 + 1],
            c = mesh->face[f * 3 + 2];
        /* A partially simplified mesh should never contain an alive face
         * referencing a dead vertex; guard regardless. */
        if (!mesh->alive_v[a] || !mesh->alive_v[b] || !mesh->alive_v[c])
            continue;
        double q[16];
        face_quadric(mesh, f, q);
        add_quadric(t->Q + a * 16, q);
        add_quadric(t->Q + b * 16, q);
        add_quadric(t->Q + c * 16, q);

        ilist_push(&t->vfaces[a], f);
        ilist_push(&t->vfaces[b], f);
        ilist_push(&t->vfaces[c], f);

        if (get_edge(t, a, b) < 0 || get_edge(t, a, c) < 0 ||
            get_edge(t, b, c) < 0) {
            if (err)
                *err = LM_ERR_MEMORY;
            lm_task_free(t);
            return NULL;
        }
    }
    return t;
}

void lm_task_set_progress_cb(lm_task *t, lm_progress_fn fn, void *arg) {
    if (t) {
        t->progress = fn;
        t->progress_arg = arg;
    }
}

void lm_task_request_cancel(lm_task *t) {
    if (t) {
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&t->cancel_flag, 1, __ATOMIC_RELEASE);
#else
        t->cancel_flag = 1; /* volatile fallback */
#endif
    }
}

/* Hoppe link condition: every alive common neighbour of u and v (through
 * edges) must be bound to both by an alive triangle (u,v,w). */
static int collapse_legal(lm_task *t, int u, int v) {
    lm_mesh *m = t->m;

    int nn = 0, nncap = 0;
    int *neigh = NULL;

    ilist *lu = &t->incident[u];
    for (int i = 0; i < lu->n; ++i) {
        edge *e = &t->edges[lu->items[i]];
        int w = e->a == u ? e->b : e->a;
        if (!m->alive_v[w] || w == v)
            continue;
        int seen = 0;
        for (int j = 0; j < nn; ++j)
            if (neigh[j] == w) {
                seen = 1;
                break;
            }
        if (!seen) {
            if (nn == nncap) {
                int nc = nncap ? nncap * 2 : 16;
                int *np = (int *)realloc(neigh, sizeof(int) * (size_t)nc);
                if (!np) {
                    free(neigh);
                    return 1; /* OOM: stay permissive */
                }
                neigh = np;
                nncap = nc;
            }
            neigh[nn++] = w;
        }
    }

    int legal = 1;
    ilist *lv = &t->incident[v];
    for (int i = 0; i < lv->n && legal; ++i) {
        edge *e = &t->edges[lv->items[i]];
        int w = e->a == v ? e->b : e->a;
        if (!m->alive_v[w] || w == u)
            continue;
        int common = 0;
        for (int j = 0; j < nn; ++j)
            if (neigh[j] == w) {
                common = 1;
                break;
            }
        if (!common)
            continue;

        int in_face = 0;
        ilist *lf = &t->vfaces[u];
        for (int j = 0; j < lf->n; ++j) {
            int f = lf->items[j];
            if (!m->alive_f[f])
                continue;
            int a = m->face[f * 3], b = m->face[f * 3 + 1],
                c = m->face[f * 3 + 2];
            if ((a == v || b == v || c == v) &&
                (a == w || b == w || c == w)) {
                in_face = 1;
                break;
            }
        }
        if (!in_face)
            legal = 0;
    }
    free(neigh);
    return legal;
}

/* monotonic seconds; 0 on platforms without a known clock */
static double now_seconds(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
    return 0.0;
}

/* Invoke the progress callback, throttled to ~60 Hz. The first and last
 * emissions (force=1) always fire; cancellation aborts the run. */
#define LM_EMIT_PROGRESS(force)                                            \
    do {                                                                   \
        if (!t->progress)                                                  \
            break;                                                         \
        double now = now_seconds();                                        \
        int pct = span > 0                                                 \
            ? (int)(100.0 * (double)(initial - t->alive_faces) /           \
                    (double)span)                                          \
            : 100;                                                         \
        if (pct > 100)                                                     \
            pct = 100;                                                     \
        int time_ok = (now == 0.0) || (now - t->last_cb_time) >= 0.016;    \
        if (!(force) && !time_ok && pct == last_pct)                       \
            break;                                                         \
        t->last_cb_time = now;                                             \
        last_pct = pct;                                                    \
        if (t->progress(t->progress_arg, t->alive_faces, initial) != 0) {  \
            t->cancelled = 1;                                              \
            return LM_ERR_CANCELLED;                                       \
        }                                                                  \
    } while (0)

int lm_task_run(lm_task *t) {
    if (!t)
        return LM_ERR_INVALID;
    lm_mesh *m = t->m;
    int initial = t->alive_faces;
    int span = initial - t->target_faces;
    int last_pct = -1;
    t->last_cb_time = now_seconds();

    LM_EMIT_PROGRESS(1);

    while (t->alive_faces > t->target_faces) {
        /* Cheap cancel check independent of callback throttling, so a
         * context cancel lands promptly even between progress emissions. */
        int cf;
#if defined(__GNUC__) || defined(__clang__)
        cf = __atomic_load_n(&t->cancel_flag, __ATOMIC_ACQUIRE);
#else
        cf = t->cancel_flag;
#endif
        if (cf) {
            t->cancelled = 1;
            break;
        }
        int eid = heap_pop(t);
        if (eid < 0)
            break; /* no more collapsible edges */

        int u = t->edges[eid].a, v = t->edges[eid].b;
        if (!collapse_legal(t, u, v))
            continue;

        double Q[16];
        memcpy(Q, t->Q + u * 16, sizeof(Q));
        add_quadric(Q, t->Q + v * 16);
        double cost;
        v3 newp = optimal_point(Q, m->pos[u], m->pos[v], &cost);
        (void)cost;

        /* Redirect faces incident to v to u; delete degenerates. */
        ilist *fv = &t->vfaces[v];
        int wri = 0;
        for (int i = 0; i < fv->n; ++i) {
            int f = fv->items[i];
            if (!m->alive_f[f])
                continue;
            int *fp = &m->face[f * 3];
            for (int k = 0; k < 3; ++k)
                if (fp[k] == v)
                    fp[k] = u;
            if (fp[0] == fp[1] || fp[1] == fp[2] || fp[0] == fp[2]) {
                m->alive_f[f] = 0;
                --t->alive_faces;
            } else {
                ilist_push(&t->vfaces[u], f);
                fv->items[wri++] = f;
            }
        }
        fv->n = wri;

        m->alive_v[v] = 0;
        m->pos[u] = newp;
        memcpy(t->Q + u * 16, Q, sizeof(Q));

        /* Re-price surviving edges incident to u. */
        ilist *lu = &t->incident[u];
        for (int i = 0; i < lu->n; ++i) {
            edge *e = &t->edges[lu->items[i]];
            int w = e->a == u ? e->b : e->a;
            if (w == v || !m->alive_v[w])
                continue;
            edge_reprice(t, lu->items[i]);
        }

        /* Transfer v's surviving links to u (creates new edges). */
        ilist *lv = &t->incident[v];
        for (int i = 0; i < lv->n; ++i) {
            edge *e = &t->edges[lv->items[i]];
            int w = e->a == v ? e->b : e->a;
            if (!m->alive_v[w])
                continue;
            get_edge(t, u, w);
        }

        LM_EMIT_PROGRESS(0);
    }

    if (t->cancelled)
        return LM_ERR_CANCELLED;

    t->finished = 1;
    LM_EMIT_PROGRESS(1);
    #undef LM_EMIT_PROGRESS
    return LM_OK;
}

void lm_task_free(lm_task *t) {
    if (!t)
        return;
    free(t->Q);
    free(t->edges);
    free(t->emap);
    free(t->heap);
    if (t->incident && t->m) {
        for (int i = 0; i < t->m->vcount; ++i)
            free(t->incident[i].items);
        free(t->incident);
    }
    if (t->vfaces && t->m) {
        for (int i = 0; i < t->m->vcount; ++i)
            free(t->vfaces[i].items);
        free(t->vfaces);
    }
    free(t);
}

const char *lm_version(void) { return "libmesh 1.0 (QEM)"; }

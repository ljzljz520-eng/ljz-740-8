/* libmeshsimp - vertex-clustering mesh decimation with progress + cancel. */
#define _POSIX_C_SOURCE 200809L  /* strtok_r */
#include "meshsimp.h"

#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ms_context {
    float    *verts;      /* 3 floats per vertex */
    size_t    nv;         /* number of floats (3 * vertex count) */
    uint32_t *faces;      /* 3 indices per triangle */
    size_t    nf;         /* number of indices (3 * triangle count) */

    float    *out_verts;
    size_t    out_nv;
    uint32_t *out_faces;
    size_t    out_nf;

    float ratio;

    atomic_int cancel;    /* 1 => cancellation requested */
};

/* ------------------------------------------------------------------ */

MS_API const char *ms_strerror(int code) {
    switch (code) {
    case MS_OK:        return "ok";
    case MS_CANCELLED: return "operation cancelled";
    case MS_ERR_ARG:   return "invalid argument";
    case MS_ERR_NOMEM: return "out of memory";
    case MS_ERR_IO:    return "i/o error";
    case MS_ERR_PARSE: return "parse error";
    case MS_ERR_STATE: return "invalid state";
    default:           return "unknown error";
    }
}

MS_API ms_context *ms_create(void) {
    ms_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->ratio = 0.5f;
    atomic_init(&ctx->cancel, 0);
    return ctx;
}

static void clear_output(ms_context *ctx) {
    free(ctx->out_verts);  ctx->out_verts = NULL;  ctx->out_nv = 0;
    free(ctx->out_faces);  ctx->out_faces = NULL;  ctx->out_nf = 0;
}

MS_API void ms_destroy(ms_context *ctx) {
    if (!ctx) return;
    free(ctx->verts);
    free(ctx->faces);
    clear_output(ctx);
    free(ctx);
}

MS_API void ms_set_ratio(ms_context *ctx, float ratio) {
    if (!ctx) return;
    if (!(ratio > 0.0f)) ratio = 0.5f;  /* also catches NaN */
    if (ratio > 1.0f) ratio = 1.0f;
    ctx->ratio = ratio;
}

MS_API void ms_cancel(ms_context *ctx) {
    if (ctx) atomic_store_explicit(&ctx->cancel, 1, memory_order_relaxed);
}

static int cancelled(const ms_context *ctx) {
    return atomic_load_explicit(&ctx->cancel, memory_order_relaxed) != 0;
}

/* ------------------------------------------------------------------ */
/* OBJ loading                                                         */

static int grow(void **arr, size_t *cap, size_t need, size_t elemsz) {
    if (need <= *cap) return 0;
    size_t ncap = *cap ? *cap : 1024;
    while (ncap < need) ncap *= 2;
    void *p = realloc(*arr, ncap * elemsz);
    if (!p) return -1;
    *arr = p;
    *cap = ncap;
    return 0;
}

/* Parses one OBJ face token: "v", "v/vt", "v//vn", "v/vt/vn".
 * Negative indices are relative to the current vertex count. */
static int parse_index(const char *tok, size_t nverts, uint32_t *out) {
    char *end = NULL;
    long idx = strtol(tok, &end, 10);
    if (end == tok || idx == 0) return -1;
    if (idx < 0) idx = (long)nverts + idx + 1;
    if (idx < 1 || (size_t)idx > nverts) return -1;
    *out = (uint32_t)(idx - 1);
    return 0;
}

MS_API int ms_load_obj(ms_context *ctx, const char *path) {
    if (!ctx || !path) return MS_ERR_ARG;

    FILE *fp = fopen(path, "rb");
    if (!fp) return MS_ERR_IO;

    float    *verts = NULL; size_t nv = 0, vcap = 0;
    uint32_t *faces = NULL; size_t nf = 0, fcap = 0;
    char line[4096];
    int rc = MS_OK;
    unsigned long lineno = 0;

    while (fgets(line, sizeof line, fp)) {
        /* ms_cancel() also aborts a load in progress. */
        if ((++lineno & 0xFFF) == 0 && cancelled(ctx)) {
            rc = MS_CANCELLED; goto fail;
        }
        if (line[0] == 'v' && (line[1] == ' ' || line[1] == '\t')) {
            float x, y, z;
            if (sscanf(line + 1, "%f %f %f", &x, &y, &z) != 3) continue;
            if (grow((void **)&verts, &vcap, nv + 3, sizeof(float))) {
                rc = MS_ERR_NOMEM; goto fail;
            }
            verts[nv++] = x; verts[nv++] = y; verts[nv++] = z;
        } else if (line[0] == 'f' && (line[1] == ' ' || line[1] == '\t')) {
            uint32_t idx[64];
            int n = 0;
            char *save = NULL;
            for (char *tok = strtok_r(line + 1, " \t\r\n", &save);
                 tok && n < 64;
                 tok = strtok_r(NULL, " \t\r\n", &save)) {
                if (parse_index(tok, nv / 3, &idx[n]) != 0) {
                    rc = MS_ERR_PARSE; goto fail;
                }
                n++;
            }
            if (n < 3) { rc = MS_ERR_PARSE; goto fail; }
            /* fan-triangulate polygons */
            for (int i = 1; i + 1 < n; i++) {
                if (grow((void **)&faces, &fcap, nf + 3, sizeof(uint32_t))) {
                    rc = MS_ERR_NOMEM; goto fail;
                }
                faces[nf++] = idx[0];
                faces[nf++] = idx[i];
                faces[nf++] = idx[i + 1];
            }
        }
    }
    if (ferror(fp)) { rc = MS_ERR_IO; goto fail; }
    fclose(fp);

    if (nv == 0 || nf == 0) { rc = MS_ERR_PARSE; goto fail; }

    free(ctx->verts);
    free(ctx->faces);
    clear_output(ctx);
    ctx->verts = verts; ctx->nv = nv;
    ctx->faces = faces; ctx->nf = nf;
    return MS_OK;

fail:
    fclose(fp);
    free(verts);
    free(faces);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Simplification (vertex clustering)                                  */

static size_t cell_of(const float *p, const float bmin[3],
                      const float inv[3], int k) {
    size_t c[3];
    for (int a = 0; a < 3; a++) {
        int i = (int)((p[a] - bmin[a]) * inv[a]);
        if (i < 0) i = 0;
        if (i > k - 1) i = k - 1;
        c[a] = (size_t)i;
    }
    return (c[0] * (size_t)k + c[1]) * (size_t)k + c[2];
}

MS_API int ms_simplify(ms_context *ctx, ms_progress_fn cb, void *user) {
    if (!ctx) return MS_ERR_ARG;
    if (ctx->nv == 0 || ctx->nf == 0) return MS_ERR_STATE;

    atomic_store_explicit(&ctx->cancel, 0, memory_order_relaxed);
    clear_output(ctx);

    const size_t nv = ctx->nv / 3;
    const size_t nf = ctx->nf / 3;
    size_t target = (size_t)((double)nf * (double)ctx->ratio);
    if (target < 1) target = 1;

    float    *acc    = NULL;   /* per-cell summed positions (3 floats) */
    uint32_t *cnt    = NULL;   /* per-cell vertex count */
    int32_t  *remap  = NULL;   /* per-cell -> output vertex index */
    uint32_t *vcell  = NULL;   /* per-vertex cell */
    float    *nverts = NULL;
    uint32_t *nfaces = NULL;
    int rc = MS_OK;

    if (target >= nf) {
        /* passthrough copy */
        nverts = malloc(ctx->nv * sizeof(float));
        nfaces = malloc(ctx->nf * sizeof(uint32_t));
        if (!nverts || !nfaces) { rc = MS_ERR_NOMEM; goto done; }
        memcpy(nverts, ctx->verts, ctx->nv * sizeof(float));
        memcpy(nfaces, ctx->faces, ctx->nf * sizeof(uint32_t));
        ctx->out_verts = nverts; ctx->out_nv = ctx->nv; nverts = NULL;
        ctx->out_faces = nfaces; ctx->out_nf = ctx->nf; nfaces = NULL;
        if (cb) cb(1.0f, user);
        return MS_OK;
    }

    /* grid resolution: cells ~= vertex budget (target/2 for closed meshes) */
    double budget = (double)target * 0.5;
    int k = (int)cbrt(budget) + 1;
    if (k < 1) k = 1;
    if (k > 64) k = 64;
    const size_t ncells = (size_t)k * (size_t)k * (size_t)k;

    acc   = calloc(ncells * 3, sizeof(float));
    cnt   = calloc(ncells, sizeof(uint32_t));
    remap = malloc(ncells * sizeof(int32_t));
    vcell = malloc(nv * sizeof(uint32_t));
    if (!acc || !cnt || !remap || !vcell) { rc = MS_ERR_NOMEM; goto done; }
    memset(remap, 0xFF, ncells * sizeof(int32_t));

    /* bounding box */
    float bmin[3] = { FLT_MAX,  FLT_MAX,  FLT_MAX};
    float bmax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t i = 0; i < nv; i++) {
        for (int a = 0; a < 3; a++) {
            float v = ctx->verts[i * 3 + a];
            if (v < bmin[a]) bmin[a] = v;
            if (v > bmax[a]) bmax[a] = v;
        }
        if ((i & 0xFFF) == 0 && cancelled(ctx)) { rc = MS_CANCELLED; goto done; }
    }
    float inv[3];
    for (int a = 0; a < 3; a++) {
        float extent = bmax[a] - bmin[a];
        inv[a] = extent > 0.0f ? (float)k / extent : 0.0f;
    }

    /* pass 1: cluster vertices into grid cells */
    for (size_t i = 0; i < nv; i++) {
        size_t c = cell_of(ctx->verts + i * 3, bmin, inv, k);
        vcell[i] = (uint32_t)c;
        acc[c * 3 + 0] += ctx->verts[i * 3 + 0];
        acc[c * 3 + 1] += ctx->verts[i * 3 + 1];
        acc[c * 3 + 2] += ctx->verts[i * 3 + 2];
        cnt[c]++;
        if ((i & 0xFFF) == 0) {
            if (cancelled(ctx)) { rc = MS_CANCELLED; goto done; }
            if (cb) cb(0.05f + 0.45f * (float)i / (float)nv, user);
        }
    }

    /* pass 2: cell representatives become output vertices */
    nverts = malloc(ncells * 3 * sizeof(float));  /* upper bound */
    if (!nverts) { rc = MS_ERR_NOMEM; goto done; }
    size_t onv = 0;
    for (size_t c = 0; c < ncells; c++) {
        if (cnt[c] == 0) continue;
        float s = 1.0f / (float)cnt[c];
        nverts[onv * 3 + 0] = acc[c * 3 + 0] * s;
        nverts[onv * 3 + 1] = acc[c * 3 + 1] * s;
        nverts[onv * 3 + 2] = acc[c * 3 + 2] * s;
        remap[c] = (int32_t)onv;
        onv++;
        if ((c & 0xFFFF) == 0 && cancelled(ctx)) { rc = MS_CANCELLED; goto done; }
    }
    if (cb) cb(0.7f, user);

    /* pass 3: rebuild triangles, dropping degenerate ones */
    nfaces = malloc(ctx->nf * sizeof(uint32_t));
    if (!nfaces) { rc = MS_ERR_NOMEM; goto done; }
    size_t onf = 0;
    for (size_t f = 0; f < nf; f++) {
        uint32_t a = (uint32_t)remap[vcell[ctx->faces[f * 3 + 0]]];
        uint32_t b = (uint32_t)remap[vcell[ctx->faces[f * 3 + 1]]];
        uint32_t c = (uint32_t)remap[vcell[ctx->faces[f * 3 + 2]]];
        if (a == b || b == c || a == c) continue;
        nfaces[onf * 3 + 0] = a;
        nfaces[onf * 3 + 1] = b;
        nfaces[onf * 3 + 2] = c;
        onf++;
        if ((f & 0xFFF) == 0) {
            if (cancelled(ctx)) { rc = MS_CANCELLED; goto done; }
            if (cb) cb(0.7f + 0.3f * (float)f / (float)nf, user);
        }
    }

    ctx->out_verts = nverts; ctx->out_nv = onv * 3; nverts = NULL;
    ctx->out_faces = nfaces; ctx->out_nf = onf * 3; nfaces = NULL;
    if (cb) cb(1.0f, user);

done:
    /* All intermediate buffers are released on every exit path,
     * including cancellation. */
    free(acc);
    free(cnt);
    free(remap);
    free(vcell);
    free(nverts);
    free(nfaces);
    if (rc == MS_CANCELLED) clear_output(ctx);  /* discard partial result */
    return rc;
}

/* ------------------------------------------------------------------ */
/* Export                                                              */

MS_API int ms_export_obj(const ms_context *ctx, const char *path) {
    if (!ctx || !path) return MS_ERR_ARG;
    if (!ctx->out_verts || !ctx->out_faces ||
        ctx->out_nv == 0 || ctx->out_nf == 0) return MS_ERR_STATE;

    FILE *fp = fopen(path, "wb");
    if (!fp) return MS_ERR_IO;

    fprintf(fp, "# simplified by libmeshsimp\n");
    for (size_t i = 0; i < ctx->out_nv; i += 3) {
        fprintf(fp, "v %.9g %.9g %.9g\n",
                ctx->out_verts[i], ctx->out_verts[i + 1], ctx->out_verts[i + 2]);
    }
    for (size_t f = 0; f < ctx->out_nf; f += 3) {
        fprintf(fp, "f %u %u %u\n",
                ctx->out_faces[f] + 1, ctx->out_faces[f + 1] + 1,
                ctx->out_faces[f + 2] + 1);
    }
    if (fclose(fp) != 0) return MS_ERR_IO;
    return MS_OK;
}

/* ------------------------------------------------------------------ */

MS_API uint64_t ms_input_vertex_count(const ms_context *ctx)  { return ctx ? ctx->nv / 3 : 0; }
MS_API uint64_t ms_input_face_count(const ms_context *ctx)    { return ctx ? ctx->nf / 3 : 0; }
MS_API uint64_t ms_output_vertex_count(const ms_context *ctx) { return ctx ? ctx->out_nv / 3 : 0; }
MS_API uint64_t ms_output_face_count(const ms_context *ctx)   { return ctx ? ctx->out_nf / 3 : 0; }

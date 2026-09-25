/*
 * libmesh: minimal 3D mesh simplification library.
 *
 * Triangle meshes are simplified with the Garland-Heckbert quadric error
 * metric (QEM): each vertex carries a quadric equal to the sum of the
 * fundamental error quadrics of its incident faces. Every edge stores the
 * cost of collapsing it into the point that minimises the summed quadrics.
 * A binary heap always expands the cheapest edge; collapsed vertices and
 * degenerate faces are flagged (never freed mid-run) and affected edges are
 * re-priced. Progress/cancellation callbacks let the host observe and abort.
 */
#ifndef LIBMESH_H
#define LIBMESH_H

#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef LIBMESH_BUILD
    #define LM_API __declspec(dllexport)
  #else
    #define LM_API __declspec(dllimport)
  #endif
#else
  #define LM_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. */
#define LM_OK              0
#define LM_ERR_MEMORY      1
#define LM_ERR_INVALID     2
#define LM_ERR_OPEN_FILE   3
#define LM_ERR_BAD_FORMAT  4
#define LM_ERR_CANCELLED   5
#define LM_ERR_NOT_READY   6
#define LM_ERR_RATIO       7

typedef struct lm_mesh lm_mesh;
typedef struct lm_task lm_task;

/*
 * Progress callback.
 *   user_data : opaque pointer registered with lm_set_progress_cb
 *   face_count: number of faces currently alive
 *   total     : face count of the original mesh
 * Returns non-zero to request cancellation.
 */
typedef int (*lm_progress_fn)(void *user_data, int face_count, int total);

/* ---- mesh lifecycle ---------------------------------------------------- */
LM_API lm_mesh *lm_mesh_load_obj(const char *path, int *err);
LM_API lm_mesh *lm_mesh_new(void);
/* vertices: xyz floats, count = vert_count*3; indices: 3 per face. */
LM_API int lm_mesh_set_data(lm_mesh *m, const float *vertices, int vert_count,
                            const int *indices, int face_count);
LM_API void lm_mesh_free(lm_mesh *m);

LM_API int lm_mesh_vertex_count(const lm_mesh *m);
LM_API int lm_mesh_face_count(const lm_mesh *m);
LM_API int lm_mesh_alive_vertex_count(const lm_mesh *m);
LM_API int lm_mesh_alive_face_count(const lm_mesh *m);
LM_API int lm_mesh_write_obj(const lm_mesh *m, const char *path);
/* Fill caller-owned arrays (positions: alive*3, indices: alive_faces*3).
 * Returns LM_OK or LM_ERR_INVALID when counts mismatch. */
LM_API int lm_mesh_get_data(const lm_mesh *m, float *vertices, int vert_cap,
                            int *indices, int face_cap);

/* ---- simplification task ---------------------------------------------- */
/* ratio in (0,1]: keep this fraction of the original faces. */
LM_API lm_task *lm_task_new(lm_mesh *mesh, float ratio, int *err);
LM_API void lm_task_set_progress_cb(lm_task *t, lm_progress_fn fn,
                                    void *user_data);
/* Thread-safe request to abort a running task (checked per edge). */
LM_API void lm_task_request_cancel(lm_task *t);
LM_API int lm_task_run(lm_task *t);
LM_API void lm_task_free(lm_task *t);

LM_API const char *lm_version(void);

#ifdef __cplusplus
}
#endif
#endif /* LIBMESH_H */

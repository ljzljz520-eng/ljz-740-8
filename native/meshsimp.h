/* libmeshsimp - minimal 3D mesh simplification library.
 *
 * C ABI consumed by the Go bindings via purego (no cgo).
 * Threading model: one ms_simplify() per context at a time;
 * ms_cancel() is async-safe and may be called from any thread.
 */
#ifndef MESHSIMP_H
#define MESHSIMP_H

#include <stdint.h>

#if defined(_WIN32)
#  define MS_API __declspec(dllexport)
#else
#  define MS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ms_context ms_context;

/* Progress callback: progress in [0,1]. Called from the thread
 * running ms_simplify(). */
typedef void (*ms_progress_fn)(float progress, void *user);

enum {
    MS_OK          =  0,
    MS_CANCELLED   =  1,   /* ms_simplify was cancelled; temp memory released */
    MS_ERR_ARG     = -1,
    MS_ERR_NOMEM   = -2,
    MS_ERR_IO      = -3,
    MS_ERR_PARSE   = -4,
    MS_ERR_STATE   = -5    /* e.g. export before a successful simplify */
};

MS_API ms_context *ms_create(void);
/* Frees every buffer owned by the context and the context itself. */
MS_API void        ms_destroy(ms_context *ctx);

/* Loads an OBJ model. Also aborts with MS_CANCELLED if ms_cancel()
 * is called while loading; the previous mesh (if any) is preserved. */
MS_API int  ms_load_obj(ms_context *ctx, const char *path);
/* ratio in (0,1]: target triangle count = input triangles * ratio. */
MS_API void ms_set_ratio(ms_context *ctx, float ratio);

/* Runs simplification. Returns MS_OK, MS_CANCELLED or MS_ERR_*.
 * On MS_CANCELLED all intermediate buffers are freed and any partial
 * output is discarded; the input mesh is preserved. */
MS_API int  ms_simplify(ms_context *ctx, ms_progress_fn cb, void *user);

/* Requests cancellation of a running ms_simplify(). Async-safe. */
MS_API void ms_cancel(ms_context *ctx);

/* Writes the simplified mesh. Fails with MS_ERR_STATE if there is no
 * valid simplified result (e.g. after cancellation). */
MS_API int  ms_export_obj(const ms_context *ctx, const char *path);

MS_API uint64_t ms_input_vertex_count(const ms_context *ctx);
MS_API uint64_t ms_input_face_count(const ms_context *ctx);
MS_API uint64_t ms_output_vertex_count(const ms_context *ctx);
MS_API uint64_t ms_output_face_count(const ms_context *ctx);

MS_API const char *ms_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif /* MESHSIMP_H */

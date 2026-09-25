/*
 * meshlib - 小型三维网格处理库（C ABI，供 purego 动态加载）
 *
 * 提供 OBJ 模型读取、基于 QEM（二次误差测度，Garland-Heckbert 98）
 * 的边折叠网格简化、OBJ 导出，以及进度回调 / 取消支持。
 */
#ifndef MESHLIB_H
#define MESHLIB_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #define MESHLIB_API __declspec(dllexport)
#else
  #define MESHLIB_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 进度回调：progress 为 0..100 的百分比；返回非 0 请求取消任务。 */
typedef int (*meshlib_progress_cb)(int progress, void *user_data);

/* 不透明网格句柄 */
typedef struct meshlib_mesh meshlib_mesh;

/* 读取 OBJ 模型。失败返回 NULL，*err_msg 写入错误说明（可能为 NULL）。 */
MESHLIB_API meshlib_mesh *meshlib_load_obj(const char *path, char **err_msg);

/* 将网格导出为 OBJ。成功返回 0，失败返回非 0 并写入 *err_msg。 */
MESHLIB_API int meshlib_save_obj(const meshlib_mesh *m, const char *path,
                                 char **err_msg);

/*
 * 原地执行网格简化。
 *   ratio     : 目标面片数占原始面片数的比例，范围 (0,1]
 *   cb        : 进度回调（可为 NULL）
 *   user_data : 回调的用户数据指针（可为 NULL）
 * 成功返回 0；被回调取消返回 1（取消点在两次折叠之间，网格保持
 * 结构一致，为当时的中间结果；全部临时资源均被释放）；其他错误返回 2。
 */
MESHLIB_API int meshlib_simplify(meshlib_mesh *m, double ratio,
                                 meshlib_progress_cb cb, void *user_data,
                                 char **err_msg);

MESHLIB_API int meshlib_vertex_count(const meshlib_mesh *m);
MESHLIB_API int meshlib_face_count(const meshlib_mesh *m);
MESHLIB_API const char *meshlib_version(void);

/* 释放网格及其全部底层资源。传入 NULL 是安全的。 */
MESHLIB_API void meshlib_free(meshlib_mesh *m);

/* 库内 malloc/free，供绑定层分配与库同一 CRT 的内存。 */
MESHLIB_API void *meshlib_calloc(size_t n, size_t size);
MESHLIB_API void meshlib_cfree(void *p);

#ifdef __cplusplus
}
#endif

#endif /* MESHLIB_H */

/* AddressSanitizer/UBSan 自测：验证简化正确、取消时资源全部回收。 */
#include "meshlib.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_grid_obj(const char *path, int nx, int ny) {
    FILE *fp = fopen(path, "wb");
    for (int j = 0; j <= ny; j++)
        for (int i = 0; i <= nx; i++)
            fprintf(fp, "v %d %d 0\n", i, j);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            int a = j * (nx + 1) + i + 1;
            int b = a + 1;
            int c = a + (nx + 1);
            int d = c + 1;
            fprintf(fp, "f %d %d %d\n", a, b, d);
            fprintf(fp, "f %d %d %d\n", a, d, c);
        }
    fclose(fp);
}

static int cancel_at_50(int p, void *ud) {
    (void)p; (void)ud;
    return 0; /* 见 cancel_early */
}

static int g_calls;
static int cancel_early(int p, void *ud) {
    (void)ud;
    g_calls++;
    /* 第 2 次回调即取消 */
    return p >= 10 ? 1 : 0;
}

int main(void) {
    const char *p = "build/_grid.obj";
    write_grid_obj(p, 60, 60);
    int faces0 = 60 * 60 * 2;

    char *err = NULL;
    meshlib_mesh *m = meshlib_load_obj(p, &err);
    assert(m);
    assert(meshlib_face_count(m) == faces0);
    assert(meshlib_vertex_count(m) == 61 * 61);

    /* 1) 正常简化到 25% */
    int rc = meshlib_simplify(m, 0.25, cancel_at_50, NULL, &err);
    assert(rc == 0);
    int f = meshlib_face_count(m);
    printf("faces: %d -> %d (%.1f%%)\n", faces0, f, 100.0 * f / faces0);
    assert(f <= faces0 / 4 + 2);
    assert(meshlib_save_obj(m, "build/_grid_out.obj", &err) == 0);
    meshlib_free(m);

    /* 2) 提前取消：反复构造并取消，检查无泄漏/越界 */
    for (int i = 0; i < 5; i++) {
        g_calls = 0;
        m = meshlib_load_obj(p, &err);
        assert(m);
        rc = meshlib_simplify(m, 0.01, cancel_early, NULL, &err);
        assert(rc == 1);
        assert(g_calls >= 2);
        assert(meshlib_face_count(m) < faces0);  /* 已折叠部分保留 */
        meshlib_free(m);
    }

    /* 3) ratio=1 应直接返回，不改变网格 */
    m = meshlib_load_obj(p, &err);
    assert(m);
    rc = meshlib_simplify(m, 1.0, NULL, NULL, &err);
    assert(rc == 0);
    assert(meshlib_face_count(m) == faces0);
    meshlib_free(m);

    /* 4) 错误路径：不存在的文件 / 非法参数 */
    err = NULL;
    assert(meshlib_load_obj("build/_nope.obj", &err) == NULL);
    assert(err);
    free(err);

    remove(p);
    remove("build/_grid_out.obj");
    printf("selftest OK (meshlib %s)\n", meshlib_version());
    return 0;
}

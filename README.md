# meshsimplify — Go 三维网格简化绑定

用 **purego**（运行时 `dlopen`，无需 cgo 工具链）加载本地 C 网格库 `libmesh`，
对三角网格执行基于 **二次误差度量（Garland–Heckbert QEM）** 的边折叠简化。

提供的能力：

- 读取模型（Wavefront `.obj`，多边形面自动扇形三角化）或从顶点/索引切片构建；
- 设置简化比例（保留面数比例 `ratio ∈ (0,1]`）；
- 执行简化（阻塞式，`context.Context` 可取消）；
- 导出结果（写 `.obj` 或取回紧凑的顶点/索引数据）；
- 进度回调（约 60Hz 节流，起止必达；回调返回 `true` 即可取消）；
- 取消或失败时**底层资源立即回收**，并辅以 finalizer 兜底。

## 目录结构

```
internal/libmesh/      C 核心库（QEM、OBJ 读写、取消检查点）
  mesh.h / mesh.c      C ABI（lm_*）
  libmesh.so           由 make 构建出的共享库
meshsimplify/          purego Go 绑定（无 cgo）
cmd/simplify/          示例命令：obj 简化后保存
Makefile               构建本地库 / 测试 / 示例
```

## 构建共享库

需要 C 编译器（cc/clang/gcc）。

```bash
make lib        # 生成 internal/libmesh/libmesh.so
# 或手动：
cc -O2 -fPIC -Wall -Wextra -shared internal/libmesh/mesh.c \
    -o internal/libmesh/libmesh.so -lm
```

运行时查找共享库的顺序：

1. `MESH_LIBRARY_PATH` 环境变量；
2. 可执行文件目录及其 `lib`、`internal/libmesh` 子目录；
3. 当前工作目录下的 `internal/libmesh`；
4. 系统路径 `/usr/local/lib`、`/usr/lib` 等。

也可在代码里显式指定：`meshsimplify.LoadLibrary("/path/to/libmesh.so")`。

## 快速开始（命令行）

```bash
make example
MESH_LIBRARY_PATH=$PWD/internal/libmesh/libmesh.so \
  ./bin/simplify -in model.obj -out model_simple.obj -ratio 0.25
```

输出：

```
loaded model.obj: 3600 vertices, 6962 faces
done in 53ms
wrote model_simple.obj: 926 vertices, 1740 faces (25.0% of input)
```

按 `Ctrl-C` 会在当前边折叠处中断任务并释放本地内存，退出码为 1。

## 作为库使用

```go
package main

import (
    "context"
    "log"

    mesh "github.com/example/meshsimplify/meshsimplify"
)

func main() {
    // 库不在默认搜索路径时可显式加载：
    // mesh.LoadLibrary("/opt/lib/libmesh.so")

    m, err := mesh.LoadOBJ("model.obj")
    if err != nil {
        log.Fatal(err)
    }
    defer m.Close() // 释放 lm_mesh

    task, err := mesh.NewTask(m, 0.25, mesh.WithProgress(
        func(faces, total int) (cancel bool) {
            log.Printf("progress: %d/%d faces", faces, total)
            return false // 返回 true 可中止
        }))
    if err != nil {
        log.Fatal(err)
    }
    defer task.Close() // 释放 lm_task（二次误差、边堆、邻接表）

    ctx := context.Background() // 也可 context.WithTimeout / WithCancel
    if err := task.Run(ctx); err != nil {
        log.Fatal(err)
    }

    if err := m.WriteOBJ("model_simple.obj"); err != nil {
        log.Fatal(err)
    }

    // 或直接取回紧凑数据：
    positions, indices, err := m.Data() // []float32 (xyz*N) / []int (3*F)
    _ = positions
    _ = indices
}
```

### 从内存数据构建

```go
m, _ := mesh.NewMesh()
positions := []float32{ /* xyz ... */ }
indices  := []int{ /* 0-based, 3 per triangle */ }
if err := m.SetData(positions, indices); err != nil { ... }
```

## 取消与资源回收

- `task.Run(ctx)`：`ctx` 取消时，Go 侧 watcher 会同时
  置位回调标志并调用原生 `lm_task_request_cancel`；C 主循环在**每条边**
  及每个进度检查点读取该标志并返回 `LM_ERR_CANCELLED`。
- 取消或失败后 `Run` 内部会立即调用原生 `lm_task_free`，回收二次误差矩阵、
  边哈希表、二叉堆与顶点邻接表；回调注册表项与用户数据也同步删除。
- `Mesh` 不会被 `Run` 释放——取消后仍可重新 `NewTask` 再试；调用方用
  `defer m.Close()` 管理。
- 即使忘记 `Close`，`runtime.SetFinalizer` 也会在 GC 时回收原生句柄
  （`make test` 中包含丢弃引用 + 强制 GC 的用例）。
- `Close` 幂等，可在取消后的任意时刻重复调用。

## 测试

```bash
make test
```

测试覆盖：OBJ 往返、面数精确收敛、进度回调、`context` 取消、回调取消、
非法比例、`SetData`、finalizer 回收，以及取消后用同一 mesh 重新简化。
测试无需 cgo；用 `CGO_ENABLED=1 go test -race ./...` 可加竞态检测。

## 算法说明（C 核心）

- 每个面由平面方程 `(a,b,c,d)` 构造基础误差二次型 `Q = p pᵀ`，
  顶点二次型为其入射面之和；
- 每条边的代价为合并点 `v*` 处的 `v*ᵀ(Qᵢ+Qⱼ)v*`，`v*` 通过求解
  3×3 正规方程得到（奇异时退化为边中点）；
- 二叉堆始终弹出代价最小的边，折叠后对受影响边重新计价（版本号使旧堆项
  惰性失效）；
- 合法性采用 Hoppe link condition，避免在非流形配置产生折叠翻转；
- 边集合使用开放寻址哈希表并按负载因子自动扩容。

## 平台

Linux / macOS（amd64、arm64）开箱即用；Windows 需对应构建 `mesh.dll`
（回调封装已提供 build tag 分离）。共享库本身只依赖 libm。

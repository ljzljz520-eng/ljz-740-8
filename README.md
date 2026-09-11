# meshsimp — Go 三维网格简化绑定

通过 [purego](https://github.com/ebitengine/purego)（无需 cgo）动态加载本地 C 网格简化库，
提供 OBJ 模型读取、简化比例设置、执行、结果导出与进度回调；附带示例命令行工具。

## 目录结构

```
native/            本地 C 库（顶点聚类简化算法，C11，零依赖）
  meshsimp.h         C ABI：ms_create/ms_load_obj/ms_set_ratio/ms_simplify/ms_cancel/ms_export_obj...
  meshsimp.c         实现：OBJ 解析、简化、进度回调、原子取消标志
  Makefile           构建 libmeshsimp.so
meshsimp/          Go 绑定包（purego）
  meshsimp.go        LoadLibrary + Simplifier(Load/SetRatio/Run/Cancel/Export/Counts/Close)
  meshsimp_test.go   简化、取消回收、运行中 Close、错误路径测试
cmd/meshsimp/        示例命令：obj 简化后保存，带进度条，Ctrl+C 取消
testdata/grid.obj    示例网格（3721 顶点 / 7200 三角形）
```

## 构建与测试

```sh
make            # 编译 native/libmeshsimp.so 并构建 Go 包
make test       # 运行全部测试
make run        # 示例：testdata/grid.obj -> /tmp/grid_simple.obj (25%)
```

要求：Go ≥ 1.25（purego v0.11）、gcc。注意必须 `CGO_ENABLED=0`
（purego 的 Go 回调实现只在无 cgo 时可用；Makefile 已自动设置）。

## 示例命令

```sh
go run ./cmd/meshsimp -lib native/libmeshsimp.so \
    -in model.obj -out model_small.obj -ratio 0.5
```

输出带实时进度条；运行中按 **Ctrl+C**（或 SIGTERM）会取消任务、
回收底层资源并以退出码 2 结束，不写出结果文件。

## Go API

```go
err := meshsimp.LoadLibrary("native/libmeshsimp.so") // 进程内一次

s, _ := meshsimp.New()
defer s.Close()                  // 释放 native 上下文与全部缓冲区

s.Load("model.obj")              // 读取模型（OBJ）
s.SetRatio(0.5)                  // 目标三角形比例 (0,1]
err = s.Run(func(p float64) {    // 执行；progress ∈ [0,1]
    fmt.Printf("\r%.0f%%", p*100)
})
// err == meshsimp.ErrCancelled 表示被取消
s.Export("model_small.obj")      // 导出结果
inV, inF, outV, outF := s.Counts()
```

## 取消与资源回收

- `Cancel()` 可从任意 goroutine / 信号处理函数调用，非阻塞；
  它会置位 native 侧的原子取消标志，`ms_simplify`/`ms_load_obj`
  在主循环中周期性检查。
- 取消时 native 立即释放全部中间缓冲区并丢弃部分结果，
  `Run` 返回 `ErrCancelled`；此后 `Export` 会失败，直到下一次成功的 `Run`。
- 没有任务在跑时调用 `Cancel` 会被记住一次，使紧随的 `Run` 返回
  `ErrCancelled`（Ctrl+C 早于 `Run` 开始也不会丢失）。
- `Close()` 先取消正在运行的任务、等待其退出，再销毁 native 上下文；
  幂等，可与 `Cancel` 并发。另设 finalizer 兜底防止泄漏。

## 说明与限制

- 简化算法为顶点聚类（vertex clustering）：按包围盒划分网格，
  同格顶点合并为其质心，退化三角形被剔除。比例为近似目标。
- OBJ 支持 `v`/`f`（含 `v/vt/vn` 与负索引），多边形面扇形三角化；
  忽略法线/纹理坐标。
- 每个 `Simplifier` 同一时间只能跑一个 `Run`（内部串行化）；
  多个 `Simplifier` 可并行。
- `-race` 不可用：Go race detector 需要 cgo，而 purego 回调要求 `CGO_ENABLED=0`。

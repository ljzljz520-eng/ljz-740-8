# meshbind — Go × purego 三维网格简化绑定

通过 [purego](https://github.com/ebitengine/purego) **动态加载本地 C 网格库 `meshlib`**（全程不使用 cgo），
在 Go 侧提供：读取 OBJ 模型 → 设置简化比例 → 执行 QEM 网格简化 → 导出结果，并带
**进度回调与任务取消**；取消时底层 C 工作区资源立即回收。

简化算法为经典的 QEM 二次误差测度边折叠
（Garland & Heckbert, SIGGRAPH 1998），用二叉最小堆维护候选边。

## 目录结构

```
meshlib/              本地 C 网格库（libmeshlib.so/.dylib/.dll）
  meshlib.h/.c        C ABI：load/save/simplify/count/free
  selftest.c          C 自测（配合 ASan/UBSan）
  Makefile            构建共享库
  build/              构建产物
mesh/                 purego Go 绑定（无 cgo）
  lib.go              库加载、符号绑定、模型打开
  simplify.go         Simplifier：比例 / 进度回调 / context 取消 / 导出 / 关闭
  dlfcn_*.go          各平台 dlopen / LoadLibrary
  alloc.go cstr.go    与库同一 CRT 的内存与 C 字符串辅助
cmd/simplify/         示例命令行：obj 简化后保存
```

## 构建

先构建本地共享库：

```bash
make -C meshlib
# 产物：meshlib/build/libmeshlib.so（Linux）
#       meshlib/build/libmeshlib.dylib（macOS）
#       meshlib/build/meshlib.dll（Windows / mingw）
```

交叉编译 Windows DLL 示例：

```bash
make -C meshlib CC=x86_64-w64-mingw32-gcc
```

Go 侧直接编译，无需 CGO_ENABLED：

```bash
go build ./...
go test ./...
```

## 示例

```bash
go build -o simplify ./cmd/simplify
./simplify -in model.obj -out model_low.obj -ratio 0.25
```

输出示例：

```
meshlib version: 1.0.0
loaded: 6561 vertices, 12800 faces
progress: 100%
simplified: 1650 vertices, 3199 faces
saved to: model_low.obj
```

按 `Ctrl+C` 可随时取消，取消后打印「任务已取消，底层资源已释放」并退出，
C 侧临时的边表 / 哈希 / 堆 / 邻接数组全部立即 free。

库搜索路径：`MESHLIB_PATH` 环境变量 → 当前目录 / `meshlib/build` →
可执行文件目录相邻的 `meshlib/build` → `/usr/local/lib` 等。
也可用 `-lib`（命令行）或 `mesh.LoadLibrary(path)`（代码）显式指定。

## Go API 速览

```go
lib, _ := mesh.LoadLibrary("")                 // 自动搜索库文件
defer lib.Close()

s, _ := lib.OpenModel("model.obj")             // 读取模型
defer s.Close()                                 // 回收底层网格（必须）

_ = s.SetRatio(0.25)                            // 目标面片占比 (0,1]
s.SetProgress(func(p int) (cancel bool) {      // 0..100 进度
    fmt.Println(p)
    return false                                // 返回 true 即取消
})

err := s.SimplifyContext(ctx)                  // 也可用 context 取消
if errors.Is(err, mesh.ErrCanceled) {
    // 已取消；底层资源已释放。s 可继续 Export（保留中间结果）或 Close。
}
_ = s.Export("model_low.obj")
```

## 取消与资源回收

- 取消检查点位于**两次边折叠之间**，因此取消时网格始终结构一致，
  取消时刻的中间结果仍可导出。
- C 侧所有临时内存（工作点 / 面副本、Q 矩阵、边数组、开放寻址哈希、
  最小堆、邻接表、时间戳数组）在取消后与正常结束走同一释放路径。
- Go 侧 `Simplifier.Close()` 调用 `meshlib_free`；并设置了 finalizer 兜底。
- 已用 AddressSanitizer/UBSan 在「正常简化 / 回调取消 / context 取消 /
  并发 / 反复加载-取消-关闭-dlclose」各路径验证无内存错误与泄漏：

```bash
make -C meshlib check     # C 自测（ASan/UBSan）
go test ./...
```

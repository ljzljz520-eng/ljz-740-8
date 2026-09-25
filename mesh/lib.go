// Package mesh 通过 purego 动态加载本地 meshlib 共享库（无需 cgo），
// 提供三维网格模型读取、QEM 网格简化、结果导出以及进度回调能力。
//
// 典型流程：
//
//	lib, err := mesh.LoadLibrary("")          // 自动搜索或指定 .so/.dylib/.dll
//	defer lib.Close()
//	s, err := lib.OpenModel("input.obj")      // 读取模型
//	defer s.Close()                            // 释放底层网格资源
//	_ = s.SetRatio(0.25)                      // 设置简化比例（目标面片占比）
//	s.SetProgress(func(p int) bool { ... })    // 可选：进度/取消回调
//	if err := s.Simplify(); !errors.Is(err, mesh.ErrCanceled) {
//	    _ = s.Export("output.obj")
//	}
package mesh

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"unsafe"

	"github.com/ebitengine/purego"
)

// 库内 C ABI 函数签名：
//
//	meshlib_mesh* meshlib_load_obj(const char* path, char** err)
//	int           meshlib_save_obj(mesh*, const char* path, char** err)
//	int           meshlib_simplify(mesh*, double ratio,
//	                              progress_cb cb, void* user, char** err)
//	int           meshlib_vertex_count(const mesh*)
//	int           meshlib_face_count(const mesh*)
//	const char*   meshlib_version(void)
//	void          meshlib_free(mesh*)

type library struct {
	handle uintptr
	mu     sync.Mutex

	loadObj  func(path unsafe.Pointer, errp *unsafe.Pointer) unsafe.Pointer
	saveObj  func(m unsafe.Pointer, path unsafe.Pointer, errp *unsafe.Pointer) int32
	simplify func(m unsafe.Pointer, ratio float64, cb uintptr, user uintptr,
		errp *unsafe.Pointer) int32
	vcount   func(m unsafe.Pointer) int32
	fcount   func(m unsafe.Pointer) int32
	version  func() unsafe.Pointer
	freeMesh func(m unsafe.Pointer)
	cCalloc  func(n, size uintptr) unsafe.Pointer
	cFree    func(p unsafe.Pointer)
}

// Library 是已加载的 meshlib 动态库。可并发打开多个模型。
type Library struct{ l *library }

// LibName 返回当前平台上 meshlib 共享库的文件名。
func LibName() string {
	switch runtime.GOOS {
	case "darwin":
		return "libmeshlib.dylib"
	case "windows":
		return "meshlib.dll"
	default:
		return "libmeshlib.so"
	}
}

// LoadLibrary 加载本地 meshlib 共享库。
// path 为空时按 DefaultSearchPaths 的顺序自动搜索库文件。
func LoadLibrary(path string) (*Library, error) {
	path, err := resolveLibrary(path)
	if err != nil {
		return nil, err
	}

	lib := &library{}
	lib.mu.Lock()
	defer lib.mu.Unlock()

	h, err := openOS(path)
	if err != nil {
		return nil, fmt.Errorf("mesh: load %q: %w", path, err)
	}
	lib.handle = h

	if err := lib.bind(); err != nil {
		_ = lib.unloadOS()
		return nil, err
	}
	return &Library{l: lib}, nil
}

func (l *library) bind() error {
	syms := []struct {
		name string
		out  any
	}{
		{"meshlib_load_obj", &l.loadObj},
		{"meshlib_save_obj", &l.saveObj},
		{"meshlib_simplify", &l.simplify},
		{"meshlib_vertex_count", &l.vcount},
		{"meshlib_face_count", &l.fcount},
		{"meshlib_version", &l.version},
		{"meshlib_free", &l.freeMesh},
		{"meshlib_calloc", &l.cCalloc},
		{"meshlib_cfree", &l.cFree},
	}
	for _, s := range syms {
		addr, err := dlsym(l.handle, s.name)
		if err != nil {
			return fmt.Errorf("mesh: symbol %q not found: %w", s.name, err)
		}
		purego.RegisterFunc(s.out, addr)
	}
	// 让包级分配器指向本库导出的 calloc/free（与库同一 CRT）
	nativeCalloc = l.cCalloc
	nativeFree = l.cFree
	return nil
}

// Version 返回底层 meshlib 的版本字符串。
func (lib *Library) Version() string {
	return cString(lib.l.version())
}

// Close 卸载动态库。调用前应先 Close 所有由它打开的 Simplifier。
func (lib *Library) Close() error {
	lib.l.mu.Lock()
	defer lib.l.mu.Unlock()
	return lib.l.unloadOS()
}

// OpenModel 读取 OBJ 模型并返回其 Simplifier 句柄。
// 调用方必须在结束后调用 Simplifier.Close 回收底层资源
// （即使简化被取消或失败）。
func (lib *Library) OpenModel(path string) (*Simplifier, error) {
	cpath, free := cBytes(path)
	defer free()

	var cerr unsafe.Pointer
	handle := lib.l.loadObj(cpath, &cerr)
	if handle == nil {
		msg := cString(cerr)
		freeCString(cerr)
		return nil, fmt.Errorf("mesh: load model %q: %s", path, orUnknown(msg))
	}

	s := &Simplifier{lib: lib.l, handle: handle, ratio: 1.0}
	runtime.SetFinalizer(s, func(x *Simplifier) {
		_ = x.Close() // 兜底：忘记 Close 时也尽量回收底层网格
	})
	return s, nil
}

func orUnknown(msg string) string {
	if msg == "" {
		return "unknown native error"
	}
	return msg
}

// DefaultSearchPaths 是 path 为空时搜索库文件的目录列表（运行时计算）。
func DefaultSearchPaths() []string {
	var dirs []string

	if p := os.Getenv("MESHLIB_PATH"); p != "" {
		dirs = append(dirs, p)
	}
	if cwd, err := os.Getwd(); err == nil {
		dirs = append(dirs, cwd, filepath.Join(cwd, "meshlib", "build"))
	}
	if exe, err := os.Executable(); err == nil {
		exedir := filepath.Dir(exe)
		dirs = append(dirs, exedir, filepath.Join(exedir, "meshlib", "build"))
	}
	dirs = append(dirs, "/usr/local/lib", "/usr/lib", "/usr/lib64")
	return dirs
}

func resolveLibrary(path string) (string, error) {
	name := LibName()
	if path != "" {
		fi, err := os.Stat(path)
		if err != nil || fi.IsDir() {
			if err == nil {
				return filepath.Join(path, name), nil
			}
			return "", fmt.Errorf("mesh: library not found: %s", path)
		}
		return path, nil
	}

	for _, dir := range DefaultSearchPaths() {
		cand := filepath.Join(dir, name)
		if fi, err := os.Stat(cand); err == nil && !fi.IsDir() {
			return cand, nil
		}
	}
	return "", fmt.Errorf("mesh: cannot locate %s; build it with `make -C meshlib` "+
		"or set MESHLIB_PATH", name)
}

// ErrCanceled 表示简化任务被进度回调或 context 取消。
// 此时底层工作区资源已释放，Simplifier 仍需 Close。
var ErrCanceled = errors.New("mesh: simplification canceled")

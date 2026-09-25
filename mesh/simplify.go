package mesh

import (
	"context"
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"unsafe"

	"github.com/ebitengine/purego"
)

// ProgressFunc 是进度回调：p 取值 0..100。
// 返回 true 表示请求取消正在执行的简化任务。
type ProgressFunc func(p int) (cancel bool)

// 简化任务的状态（通过 id 传递给 C 侧 user_data，避免 Go 指针穿过边界）。
type taskState struct {
	progress ProgressFunc
	ctx      context.Context
	canceled atomic.Bool
}

var (
	taskMu     sync.Mutex
	taskSeq    uint64
	taskReg    = map[uint64]*taskState{}
	callbackFn uintptr
	cbOnce     sync.Once
)

// trampoline 由 purego 转换为 C 函数指针：
// int cb(int progress, uintptr_t user_data)
func progressTrampoline(progress int32, user uintptr) int32 {
	taskMu.Lock()
	st := taskReg[uint64(user)]
	taskMu.Unlock()
	if st == nil {
		return 0
	}
	// context 取消优先
	if st.ctx != nil && st.ctx.Err() != nil {
		st.canceled.Store(true)
	}
	if st.canceled.Load() {
		return 1
	}
	if st.progress != nil {
		// 回调内 panic 不应穿透到 C 边界
		cancel := safeProgress(st.progress, int(progress))
		if cancel {
			st.canceled.Store(true)
		}
	}
	if st.canceled.Load() {
		return 1
	}
	return 0
}

func safeProgress(fn ProgressFunc, p int) (cancel bool) {
	defer func() { _ = recover() }()
	return fn(p)
}

func ensureCallback() uintptr {
	cbOnce.Do(func() {
		callbackFn = purego.NewCallback(progressTrampoline)
	})
	return callbackFn
}

func registerTask(st *taskState) uint64 {
	taskMu.Lock()
	defer taskMu.Unlock()
	taskSeq++
	taskReg[taskSeq] = st
	return taskSeq
}

func unregisterTask(id uint64) {
	taskMu.Lock()
	delete(taskReg, id)
	taskMu.Unlock()
}

// Simplifier 表示一个已加载的网格模型及其简化操作句柄。
// 一个 Simplifier 同一时间只能执行一个简化任务。
type Simplifier struct {
	lib    *library
	handle unsafe.Pointer
	mu     sync.Mutex
	closed bool

	ratio    float64
	progress ProgressFunc
}

// Vertices 返回当前模型的顶点数。
func (s *Simplifier) Vertices() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return 0
	}
	return int(s.lib.vcount(s.handle))
}

// Faces 返回当前模型的三角面片数。
func (s *Simplifier) Faces() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return 0
	}
	return int(s.lib.fcount(s.handle))
}

// SetRatio 设置简化比例：简化后面片数占原面片数的比例，范围 (0,1]。
// 例如 0.25 表示把面片数减少到约四分之一。
func (s *Simplifier) SetRatio(r float64) error {
	if r <= 0 || r > 1 {
		return fmt.Errorf("mesh: ratio must be in (0,1], got %v", r)
	}
	s.ratio = r
	return nil
}

// SetProgress 设置进度回调（可为 nil）。回调中返回 true 可取消任务。
func (s *Simplifier) SetProgress(fn ProgressFunc) {
	s.progress = fn
}

// Simplify 使用已设置的参数执行网格简化。
// 任务被取消时返回 ErrCanceled；此时底层工作区资源已立即释放，
// 网格保留取消时刻的一致中间结果，可继续 Export，也可直接 Close。
func (s *Simplifier) Simplify() error {
	return s.SimplifyContext(context.Background())
}

// SimplifyContext 同 Simplify，但可通过 ctx 取消任务
// （例如关联 os signal 的 context）。
func (s *Simplifier) SimplifyContext(ctx context.Context) error {
	if ctx == nil {
		ctx = context.Background()
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return fmt.Errorf("mesh: simplifier already closed")
	}
	if s.ratio <= 0 {
		s.ratio = 1
	}

	st := &taskState{progress: s.progress, ctx: ctx}
	taskID := registerTask(st)
	defer unregisterTask(taskID)

	cb := ensureCallback()

	var cerr unsafe.Pointer
	rc := s.lib.simplify(s.handle, s.ratio, cb, uintptr(taskID), &cerr)

	if rc == 1 {
		// 取消：底层已释放全部工作区资源
		freeCString(cerr)
		return ErrCanceled
	}
	if rc != 0 {
		msg := cString(cerr)
		freeCString(cerr)
		return fmt.Errorf("mesh: simplify failed: %s", orUnknown(msg))
	}
	freeCString(cerr)
	runtime.KeepAlive(st)
	return nil
}

// Export 将当前网格导出为 OBJ 文件。
func (s *Simplifier) Export(path string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return fmt.Errorf("mesh: simplifier already closed")
	}
	cpath, free := cBytes(path)
	defer free()

	var cerr unsafe.Pointer
	rc := s.lib.saveObj(s.handle, cpath, &cerr)
	if rc != 0 {
		msg := cString(cerr)
		freeCString(cerr)
		return fmt.Errorf("mesh: export %q: %s", path, orUnknown(msg))
	}
	freeCString(cerr)
	return nil
}

// Close 释放底层网格及其全部本地资源。可重复调用。
func (s *Simplifier) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil
	}
	s.closed = true
	s.lib.freeMesh(s.handle)
	s.handle = nil
	return nil
}

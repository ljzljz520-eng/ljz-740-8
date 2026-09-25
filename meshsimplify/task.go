package meshsimplify

import (
	"context"
	"runtime"
	"sync"
	"sync/atomic"
)

// Task is a configured simplification run. It owns native scratch memory
// (quadrics, edge heap, adjacency tables); call Close to reclaim it.
type Task struct {
	mesh *Mesh

	mu      sync.Mutex
	ptr     uintptr
	started bool
	done    bool

	st      *progState
	cbID    int64
	unregCB func()
	tramp   uintptr

	closeOnce sync.Once
	closed    atomic.Bool
}

// Option configures a Task.
type Option func(*Task)

// WithProgress registers a callback invoked during simplification (roughly
// one call per percent). Returning true cancels the run.
func WithProgress(fn ProgressFunc) Option {
	return func(t *Task) { t.st.fn = fn }
}

// NewTask prepares a simplification of mesh to ratio of its face count
// (ratio must lie in (0,1]). Native scratch memory is allocated here; defer
// Close to release it.
func NewTask(mesh *Mesh, ratio float32, opts ...Option) (*Task, error) {
	if mesh == nil {
		return nil, &Error{Code: libErrInvalid, Op: "new task"}
	}
	n, err := loaded()
	if err != nil {
		return nil, err
	}
	mesh.mu.Lock()
	defer mesh.mu.Unlock()
	if mesh.ptr == 0 {
		return nil, &Error{Code: libErrInvalid, Op: "new task"}
	}

	t := &Task{mesh: mesh, st: &progState{}}
	for _, o := range opts {
		o(t)
	}

	var code int32
	p := n.taskNew(mesh.ptr, ratio, &code)
	if p == 0 {
		return nil, libErr("new task", code)
	}
	t.ptr = p
	runtime.SetFinalizer(t, finalizeTask)

	// Synthesise a C-callable trampoline and pass the registry id as the
	// native user-data. No Go pointer crosses the FFI boundary.
	t.cbID, t.unregCB = callbacks.register(t.st)
	t.tramp = puregoCallback(nativeProgress)
	n.taskSetProgCB(p, t.tramp, uintptr(t.cbID))
	return t, nil
}

// Run executes the simplification. When ctx is cancelled (or the progress
// callback returns true), the native run stops at its next checkpoint,
// native task resources are reclaimed and an error wrapping
// LM_ERR_CANCELLED is returned. The underlying Mesh is never freed here, so
// the caller can inspect, reload or retry after a cancellation.
func (t *Task) Run(ctx context.Context) error {
	t.mu.Lock()
	if t.ptr == 0 {
		t.mu.Unlock()
		return &Error{Code: libErrInvalid, Op: "run"}
	}
	if t.started {
		t.mu.Unlock()
		return &Error{Code: libErrInvalid, Op: "run: task already used"}
	}
	t.started = true
	n, err := loaded()
	if err != nil {
		t.mu.Unlock()
		return err
	}
	t.mu.Unlock()

	// Bridge context.Done to the native cancel flag. Two paths are wired:
	// the progress callback observes st.cancel (checked at ~60 Hz inside
	// the C loop), and lm_task_request_cancel is invoked directly so the
	// flag is also checked per edge between progress emissions.
	watchDone := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			t.st.cancel.Store(true)
			t.mu.Lock()
			p := t.ptr
			t.mu.Unlock()
			if p != 0 {
				if nn, err := loaded(); err == nil {
					nn.taskCancel(p)
				}
			}
		case <-watchDone:
		}
	}()
	defer close(watchDone)

	code := n.taskRun(t.ptr)
	if code != libOK {
		// Cancellation or failure: release all native task resources now.
		_ = t.Close()
		return libErr("run", code)
	}
	t.mu.Lock()
	t.done = true
	t.mu.Unlock()
	return nil
}

// Done reports whether Run completed successfully.
func (t *Task) Done() bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.done
}

// Close releases every native resource owned by the task. It never frees
// the underlying Mesh. Safe to call repeatedly and after a cancelled Run.
func (t *Task) Close() error {
	t.closeOnce.Do(t.release)
	return nil
}

func (t *Task) release() {
	if !t.closed.CompareAndSwap(false, true) {
		return
	}
	t.mu.Lock()
	p := t.ptr
	t.ptr = 0
	t.mu.Unlock()

	if p != 0 {
		if n, err := loaded(); err == nil {
			n.taskFree(p)
		}
	}
	if t.unregCB != nil {
		t.unregCB()
	}
	runtime.SetFinalizer(t, nil)
}

func finalizeTask(t *Task) { t.release() }

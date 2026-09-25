package meshsimplify

import (
	"sync"
	"sync/atomic"
)

// ProgressFunc reports simplification progress during a run. Returning true
// requests cancellation (equivalent to cancelling the task context).
type ProgressFunc func(faceCount, totalFaces int) (cancel bool)

// callbackRegistry maps integer ids to *progState. The native callback gets
// the id as its void* user-data, avoiding any pointer into the moving Go
// heap (works with CGO_ENABLED=0 where runtime/cgo.Handle is unavailable).
type callbackRegistry struct {
	mu    sync.RWMutex
	next  int64
	items map[int64]*progState
}

var callbacks = &callbackRegistry{items: map[int64]*progState{}}

func (r *callbackRegistry) register(st *progState) (int64, func()) {
	id := atomic.AddInt64(&r.next, 1)
	r.mu.Lock()
	r.items[id] = st
	r.mu.Unlock()
	return id, func() {
		r.mu.Lock()
		delete(r.items, id)
		r.mu.Unlock()
	}
}

func (r *callbackRegistry) lookup(id int64) *progState {
	r.mu.RLock()
	st := r.items[id]
	r.mu.RUnlock()
	return st
}

// progState backs one native progress-callback registration.
type progState struct {
	fn     ProgressFunc
	cancel atomic.Bool
}

// nativeProgress matches int (*)(void*, int, int). Invoked on a non-Go
// thread by the purego callback stub; it only performs short, non-blocking
// work and never allocates native resources.
func nativeProgress(arg uintptr, faces, total int64) int32 {
	id := int64(arg)
	st := callbacks.lookup(id)
	if st == nil {
		return 0
	}
	if st.cancel.Load() {
		return 1
	}
	if st.fn != nil && st.fn(int(faces), int(total)) {
		st.cancel.Store(true)
		return 1
	}
	return 0
}

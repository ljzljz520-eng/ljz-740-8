// Package meshsimp provides Go bindings for the native libmeshsimp
// 3D mesh simplification library, loaded dynamically via purego
// (no cgo required).
//
// Usage:
//
//	if err := meshsimp.LoadLibrary("native/libmeshsimp.so"); err != nil { ... }
//	s, _ := meshsimp.New()
//	defer s.Close()          // releases all native resources
//	s.Load("model.obj")
//	s.SetRatio(0.5)
//	err := s.Run(func(p float64) { fmt.Printf("%.0f%%\n", p*100) })
//	s.Export("model_simple.obj")
//
// Cancellation: call Cancel from any goroutine (e.g. on SIGINT); Run
// returns ErrCancelled, the native side frees its intermediate buffers,
// and Close releases the remaining context.
package meshsimp

import (
	"errors"
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"unsafe"

	"github.com/ebitengine/purego"
)

// ErrCancelled is returned by Run when the operation was cancelled
// via Cancel (or Close).
var ErrCancelled = errors.New("meshsimp: operation cancelled")

// ErrClosed is returned when operating on a closed Simplifier.
var ErrClosed = errors.New("meshsimp: simplifier is closed")

// Native return codes (keep in sync with native/meshsimp.h).
const (
	msOK        = 0
	msCancelled = 1
)

/* ------------------------------------------------------------------ */
/* Library loading                                                     */

var (
	fnCreate     func() uintptr
	fnDestroy    func(ctx uintptr)
	fnLoadObj    func(ctx uintptr, path *byte) int32
	fnSetRatio   func(ctx uintptr, ratio float32)
	fnSimplify   func(ctx uintptr, cb uintptr, user uintptr) int32
	fnCancel     func(ctx uintptr)
	fnExportObj  func(ctx uintptr, path *byte) int32
	fnInVerts    func(ctx uintptr) uint64
	fnInFaces    func(ctx uintptr) uint64
	fnOutVerts   func(ctx uintptr) uint64
	fnOutFaces   func(ctx uintptr) uint64
	fnStrerror   func(code int32) unsafe.Pointer // returns char*
	fnStrerrorOK bool

	loadMu    sync.Mutex
	loaded    bool
	libHandle uintptr
)

// LoadLibrary dlopen's the native shared library and resolves all
// symbols. It must be called once before New. path may be an absolute
// path or a name resolved by the dynamic loader (e.g. "libmeshsimp.so").
func LoadLibrary(path string) error {
	loadMu.Lock()
	defer loadMu.Unlock()
	if loaded {
		return errors.New("meshsimp: library already loaded")
	}

	h, err := purego.Dlopen(path, purego.RTLD_NOW|purego.RTLD_GLOBAL)
	if err != nil {
		return fmt.Errorf("meshsimp: dlopen %q: %w", path, err)
	}

	reg := func(fn interface{}, name string) error {
		sym, err := purego.Dlsym(h, name)
		if err != nil {
			return fmt.Errorf("meshsimp: missing symbol %s: %w", name, err)
		}
		purego.RegisterFunc(fn, sym)
		return nil
	}

	bindings := []struct {
		fn   interface{}
		name string
	}{
		{&fnCreate, "ms_create"},
		{&fnDestroy, "ms_destroy"},
		{&fnLoadObj, "ms_load_obj"},
		{&fnSetRatio, "ms_set_ratio"},
		{&fnSimplify, "ms_simplify"},
		{&fnCancel, "ms_cancel"},
		{&fnExportObj, "ms_export_obj"},
		{&fnInVerts, "ms_input_vertex_count"},
		{&fnInFaces, "ms_input_face_count"},
		{&fnOutVerts, "ms_output_vertex_count"},
		{&fnOutFaces, "ms_output_face_count"},
	}
	for _, b := range bindings {
		if err := reg(b.fn, b.name); err != nil {
			return err
		}
	}
	// Optional symbol: nicer error messages when present.
	if sym, err := purego.Dlsym(h, "ms_strerror"); err == nil {
		purego.RegisterFunc(&fnStrerror, sym)
		fnStrerrorOK = true
	}

	libHandle = h
	loaded = true
	return nil
}

func nativeErr(op string, code int32) error {
	if fnStrerrorOK {
		msg := goString(fnStrerror(code))
		return fmt.Errorf("meshsimp: %s failed: %s (code %d)", op, msg, code)
	}
	return fmt.Errorf("meshsimp: %s failed (code %d)", op, code)
}

// goString copies a NUL-terminated C string into a Go string.
func goString(p unsafe.Pointer) string {
	if p == nil {
		return ""
	}
	n := 0
	for *(*byte)(unsafe.Add(p, n)) != 0 {
		n++
	}
	return string(unsafe.Slice((*byte)(p), n))
}

/* ------------------------------------------------------------------ */
/* Progress callback bridge                                            */

var (
	cbMu sync.Mutex
	cbs  = map[uintptr]func(float64){}
)

// progressTrampoline is the single C->Go callback; user is the native
// context pointer, used to find the per-Simplifier Go callback.
var progressTrampoline = purego.NewCallback(func(progress float32, user uintptr) {
	cbMu.Lock()
	cb := cbs[user]
	cbMu.Unlock()
	if cb != nil {
		cb(float64(progress))
	}
})

/* ------------------------------------------------------------------ */
/* Simplifier                                                          */

// Simplifier wraps one native ms_context. It is safe for concurrent
// use: Cancel may be called while Run is blocked in native code.
type Simplifier struct {
	ctx uintptr

	runMu   sync.Mutex // serializes Run; Close waits for a running Run
	stateMu sync.Mutex // guards closed and all quick native calls
	closed  bool

	// pendingCancel remembers a Cancel that arrived while no Run was
	// active; the next Run consumes it and returns ErrCancelled.
	pendingCancel atomic.Bool
}

// New creates a Simplifier holding a fresh native context.
// Call Close to release the native resources.
func New() (*Simplifier, error) {
	loadMu.Lock()
	ok := loaded
	loadMu.Unlock()
	if !ok {
		return nil, errors.New("meshsimp: LoadLibrary must be called first")
	}
	ctx := fnCreate()
	if ctx == 0 {
		return nil, errors.New("meshsimp: ms_create failed")
	}
	s := &Simplifier{ctx: ctx}
	// Safety net for forgotten Close; explicit Close is preferred.
	runtime.SetFinalizer(s, func(s *Simplifier) { _ = s.Close() })
	return s, nil
}

// Load reads an OBJ model from path, replacing any previous mesh.
func (s *Simplifier) Load(path string) error {
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	if s.closed {
		return ErrClosed
	}
	cpath := append([]byte(path), 0)
	rc := fnLoadObj(s.ctx, &cpath[0])
	runtime.KeepAlive(cpath)
	switch rc {
	case msOK:
		return nil
	case msCancelled:
		return ErrCancelled
	default:
		return nativeErr("load "+path, rc)
	}
}

// SetRatio sets the target size as a fraction of the input triangle
// count, in (0,1]. Values outside are clamped by the native side.
func (s *Simplifier) SetRatio(ratio float64) error {
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	if s.closed {
		return ErrClosed
	}
	fnSetRatio(s.ctx, float32(ratio))
	return nil
}

// Run executes the simplification, invoking onProgress with values in
// [0,1] as work advances (onProgress may be nil). It blocks until done
// and returns ErrCancelled if Cancel was called.
func (s *Simplifier) Run(onProgress func(float64)) error {
	s.runMu.Lock()
	defer s.runMu.Unlock()

	s.stateMu.Lock()
	if s.closed {
		s.stateMu.Unlock()
		return ErrClosed
	}
	s.stateMu.Unlock()

	// A Cancel that arrived while no Run was active cancels this Run.
	if s.pendingCancel.Swap(false) {
		return ErrCancelled
	}

	if onProgress != nil {
		cbMu.Lock()
		cbs[s.ctx] = onProgress
		cbMu.Unlock()
		defer func() {
			cbMu.Lock()
			delete(cbs, s.ctx)
			cbMu.Unlock()
		}()
	}

	rc := fnSimplify(s.ctx, progressTrampoline, s.ctx)
	switch rc {
	case msOK:
		return nil
	case msCancelled:
		s.pendingCancel.Store(false) // the cancel was honored here
		return ErrCancelled
	default:
		return nativeErr("simplify", rc)
	}
}

// Cancel requests cancellation of a running Load or Run. It is
// non-blocking and safe to call from any goroutine or signal handler.
// A Cancel that arrives while nothing is running is remembered and
// cancels the next Run once. After Run returns ErrCancelled, the
// native intermediate buffers are already freed and Export will fail
// until the next successful Run.
func (s *Simplifier) Cancel() {
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	if s.closed {
		return
	}
	s.pendingCancel.Store(true)
	fnCancel(s.ctx)
}

// Export writes the simplified mesh as an OBJ file. It fails if no
// valid result exists (e.g. the last Run was cancelled).
func (s *Simplifier) Export(path string) error {
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	if s.closed {
		return ErrClosed
	}
	cpath := append([]byte(path), 0)
	rc := fnExportObj(s.ctx, &cpath[0])
	runtime.KeepAlive(cpath)
	if rc != msOK {
		return nativeErr("export "+path, rc)
	}
	return nil
}

// Counts returns the input and output mesh sizes
// (inputVerts, inputFaces, outputVerts, outputFaces).
func (s *Simplifier) Counts() (inV, inF, outV, outF int) {
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	if s.closed {
		return 0, 0, 0, 0
	}
	return int(fnInVerts(s.ctx)), int(fnInFaces(s.ctx)),
		int(fnOutVerts(s.ctx)), int(fnOutFaces(s.ctx))
}

// Close cancels any in-flight Run, waits for it to finish, and frees
// all native resources. It is idempotent and safe to call concurrently
// with Cancel.
func (s *Simplifier) Close() error {
	// Mark closed first so new calls fail fast.
	s.stateMu.Lock()
	if s.closed {
		s.stateMu.Unlock()
		return nil
	}
	s.closed = true
	fnCancel(s.ctx) // unblock a running ms_simplify
	s.stateMu.Unlock()

	// Wait for an in-flight Run, then destroy the native context.
	s.runMu.Lock()
	s.stateMu.Lock()
	fnDestroy(s.ctx)
	s.ctx = 0
	s.stateMu.Unlock()
	s.runMu.Unlock()

	runtime.SetFinalizer(s, nil)
	return nil
}

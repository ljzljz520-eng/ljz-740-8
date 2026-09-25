package meshsimplify

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestMain(m *testing.M) {
	// Tests assume the built library next to the C sources; the Makefile
	// target `test` builds it first.
	if p := os.Getenv("MESH_LIBRARY_PATH"); p != "" {
		_ = LoadLibrary(p)
	} else {
		_ = LoadLibrary(filepath.Join("..", "internal", "libmesh",
			libName()))
	}
	os.Exit(m.Run())
}

func libName() string {
	return map[string]string{
		"darwin":  "libmesh.dylib",
		"windows": "mesh.dll",
	}[goos()]
}

func goos() string {
	// tiny indirection to avoid importing runtime.GOOS awkwardly
	return runtime.GOOS
}

// writeGridOBJ emits an n x n warped grid as triangles.
func writeGridOBJ(t *testing.T, n int) string {
	t.Helper()
	dir := t.TempDir()
	p := filepath.Join(dir, "grid.obj")
	var b strings.Builder
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			x := float64(j) / float64(n-1)
			y := float64(i) / float64(n-1)
			z := float64((i*i+j*j)%5) * 0.1
			fmt.Fprintf(&b, "v %.3f %.3f %.3f\n", x, y, z)
		}
	}
	for i := 0; i < n-1; i++ {
		for j := 0; j < n-1; j++ {
			a := i*n + j + 1
			fmt.Fprintf(&b, "f %d %d %d\n", a, a+n, a+n+1)
			fmt.Fprintf(&b, "f %d %d %d\n", a, a+n+1, a+1)
		}
	}
	if err := os.WriteFile(p, []byte(b.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestSimplifyGrid(t *testing.T) {
	path := writeGridOBJ(t, 30)
	m, err := LoadOBJ(path)
	if err != nil {
		t.Fatalf("LoadOBJ: %v", err)
	}
	defer m.Close()

	v, f := m.Counts()
	if v != 900 || f != 2*29*29 {
		t.Fatalf("initial counts = (%d,%d), want (900,%d)", v, f, 2*29*29)
	}

	var calls atomic.Int32
	var lastFaces atomic.Int32
	task, err := NewTask(m, 0.25, WithProgress(
		func(faces, total int) bool {
			calls.Add(1)
			lastFaces.Store(int32(faces))
			return false
		}))
	if err != nil {
		t.Fatalf("NewTask: %v", err)
	}
	if err := task.Run(context.Background()); err != nil {
		t.Fatalf("Run: %v", err)
	}
	if !task.Done() {
		t.Fatal("task not marked done")
	}
	if calls.Load() == 0 {
		t.Fatal("progress callback never invoked")
	}
	if err := task.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	_, faces := m.Counts()
	if g, w := faces, int(float64(f)*0.25); g != w {
		t.Fatalf("faces after = %d, want %d", g, w)
	}
	if int(lastFaces.Load()) != faces {
		t.Fatalf("last progress %d != result %d", lastFaces.Load(), faces)
	}

	// exported geometry must be compact and valid
	pos, idx, err := m.Data()
	if err != nil {
		t.Fatalf("Data: %v", err)
	}
	if len(pos)%3 != 0 || len(idx) != faces*3 {
		t.Fatalf("data sizes pos=%d idx=%d faces=%d", len(pos), len(idx), faces)
	}
	vc := len(pos) / 3
	for _, ix := range idx {
		if ix < 0 || ix >= vc {
			t.Fatalf("index %d out of range [0,%d)", ix, vc)
		}
	}
	for k := 0; k < len(idx); k += 3 {
		if idx[k] == idx[k+1] || idx[k+1] == idx[k+2] ||
			idx[k] == idx[k+2] {
			t.Fatal("degenerate triangle in output")
		}
	}

	out := filepath.Join(t.TempDir(), "out.obj")
	if err := m.WriteOBJ(out); err != nil {
		t.Fatalf("WriteOBJ: %v", err)
	}
	m2, err := LoadOBJ(out)
	if err != nil {
		t.Fatalf("reload output: %v", err)
	}
	defer m2.Close()
	if _, f2 := m2.Counts(); f2 != faces {
		t.Fatalf("round-trip faces = %d, want %d", f2, faces)
	}
}

func TestContextCancellationReleasesTask(t *testing.T) {
	path := writeGridOBJ(t, 120)
	m, err := LoadOBJ(path)
	if err != nil {
		t.Fatal(err)
	}
	defer m.Close()

	// Pre-cancel: Run must abort at its first checkpoint even though the
	// mesh is large enough to keep running. The progress callback records
	// invocations; context cancellation (not the callback) drives abort.
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var sawProgress atomic.Int32
	task, err := NewTask(m, 0.01, WithProgress(
		func(faces, total int) bool {
			sawProgress.Add(1)
			return false
		}))
	if err != nil {
		t.Fatal(err)
	}
	err = task.Run(ctx)
	if err == nil {
		t.Fatal("expected cancellation error, got nil")
	}
	var e *Error
	if !errors.As(err, &e) || e.Code != libErrCancelled {
		t.Fatalf("want libErrCancelled, got %v", err)
	}
	if err := task.Close(); err != nil {
		t.Fatalf("double close: %v", err)
	}
	func() {
		task.mu.Lock()
		defer task.mu.Unlock()
		if task.ptr != 0 {
			t.Fatal("native task handle not cleared after cancellation")
		}
	}()
	if sawProgress.Load() == 0 {
		t.Fatal("progress callback was never invoked")
	}

	// The mesh survives cancellation, so a fresh task is possible.
	task2, err := NewTask(m, 0.5)
	if err != nil {
		t.Fatalf("retry after cancel: %v", err)
	}
	if err := task2.Run(context.Background()); err != nil {
		t.Fatalf("retry run: %v", err)
	}
	task2.Close()
}

func TestProgressCallbackCancels(t *testing.T) {
	path := writeGridOBJ(t, 50)
	m, err := LoadOBJ(path)
	if err != nil {
		t.Fatal(err)
	}
	defer m.Close()

	var n atomic.Int32
	task, err := NewTask(m, 0.05, WithProgress(
		func(faces, total int) bool {
			return n.Add(1) >= 3
		}))
	if err != nil {
		t.Fatal(err)
	}
	err = task.Run(context.Background())
	var e *Error
	if !errors.As(err, &e) || e.Code != libErrCancelled {
		t.Fatalf("want cancellation, got %v", err)
	}
	task.Close()
}

func TestInvalidRatio(t *testing.T) {
	path := writeGridOBJ(t, 5)
	m, err := LoadOBJ(path)
	if err != nil {
		t.Fatal(err)
	}
	defer m.Close()
	for _, r := range []float32{0, -0.5, 1.01, 2} {
		if _, err := NewTask(m, r); err == nil {
			t.Fatalf("ratio %v should be rejected", r)
		}
	}
}

func TestSetDataAndErrors(t *testing.T) {
	if _, err := LoadOBJ(filepath.Join(t.TempDir(), "missing.obj")); err == nil {
		t.Fatal("missing file should fail")
	}
	m, err := NewMesh()
	if err != nil {
		t.Fatal(err)
	}
	defer m.Close()
	pos := []float32{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}
	idx := []int{0, 1, 2, 1, 3, 2}
	if err := m.SetData(pos, idx); err != nil {
		t.Fatalf("SetData: %v", err)
	}
	if v, f := m.Counts(); v != 4 || f != 2 {
		t.Fatalf("counts = %d,%d", v, f)
	}
	// ratio 1 keeps the mesh unchanged
	task, err := NewTask(m, 1)
	if err != nil {
		t.Fatal(err)
	}
	if err := task.Run(context.Background()); err != nil {
		t.Fatal(err)
	}
	task.Close()
	if _, f := m.Counts(); f != 2 {
		t.Fatalf("ratio=1 changed face count to %d", f)
	}
}

func TestFinalizerReclaimsNative(t *testing.T) {
	path := writeGridOBJ(t, 10)
	// Create and abandon tasks without Close; under repeated GC the
	// finalizers must run and the process must remain healthy.
	for i := 0; i < 50; i++ {
		m, err := LoadOBJ(path)
		if err != nil {
			t.Fatal(err)
		}
		task, err := NewTask(m, 0.3)
		if err != nil {
			t.Fatal(err)
		}
		if err := task.Run(context.Background()); err != nil {
			t.Fatal(err)
		}
		// intentionally drop both references
		_ = m
		_ = task
		if i%10 == 0 {
			runtime.GC()
			time.Sleep(time.Millisecond)
		}
	}
	runtime.GC()
}

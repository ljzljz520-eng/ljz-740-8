package mesh

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
)

// 在测试二进制的 meshlib/build 中找不到库时，回退到仓库默认路径。
func testLibrary(t *testing.T) *Library {
	t.Helper()
	candidates := []string{
		os.Getenv("MESHLIB_PATH"),
		filepath.Join("..", "meshlib", "build"),
		"meshlib/build",
	}
	for _, dir := range candidates {
		if dir == "" {
			continue
		}
		lib, err := LoadLibrary(dir)
		if err == nil {
			t.Cleanup(func() { _ = lib.Close() })
			return lib
		}
	}
	t.Skip("meshlib shared library not built; run `make -C meshlib`")
	return nil
}

func writeGrid(t *testing.T, path string, n int) {
	t.Helper()
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	for j := 0; j <= n; j++ {
		for i := 0; i <= n; i++ {
			fmt.Fprintf(f, "v %d %d 0\n", i, j)
		}
	}
	for j := 0; j < n; j++ {
		for i := 0; i < n; i++ {
			a := j*(n+1) + i + 1
			b := a + 1
			c := a + (n + 1)
			d := c + 1
			fmt.Fprintf(f, "f %d %d %d\n", a, b, d)
			fmt.Fprintf(f, "f %d %d %d\n", a, d, c)
		}
	}
}

func TestSimplifyRatio(t *testing.T) {
	lib := testLibrary(t)
	dir := t.TempDir()
	in := filepath.Join(dir, "grid.obj")
	out := filepath.Join(dir, "grid_small.obj")
	writeGrid(t, in, 50)
	faces0 := 50 * 50 * 2

	s, err := lib.OpenModel(in)
	if err != nil {
		t.Fatal(err)
	}
	if s.Faces() != faces0 {
		t.Fatalf("faces = %d, want %d", s.Faces(), faces0)
	}

	var maxP atomic.Int32
	s.SetProgress(func(p int) bool {
		if int32(p) > maxP.Load() {
			maxP.Store(int32(p))
		}
		return false
	})
	if err := s.SetRatio(0.1); err != nil {
		t.Fatal(err)
	}
	if err := s.Simplify(); err != nil {
		t.Fatalf("simplify: %v", err)
	}
	if f := s.Faces(); f > faces0/10+2 {
		t.Fatalf("faces after simplify = %d, want ~%d", f, faces0/10)
	}
	if maxP.Load() != 100 {
		t.Fatalf("progress never reached 100, max=%d", maxP.Load())
	}
	if err := s.Export(out); err != nil {
		t.Fatal(err)
	}
	if fi, err := os.Stat(out); err != nil || fi.Size() == 0 {
		t.Fatal("output file missing or empty")
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	// 重复 Close 安全
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
}

func TestCancelByCallback(t *testing.T) {
	lib := testLibrary(t)
	dir := t.TempDir()
	in := filepath.Join(dir, "grid.obj")
	writeGrid(t, in, 70)

	s, err := lib.OpenModel(in)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	faces0 := s.Faces()

	var calls int32
	s.SetProgress(func(p int) bool {
		atomic.AddInt32(&calls, 1)
		return p >= 20 // 在 20% 处取消
	})
	_ = s.SetRatio(0.02)

	err = s.Simplify()
	if !errors.Is(err, ErrCanceled) {
		t.Fatalf("want ErrCanceled, got %v", err)
	}
	if atomic.LoadInt32(&calls) < 2 {
		t.Fatalf("progress callback called %d times", calls)
	}
	// 取消后网格为一致的中间结果，仍可导出
	out := filepath.Join(dir, "partial.obj")
	if err := s.Export(out); err != nil {
		t.Fatalf("export after cancel: %v", err)
	}
	if s.Faces() >= faces0 || s.Faces() <= 0 {
		t.Fatalf("unexpected face count after cancel: %d", s.Faces())
	}
}

func TestCancelByContext(t *testing.T) {
	lib := testLibrary(t)
	dir := t.TempDir()
	in := filepath.Join(dir, "grid.obj")
	writeGrid(t, in, 70)

	s, err := lib.OpenModel(in)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	_ = s.SetRatio(0.02)

	ctx, cancel := context.WithCancel(context.Background())
	cancel() // 预先取消：第一次回调即生效
	err = s.SimplifyContext(ctx)
	if !errors.Is(err, ErrCanceled) {
		t.Fatalf("want ErrCanceled, got %v", err)
	}
}

func TestRatioValidation(t *testing.T) {
	lib := testLibrary(t)
	dir := t.TempDir()
	in := filepath.Join(dir, "grid.obj")
	writeGrid(t, in, 4)
	s, err := lib.OpenModel(in)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	for _, r := range []float64{0, -0.1, 1.5} {
		if err := s.SetRatio(r); err == nil {
			t.Fatalf("ratio %v should be rejected", r)
		}
	}
}

func TestLoadMissing(t *testing.T) {
	lib := testLibrary(t)
	if _, err := lib.OpenModel(filepath.Join(t.TempDir(), "no.obj")); err == nil {
		t.Fatal("expected error for missing file")
	}
}

func TestConcurrentSimplifiers(t *testing.T) {
	lib := testLibrary(t)
	var wg sync.WaitGroup
	for g := 0; g < 4; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			in := filepath.Join(t.TempDir(), "grid.obj")
			writeGrid(t, in, 30)
			s, err := lib.OpenModel(in)
			if err != nil {
				t.Error(err)
				return
			}
			defer s.Close()
			_ = s.SetRatio(0.3)
			s.SetProgress(func(p int) bool {
				if g%2 == 1 && p > 50 {
					return true // 一半任务中途取消
				}
				return false
			})
			err = s.Simplify()
			if g%2 == 1 {
				if !errors.Is(err, ErrCanceled) {
					t.Errorf("g%d want cancel, got %v", g, err)
				}
			} else if err != nil {
				t.Errorf("g%d simplify: %v", g, err)
			}
		}(g)
	}
	wg.Wait()
}

func TestVersion(t *testing.T) {
	lib := testLibrary(t)
	if v := lib.Version(); v == "" {
		t.Fatal("empty version")
	} else {
		t.Log("meshlib", v)
	}
}

package meshsimp_test

import (
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/solo-manager/meshsimp/meshsimp"
)

var (
	loadOnce sync.Once
	loadErr  error
)

func ensureLib(t *testing.T) {
	t.Helper()
	loadOnce.Do(func() {
		p := os.Getenv("MESHSIMP_LIB")
		if p == "" {
			p = filepath.Join("..", "native", "libmeshsimp.so")
		}
		abs, err := filepath.Abs(p)
		if err == nil {
			loadErr = meshsimp.LoadLibrary(abs)
		} else {
			loadErr = err
		}
	})
	if loadErr != nil {
		t.Skipf("native library unavailable (run `make native` first): %v", loadErr)
	}
}

// writeGridObj writes an (n+1)x(n+1) bumpy grid mesh: 2*n*n triangles.
func writeGridObj(t *testing.T, dir string, n int) string {
	t.Helper()
	path := filepath.Join(dir, fmt.Sprintf("grid_%d.obj", n))
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	for j := 0; j <= n; j++ {
		for i := 0; i <= n; i++ {
			x, y := float64(i)*0.1, float64(j)*0.1
			z := 0.3*math.Sin(2*x)*math.Cos(2*y) + 0.05*math.Sin(7*x+3*y)
			fmt.Fprintf(f, "v %f %f %f\n", x, y, z)
		}
	}
	for j := 0; j < n; j++ {
		for i := 0; i < n; i++ {
			a := j*(n+1) + i + 1
			b, c, d := a+1, a+(n+1), a+(n+1)+1
			fmt.Fprintf(f, "f %d %d %d\n", a, b, d)
			fmt.Fprintf(f, "f %d %d %d\n", a, d, c)
		}
	}
	return path
}

func newLoaded(t *testing.T, obj string, ratio float64) *meshsimp.Simplifier {
	t.Helper()
	s, err := meshsimp.New()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	if err := s.Load(obj); err != nil {
		t.Fatal(err)
	}
	if err := s.SetRatio(ratio); err != nil {
		t.Fatal(err)
	}
	return s
}

func TestSimplifyAndExport(t *testing.T) {
	ensureLib(t)
	dir := t.TempDir()
	obj := writeGridObj(t, dir, 40) // 1681 verts, 3200 tris

	s := newLoaded(t, obj, 0.25)

	var lastP float64
	calls := 0
	if err := s.Run(func(p float64) {
		calls++
		if p < lastP {
			t.Errorf("progress went backwards: %f -> %f", lastP, p)
		}
		lastP = p
	}); err != nil {
		t.Fatal(err)
	}
	if calls == 0 {
		t.Fatal("progress callback never invoked")
	}
	if lastP != 1.0 {
		t.Errorf("final progress = %v, want 1.0", lastP)
	}

	inV, inF, outV, outF := s.Counts()
	t.Logf("in: %d v / %d f -> out: %d v / %d f", inV, inF, outV, outF)
	if inV != 1681 || inF != 3200 {
		t.Errorf("unexpected input counts %d/%d", inV, inF)
	}
	if outF == 0 || outF >= inF {
		t.Errorf("output faces %d not in (0, %d)", outF, inF)
	}

	out := filepath.Join(dir, "out.obj")
	if err := s.Export(out); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	var vLines, fLines int
	for _, line := range splitLines(string(data)) {
		if len(line) > 2 && line[0] == 'v' && line[1] == ' ' {
			vLines++
		}
		if len(line) > 2 && line[0] == 'f' && line[1] == ' ' {
			fLines++
		}
	}
	if vLines != outV || fLines != outF {
		t.Errorf("exported file has %d v / %d f, want %d/%d", vLines, fLines, outV, outF)
	}
}

func TestCancelReclaimsResources(t *testing.T) {
	ensureLib(t)
	dir := t.TempDir()
	obj := writeGridObj(t, dir, 200) // 40401 verts, 80000 tris

	s := newLoaded(t, obj, 0.1)

	// Cancel from inside the first progress callback: deterministic.
	var once sync.Once
	err := s.Run(func(p float64) {
		once.Do(func() { s.Cancel() })
	})
	if !errors.Is(err, meshsimp.ErrCancelled) {
		t.Fatalf("Run = %v, want ErrCancelled", err)
	}

	// Partial result must be discarded: export has to fail now.
	if err := s.Export(filepath.Join(dir, "should_not_exist.obj")); err == nil {
		t.Fatal("Export succeeded after cancellation")
	}
	_, _, outV, outF := s.Counts()
	if outV != 0 || outF != 0 {
		t.Errorf("output not cleared after cancel: %d v / %d f", outV, outF)
	}

	// The context survives: a fresh Run must work again.
	if err := s.Run(nil); err != nil {
		t.Fatalf("re-run after cancel: %v", err)
	}
	if _, _, _, outF := s.Counts(); outF == 0 {
		t.Fatal("no output after re-run")
	}
}

func TestCancelBeforeRun(t *testing.T) {
	ensureLib(t)
	dir := t.TempDir()
	obj := writeGridObj(t, dir, 40)

	s := newLoaded(t, obj, 0.5)

	// A Cancel with no active Run is remembered and cancels the next Run.
	s.Cancel()
	if err := s.Run(nil); !errors.Is(err, meshsimp.ErrCancelled) {
		t.Fatalf("Run after Cancel = %v, want ErrCancelled", err)
	}

	// The pending cancel was consumed: the next Run works normally.
	if err := s.Run(nil); err != nil {
		t.Fatalf("second Run: %v", err)
	}
	if _, _, _, outF := s.Counts(); outF == 0 {
		t.Fatal("no output after second Run")
	}
}

func TestCloseWhileRunning(t *testing.T) {
	ensureLib(t)
	dir := t.TempDir()
	obj := writeGridObj(t, dir, 400) // 160801 verts, 320000 tris

	s, err := meshsimp.New()
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Load(obj); err != nil {
		t.Fatal(err)
	}
	s.SetRatio(0.05)

	runErr := make(chan error, 1)
	go func() { runErr <- s.Run(nil) }()

	// Give Run a moment to enter native code, then Close: this cancels
	// the task, waits for it, and frees the native context.
	time.Sleep(5 * time.Millisecond)
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-runErr:
		if err != nil && !errors.Is(err, meshsimp.ErrCancelled) {
			t.Fatalf("Run returned %v", err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("Run did not return after Close")
	}

	// Idempotent and safe after close.
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	s.Cancel() // must not panic or touch freed memory
	if err := s.Load(obj); !errors.Is(err, meshsimp.ErrClosed) {
		t.Errorf("Load after Close = %v, want ErrClosed", err)
	}
}

func TestErrors(t *testing.T) {
	ensureLib(t)
	dir := t.TempDir()

	s, err := meshsimp.New()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	if err := s.Load(filepath.Join(dir, "missing.obj")); err == nil {
		t.Error("Load of missing file succeeded")
	}
	if err := s.Run(nil); err == nil {
		t.Error("Run without a loaded model succeeded")
	}
	if err := s.Export(filepath.Join(dir, "x.obj")); err == nil {
		t.Error("Export without a result succeeded")
	}
}

func splitLines(s string) []string {
	var out []string
	start := 0
	for i := 0; i < len(s); i++ {
		if s[i] == '\n' {
			out = append(out, s[start:i])
			start = i + 1
		}
	}
	if start < len(s) {
		out = append(out, s[start:])
	}
	return out
}

// Command simplify reduces the triangle count of a Wavefront OBJ model using
// quadric-error-metric edge collapse and writes the decimated result.
//
// Usage:
//
//	simplify -in model.obj -out model_simple.obj -ratio 0.25
//
// Set MESH_LIBRARY_PATH to point at libmesh.so when it is not installed in
// one of the default search locations.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"
	"time"

	mesh "github.com/example/meshsimplify/meshsimplify"
)

func main() {
	in := flag.String("in", "", "input Wavefront OBJ file (required)")
	out := flag.String("out", "simplified.obj", "output OBJ file")
	ratio := flag.Float64("ratio", 0.25,
		"fraction of faces to keep, in (0,1]")
	lib := flag.String("lib", os.Getenv("MESH_LIBRARY_PATH"),
		"path to libmesh shared library (optional)")
	flag.Parse()

	if *in == "" {
		fmt.Fprintln(os.Stderr, "missing -in; see -h")
		os.Exit(2)
	}
	if *lib != "" {
		if err := mesh.LoadLibrary(*lib); err != nil {
			fail("load library", err)
		}
	}

	if err := run(*in, *out, float32(*ratio)); err != nil {
		fail("simplify", err)
	}
}

func run(in, out string, ratio float32) error {
	m, err := mesh.LoadOBJ(in)
	if err != nil {
		return err
	}
	defer m.Close()

	iv, ifc := m.Counts()
	fmt.Printf("loaded %s: %d vertices, %d faces\n", in, iv, ifc)

	// Ctrl-C cancels the native run; deferred Close still reclaims memory.
	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	start := time.Now()
	task, err := mesh.NewTask(m, ratio, mesh.WithProgress(
		func(faces, total int) bool {
			fmt.Fprintf(os.Stderr, "\rprogress: %d/%d faces (%.1f%%)",
				faces, total,
				100.0*float64(total-faces)/float64(total))
			return false
		}))
	if err != nil {
		return err
	}
	defer task.Close()

	if err := task.Run(ctx); err != nil {
		fmt.Fprintln(os.Stderr) // leave the progress line
		return err
	}
	fmt.Fprintf(os.Stderr, "\rdone in %s%s\n",
		time.Since(start).Round(time.Millisecond),
		repeatSpaces(20))

	if err := m.WriteOBJ(out); err != nil {
		return err
	}
	ov, ofc := m.Counts()
	fmt.Printf("wrote %s: %d vertices, %d faces (%.1f%% of input)\n",
		out, ov, ofc, 100.0*float64(ofc)/float64(ifc))
	return nil
}

func fail(op string, err error) {
	fmt.Fprintf(os.Stderr, "error: %s: %v\n", op, err)
	os.Exit(1)
}

func repeatSpaces(n int) string {
	b := make([]byte, n)
	for i := range b {
		b[i] = ' '
	}
	return string(b)
}

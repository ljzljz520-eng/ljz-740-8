// Minimal in-process example: load, simplify with a progress callback and
// export. Run after building the native library:
//
//	make lib
//	MESH_LIBRARY_PATH=$PWD/internal/libmesh/libmesh.so \
//	  go run ./examples/basic model.obj out.obj 0.2
package main

import (
	"context"
	"fmt"
	"os"
	"strconv"

	mesh "github.com/example/meshsimplify/meshsimplify"
)

func main() {
	if len(os.Args) != 4 {
		fmt.Fprintln(os.Stderr,
			"usage: basic <input.obj> <output.obj> <ratio>")
		os.Exit(2)
	}
	in, out := os.Args[1], os.Args[2]
	ratio, err := strconv.ParseFloat(os.Args[3], 32)
	if err != nil || ratio <= 0 || ratio > 1 {
		fmt.Fprintln(os.Stderr, "ratio must be in (0,1]")
		os.Exit(2)
	}

	m, err := mesh.LoadOBJ(in)
	must(err)
	defer m.Close()

	task, err := mesh.NewTask(m, float32(ratio), mesh.WithProgress(
		func(faces, total int) bool {
			fmt.Fprintf(os.Stderr, "\r%5d / %d faces", faces, total)
			return false
		}))
	must(err)
	defer task.Close()

	must(task.Run(context.Background()))
	fmt.Fprintln(os.Stderr)
	must(m.WriteOBJ(out))

	_, f := m.Counts()
	fmt.Printf("simplified %s -> %s (%d faces)\n", in, out, f)
}

func must(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

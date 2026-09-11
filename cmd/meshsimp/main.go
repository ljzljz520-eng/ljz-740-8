// Command meshsimp simplifies an OBJ mesh and saves the result.
//
//	meshsimp -lib native/libmeshsimp.so -in model.obj -out small.obj -ratio 0.5
//
// Press Ctrl+C while it runs: the native task is cancelled, its
// resources are reclaimed, and the command exits with code 2.
package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"github.com/solo-manager/meshsimp/meshsimp"
)

func main() {
	lib := flag.String("lib", "native/libmeshsimp.so", "path to the native libmeshsimp shared library")
	in := flag.String("in", "", "input OBJ file (required)")
	out := flag.String("out", "", "output OBJ file (required)")
	ratio := flag.Float64("ratio", 0.5, "target triangle ratio in (0,1]")
	flag.Parse()

	if *in == "" || *out == "" {
		flag.Usage()
		os.Exit(1)
	}

	if err := meshsimp.LoadLibrary(*lib); err != nil {
		fatal(err)
	}

	s, err := meshsimp.New()
	if err != nil {
		fatal(err)
	}
	// Releases the native context and every buffer it owns.
	defer s.Close()

	// Ctrl+C / SIGTERM -> cancel the native task (also while loading).
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	done := make(chan struct{})
	defer close(done)
	defer signal.Stop(sigCh)
	go func() {
		select {
		case <-sigCh:
			fmt.Fprintln(os.Stderr, "\ncancel requested, reclaiming resources...")
			s.Cancel()
		case <-done:
		}
	}()

	if err := s.Load(*in); err != nil {
		if errors.Is(err, meshsimp.ErrCancelled) {
			fmt.Fprintln(os.Stderr, "cancelled while loading: native resources reclaimed")
			os.Exit(2)
		}
		fatal(err)
	}
	if err := s.SetRatio(*ratio); err != nil {
		fatal(err)
	}

	inV, inF, _, _ := s.Counts()
	fmt.Printf("loaded %s: %d vertices, %d triangles (target ratio %.2f)\n",
		*in, inV, inF, *ratio)

	err = s.Run(progressBar)
	fmt.Fprintln(os.Stderr) // finish the progress line

	if errors.Is(err, meshsimp.ErrCancelled) {
		fmt.Fprintln(os.Stderr, "cancelled: native resources reclaimed, nothing exported")
		os.Exit(2)
	}
	if err != nil {
		fatal(err)
	}

	if err := s.Export(*out); err != nil {
		fatal(err)
	}
	_, _, outV, outF := s.Counts()
	fmt.Printf("saved %s: %d vertices, %d triangles (%.1f%% of input)\n",
		*out, outV, outF, 100*float64(outF)/float64(inF))
}

func progressBar(p float64) {
	const width = 40
	filled := int(p * width)
	if filled > width {
		filled = width
	}
	fmt.Fprintf(os.Stderr, "\r[%s%s] %5.1f%%",
		strings.Repeat("#", filled), strings.Repeat("-", width-filled), p*100)
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "error:", err)
	os.Exit(1)
}

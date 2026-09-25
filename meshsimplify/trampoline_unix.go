//go:build !windows

package meshsimplify

import "github.com/ebitengine/purego"

// puregoCallback turns a Go function matching the native
// int (*)(void*, int, int) ABI into a C function pointer. The C ints are
// passed in full 64-bit integer registers, so the Go signature uses int64
// (a 4-byte int32 would be unpacked from half a slot and misalign the
// following arguments on arm64).
func puregoCallback(fn func(arg uintptr, faces, total int64) int32) uintptr {
	return purego.NewCallback(fn)
}

//go:build windows

package meshsimplify

import "github.com/ebitengine/purego"

// On Windows, C int arguments still occupy full register slots seen from Go;
// use 64-bit integers so purego unpacks every argument from its own slot.
func puregoCallback(fn func(arg uintptr, faces, total int64) int32) uintptr {
	return purego.NewCallback(fn)
}

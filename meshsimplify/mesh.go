// Package meshsimplify binds a native 3D triangle-mesh decimation library
// (loaded at runtime with github.com/ebitengine/purego, so no cgo toolchain
// is needed) and exposes an idiomatic Go API:
//
//   - Load a model (Wavefront OBJ) or build a mesh from vertex/face slices.
//   - Create a Task with a reduction ratio and an optional progress
//     callback.
//   - Run the task, honoring context cancellation; export the result.
//
// Native heap allocations (meshes and tasks) are always released: explicitly
// through Close, automatically via runtime finalisers as a safety net, and
// promptly when a run is cancelled.
package meshsimplify

import (
	"runtime"
	"sync"
	"unsafe"
)

// Mesh is an in-memory triangle mesh backed by a native lm_mesh.
//
// A Mesh is mutated in place by a Task and is not safe for concurrent use;
// reload or clone it to run several simplifications independently.
type Mesh struct {
	ptr uintptr // native lm_mesh*; 0 after Close
	mu  sync.Mutex
}

// LoadOBJ reads a Wavefront OBJ file. Polygonal faces are triangulated with
// a fan; UVs, normals and material references are ignored.
func LoadOBJ(path string) (*Mesh, error) {
	n, err := loaded()
	if err != nil {
		return nil, err
	}
	var code int32
	p := n.meshLoadObj(path, &code)
	if p == 0 {
		return nil, libErr("load obj", code)
	}
	m := &Mesh{ptr: p}
	runtime.SetFinalizer(m, finalizeMesh)
	return m, nil
}

// NewMesh creates an empty mesh; fill it with SetData.
func NewMesh() (*Mesh, error) {
	n, err := loaded()
	if err != nil {
		return nil, err
	}
	p := n.meshNew()
	if p == 0 {
		return nil, &Error{Code: libErrMemory, Op: "new mesh"}
	}
	m := &Mesh{ptr: p}
	runtime.SetFinalizer(m, finalizeMesh)
	return m, nil
}

// SetData replaces mesh contents. positions holds 3 floats per vertex
// (xyz); indices holds 3 zero-based vertex indices per triangle.
func (m *Mesh) SetData(positions []float32, indices []int) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.ptr == 0 {
		return &Error{Code: libErrInvalid, Op: "set data"}
	}
	n, err := loaded()
	if err != nil {
		return err
	}
	vc, fc := len(positions)/3, len(indices)/3
	if vc == 0 || fc == 0 || len(positions)%3 != 0 || len(indices)%3 != 0 {
		return &Error{Code: libErrInvalid, Op: "set data"}
	}
	ci := make([]int32, len(indices))
	for i, v := range indices {
		if v < 0 || v >= vc {
			return &Error{Code: libErrInvalid, Op: "set data"}
		}
		ci[i] = int32(v)
	}
	code := n.meshSetData(m.ptr,
		uintptr(unsafe.Pointer(&positions[0])), int32(vc),
		uintptr(unsafe.Pointer(&ci[0])), int32(fc))
	return libErr("set data", code)
}

// Counts returns live vertex and triangle counts. After a successful
// simplification these describe the decimated mesh.
func (m *Mesh) Counts() (vertices, faces int) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.ptr == 0 {
		return 0, 0
	}
	n, _ := loaded()
	return int(n.meshAliveV(m.ptr)), int(n.meshAliveF(m.ptr))
}

// Data returns compacted, zero-based geometry: 3 floats per surviving
// vertex and 3 indices per surviving face.
func (m *Mesh) Data() (positions []float32, indices []int, err error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.ptr == 0 {
		return nil, nil, &Error{Code: libErrInvalid, Op: "get data"}
	}
	n, err := loaded()
	if err != nil {
		return nil, nil, err
	}
	vc, fc := int(n.meshAliveV(m.ptr)), int(n.meshAliveF(m.ptr))
	positions = make([]float32, vc*3)
	idx32 := make([]int32, fc*3)
	if vc > 0 && fc > 0 {
		code := n.meshGetData(m.ptr,
			uintptr(unsafe.Pointer(&positions[0])), int32(vc),
			uintptr(unsafe.Pointer(&idx32[0])), int32(fc))
		if e := libErr("get data", code); e != nil {
			return nil, nil, e
		}
	}
	indices = make([]int, len(idx32))
	for i, v := range idx32 {
		indices[i] = int(v)
	}
	return positions, indices, nil
}

// WriteOBJ writes the current (possibly simplified) mesh to path.
func (m *Mesh) WriteOBJ(path string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.ptr == 0 {
		return &Error{Code: libErrInvalid, Op: "write obj"}
	}
	n, err := loaded()
	if err != nil {
		return err
	}
	return libErr("write obj", n.meshWriteObj(m.ptr, path))
}

// Close releases the native mesh. Safe to call multiple times.
func (m *Mesh) Close() error {
	m.mu.Lock()
	defer m.mu.Unlock()
	return releaseMesh(m)
}

func releaseMesh(m *Mesh) error {
	if m.ptr == 0 {
		return nil
	}
	if n, err := loaded(); err == nil {
		n.meshFree(m.ptr)
	}
	m.ptr = 0
	runtime.SetFinalizer(m, nil)
	return nil
}

func finalizeMesh(m *Mesh) { _ = releaseMesh(m) }

package meshsimplify

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sync"

	"github.com/ebitengine/purego"
)

// native holds dynamically resolved symbols of libmesh.
//
// Opaque C objects are carried as uintptr handles. purego converts Go
// strings to NUL-terminated C strings automatically for char* parameters.
type native struct {
	handle uintptr

	meshLoadObj   func(path string, errOut *int32) uintptr
	meshNew       func() uintptr
	meshSetData   func(m uintptr, verts uintptr, vcount int32, idx uintptr, fcount int32) int32
	meshFree      func(m uintptr)
	meshVCount    func(m uintptr) int32
	meshFCount    func(m uintptr) int32
	meshAliveV    func(m uintptr) int32
	meshAliveF    func(m uintptr) int32
	meshWriteObj  func(m uintptr, path string) int32
	meshGetData   func(m uintptr, verts uintptr, vcap int32, idx uintptr, fcap int32) int32
	taskNew       func(m uintptr, ratio float32, errOut *int32) uintptr
	taskSetProgCB func(t, fn, arg uintptr)
	taskCancel    func(t uintptr)
	taskRun       func(t uintptr) int32
	taskFree      func(t uintptr)
	version       func() uintptr
}

var (
	loadOnce sync.Once
	lib      *native
	loadErr  error
)

// DefaultLibraryNames returns the platform-specific file names accepted when
// no explicit library path is supplied.
func DefaultLibraryNames() []string {
	switch runtime.GOOS {
	case "darwin":
		return []string{"libmesh.dylib", "mesh.dylib"}
	case "windows":
		return []string{"mesh.dll", "libmesh.dll"}
	default:
		return []string{"libmesh.so"}
	}
}

func searchPaths(explicit string) []string {
	names := DefaultLibraryNames()
	if explicit != "" {
		names = []string{filepath.Base(explicit)}
	}
	var dirs []string
	if dir := os.Getenv("MESH_LIBRARY_PATH"); dir != "" {
		dirs = append(dirs, dir)
	}
	if exe, err := os.Executable(); err == nil {
		d := filepath.Dir(exe)
		dirs = append(dirs,
			filepath.Join(d, "lib"),
			filepath.Join(d, "internal", "libmesh"),
			filepath.Join(d, "..", "..", "internal", "libmesh"))
	}
	if explicit != "" {
		dirs = append([]string{filepath.Dir(explicit)}, dirs...)
	}
	dirs = append(dirs, ".", "internal/libmesh", "lib",
		"/usr/local/lib", "/usr/lib", "/usr/lib64")

	seen := map[string]bool{}
	var paths []string
	add := func(p string) {
		if p != "" && !seen[p] {
			seen[p] = true
			paths = append(paths, p)
		}
	}
	for _, n := range names {
		add(n) // let the dynamic loader search its own paths
	}
	for _, d := range dirs {
		for _, n := range names {
			add(filepath.Join(d, n))
		}
	}
	return paths
}

// LoadLibrary explicitly loads the mesh library from path and registers it
// process-wide. Pass "" to search default locations (the MESH_LIBRARY_PATH
// env var, the executable directory, ./internal/libmesh and system paths).
// The first successful load wins.
func LoadLibrary(path string) error {
	loadOnce.Do(func() { loadErr = doLoad(path) })
	return loadErr
}

func doLoad(explicit string) error {
	var lastErr error
	for _, p := range searchPaths(explicit) {
		h, err := purego.Dlopen(p, purego.RTLD_NOW|purego.RTLD_GLOBAL)
		if err != nil {
			lastErr = err
			continue
		}
		n := &native{handle: h}
		bindErr := func() (err error) {
			// RegisterLibFunc panics when a symbol is missing.
			defer func() {
				if r := recover(); r != nil {
					err = fmt.Errorf("%v", r)
				}
			}()
			purego.RegisterLibFunc(&n.meshLoadObj, h, "lm_mesh_load_obj")
			purego.RegisterLibFunc(&n.meshNew, h, "lm_mesh_new")
			purego.RegisterLibFunc(&n.meshSetData, h, "lm_mesh_set_data")
			purego.RegisterLibFunc(&n.meshFree, h, "lm_mesh_free")
			purego.RegisterLibFunc(&n.meshVCount, h, "lm_mesh_vertex_count")
			purego.RegisterLibFunc(&n.meshFCount, h, "lm_mesh_face_count")
			purego.RegisterLibFunc(&n.meshAliveV, h, "lm_mesh_alive_vertex_count")
			purego.RegisterLibFunc(&n.meshAliveF, h, "lm_mesh_alive_face_count")
			purego.RegisterLibFunc(&n.meshWriteObj, h, "lm_mesh_write_obj")
			purego.RegisterLibFunc(&n.meshGetData, h, "lm_mesh_get_data")
			purego.RegisterLibFunc(&n.taskNew, h, "lm_task_new")
			purego.RegisterLibFunc(&n.taskSetProgCB, h,
				"lm_task_set_progress_cb")
			purego.RegisterLibFunc(&n.taskCancel, h,
				"lm_task_request_cancel")
			purego.RegisterLibFunc(&n.taskRun, h, "lm_task_run")
			purego.RegisterLibFunc(&n.taskFree, h, "lm_task_free")
			purego.RegisterLibFunc(&n.version, h, "lm_version")
			return nil
		}()
		if bindErr != nil {
			_ = purego.Dlclose(h)
			lastErr = bindErr
			continue
		}
		lib = n
		return nil
	}
	return fmt.Errorf("meshsimplify: cannot locate native mesh library "+
		"(build it or set MESH_LIBRARY_PATH): %w", lastErr)
}

func loaded() (*native, error) {
	if err := LoadLibrary(""); err != nil {
		return nil, err
	}
	return lib, nil
}

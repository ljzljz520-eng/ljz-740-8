//go:build windows

package mesh

import (
	"fmt"
	"path/filepath"
	"unsafe"

	"golang.org/x/sys/windows"
)

func openOS(path string) (uintptr, error) {
	if !filepath.IsAbs(path) {
		if abs, err := filepath.Abs(path); err == nil {
			path = abs
		}
	}
	h, err := windows.LoadLibrary(path)
	if err != nil {
		return nil, err
	}
	return uintptr(h), nil
}

func dlsym(h uintptr, name string) (uintptr, error) {
	addr, err := windows.GetProcAddress(windows.Handle(h), name)
	if err != nil {
		return 0, fmt.Errorf("GetProcAddress %s: %w", name, err)
	}
	return addr, nil
}

func (l *library) unloadOS() error {
	if l.handle == 0 {
		return nil
	}
	if err := windows.FreeLibrary(windows.Handle(l.handle)); err != nil {
		return err
	}
	l.handle = 0
	return nil
}

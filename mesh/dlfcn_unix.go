//go:build !windows

package mesh

import (
	"github.com/ebitengine/purego"
)

func openOS(path string) (uintptr, error) {
	return purego.Dlopen(path, purego.RTLD_NOW|purego.RTLD_LOCAL)
}

func dlsym(h uintptr, name string) (uintptr, error) {
	return purego.Dlsym(h, name)
}

func (l *library) unloadOS() error {
	if l.handle == 0 {
		return nil
	}
	purego.Dlclose(l.handle)
	l.handle = 0
	return nil
}

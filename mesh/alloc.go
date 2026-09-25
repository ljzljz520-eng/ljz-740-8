package mesh

import (
	"unsafe"
)

// 包级分配器在库加载后绑定到库导出的 meshlib_calloc/meshlib_cfree，
// 从而保证 Go 侧分配、C 侧释放（或反过来）使用同一个 CRT 堆。
var (
	nativeCalloc func(n, size uintptr) unsafe.Pointer
	nativeFree   func(p unsafe.Pointer)
)

func allocBytes(n uintptr) unsafe.Pointer {
	return nativeCalloc(1, n)
}

func freeBytes(p unsafe.Pointer) {
	if p != nil {
		nativeFree(p)
	}
}

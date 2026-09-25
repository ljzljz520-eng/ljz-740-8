package mesh

import "unsafe"

// cBytes 复制 Go 字符串为以 NUL 结尾的字节块。
// 内存由 meshlib 所在 CRT 分配，调用结束后立即释放（C 侧只读）。
func cBytes(s string) (ptr unsafe.Pointer, freeFn func()) {
	b := append([]byte(s), 0)
	p := allocBytes(uintptr(len(b)))
	dst := unsafe.Slice((*byte)(p), len(b))
	copy(dst, b)
	return p, func() { freeBytes(p) }
}

// cString 将 NUL 结尾的 C 字符串复制为 Go 字符串；ptr 为 0 返回 ""。
func cString(ptr unsafe.Pointer) string {
	if ptr == nil {
		return ""
	}
	var n int
	for *(*byte)(unsafe.Add(ptr, n)) != 0 {
		n++
	}
	if n == 0 {
		return ""
	}
	return string(unsafe.Slice((*byte)(ptr), n))
}

func freeCString(ptr unsafe.Pointer) { freeBytes(ptr) }

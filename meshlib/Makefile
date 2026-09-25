# meshlib 本地共享库构建
#
#   make            构建当前平台共享库到 build/
#   make static     构建静态库（调试用）
#   make clean      清理构建产物
#   make check      额外以 AddressSanitizer 构建并运行自测
CC      = gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -fPIC -fvisibility=hidden
LDFLAGS ?= -lm

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifeq ($(UNAME_S),Darwin)
  SHLIB := build/libmeshlib.dylib
  SHFLAGS := -shared
else ifeq ($(UNAME_S),Linux)
  SHLIB := build/libmeshlib.so
  SHFLAGS := -shared
else
  SHLIB := build/meshlib.dll
  SHFLAGS := -shared -Wl,--out-implib,build/libmeshlib.a
endif

# mingw 交叉编译示例： make CC=x86_64-w64-mingw32-gcc
ifneq ($(findstring w64-mingw32,$(CC)),)
  SHLIB := build/meshlib.dll
  SHFLAGS := -shared -Wl,--out-implib,build/libmeshlib.a
endif

.PHONY: all clean static check

all: $(SHLIB)

$(SHLIB): meshlib.c meshlib.h | build
	$(CC) $(CFLAGS) $(SHFLAGS) -o $@ meshlib.c $(LDFLAGS)

static: build/libmeshlib.a

build/libmeshlib.a: meshlib.c meshlib.h | build
	$(CC) $(CFLAGS) -c -o build/meshlib.o meshlib.c
	ar rcs $@ build/meshlib.o

build:
	mkdir -p build

check: | build
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined \
		meshlib.c selftest.c -o build/meshlib_selftest $(LDFLAGS)
	cd . && ./build/meshlib_selftest

clean:
	rm -rf build

# Build targets for the native mesh library and the Go binding/examples.
#
#   make            build libmesh shared library
#   make test       build library + run Go tests (CGO-free)
#   make example    build the simplify CLI into ./bin/
#   make all        library + example
#   make clean      remove build artefacts

GO      ?= go
CC      ?= cc
AR      ?= ar

SRC_DIR   := internal/libmesh
LIB_BASE  := libmesh
SOURCES   := $(SRC_DIR)/mesh.c

# Platform-specific shared library name
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifeq ($(UNAME_S),Darwin)
  SHLIB := $(SRC_DIR)/$(LIB_BASE).dylib
  SHFLAGS := -dynamiclib
else ifneq ($(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S))$(findstring CYGWIN,$(UNAME_S)),)
  SHLIB := $(SRC_DIR)/$(LIB_BASE).dll
  SHFLAGS := -shared
else
  SHLIB := $(SRC_DIR)/$(LIB_BASE).so
  SHFLAGS := -shared
endif

CFLAGS  ?= -O2 -fPIC -Wall -Wextra
LDFLAGS := -lm

.PHONY: all lib test example vet fmt clean

all: lib example

lib: $(SHLIB)

$(SHLIB): $(SOURCES) $(SRC_DIR)/mesh.h
	$(CC) $(CFLAGS) $(SHFLAGS) $(SOURCES) -o $(SHLIB) $(LDFLAGS)

test: lib
	MESH_LIBRARY_PATH=$(abspath $(SHLIB)) CGO_ENABLED=0 $(GO) test -count=1 ./...

vet: lib
	CGO_ENABLED=0 $(GO) vet ./...

fmt:
	$(GO) fmt ./...

example: lib
	mkdir -p bin
	MESH_LIBRARY_PATH=$(abspath $(SHLIB)) CGO_ENABLED=0 $(GO) build -o bin/simplify ./cmd/simplify

clean:
	rm -f $(SRC_DIR)/$(LIB_BASE).so $(SRC_DIR)/$(LIB_BASE).dylib \
	      $(SRC_DIR)/$(LIB_BASE).dll bin/simplify

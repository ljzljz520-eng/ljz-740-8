GO      ?= go
NATIVE  := native/libmeshsimp.so
# purego requires cgo disabled for its callback implementation.
export CGO_ENABLED=0

.PHONY: all native build test run clean

all: native build

native: $(NATIVE)

$(NATIVE): native/meshsimp.c native/meshsimp.h
	$(MAKE) -C native

build: native
	$(GO) build ./...

test: native
	$(GO) test ./...

# Demo: simplify the bundled sample mesh.
run: native
	$(GO) run ./cmd/meshsimp -lib $(NATIVE) \
		-in testdata/grid.obj -out /tmp/grid_simple.obj -ratio 0.25

clean:
	$(MAKE) -C native clean
	$(GO) clean ./...

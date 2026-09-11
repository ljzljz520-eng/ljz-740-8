# Note: plain "cc" may be a non-compiler wrapper in some environments,
# so default to gcc (override with: make CC=clang).
CC      = gcc
CFLAGS  ?= -O2 -fPIC -Wall -Wextra -std=c11
LDFLAGS ?= -shared

libmeshsimp.so: meshsimp.c meshsimp.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ meshsimp.c -lm

clean:
	rm -f libmeshsimp.so

.PHONY: clean

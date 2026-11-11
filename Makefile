CC      ?= cc
PREFIX  ?= /usr/local
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Wpedantic -fno-strict-aliasing
CPPFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
LDFLAGS ?=

all: mtool

mtool: mtool.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Native static build (uses the host libc; may need glibc-static).
static: mtool.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -static -o mtool.static $< $(LDFLAGS)

# Reproducible static builds for x86_64 (musl), aarch64 and armv7.
# Requires musl-gcc and the corresponding cross toolchains; see README.
static-all:
	sh scripts/build-static.sh all

install: mtool
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 mtool $(DESTDIR)$(PREFIX)/bin/mtool

check: mtool
	sh tests/test_mtool.sh ./mtool

check-static:
	sh tests/test_mtool.sh dist/mtool.x86_64

clean:
	rm -f mtool mtool.static

distclean: clean
	rm -rf dist

.PHONY: all static static-all install check check-static clean distclean
# unpin-vfs -- local dev build (links nix's libzstd). The shipped per-package
# build instead vendors zstd's decompress-only single file; see README.
#
#   make            build packer + tests (needs zstd via pkg-config or ZSTD_*)
#   make check      run the in-memory round-trip validation
#   make e2e DIR=…  pack DIR, read it all back, byte-compare to originals
#
# zstd discovery: pkg-config if available, else set ZSTD_CFLAGS / ZSTD_LIBS.
ZSTD_CFLAGS ?= $(shell pkg-config --cflags libzstd 2>/dev/null)
ZSTD_LIBS   ?= $(shell pkg-config --libs   libzstd 2>/dev/null || echo -lzstd)

CFLAGS  ?= -O2 -Wall -Wextra
CPPFLAGS += -DMINIZ_USE_ZSTD -Isrc $(ZSTD_CFLAGS)
LDLIBS  += $(ZSTD_LIBS)

CORE = src/miniz.c src/unpin_zstd.c

all: unpin-vfs-pack unpack-verify roundtrip

unpin-vfs-pack: tools/unpin-vfs-pack.c $(CORE)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDLIBS)

unpack-verify: test/unpack-verify.c $(CORE)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDLIBS)

roundtrip: test/roundtrip.c $(CORE)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDLIBS)

check: roundtrip
	./roundtrip /tmp/unpin-vfs-check.zip

# integration test for the UNPIN_VFS_DIRS superset (packs a tree, links --wrap)
dircheck:
	ZSTD_CFLAGS="$(ZSTD_CFLAGS)" ZSTD_LIBS="$(ZSTD_LIBS)" CC="$(CC)" test/dir-test.sh

# integration test for the self-EOF mode (no blob: ZIP appended to the binary)
selfcheck:
	ZSTD_CFLAGS="$(ZSTD_CFLAGS)" ZSTD_LIBS="$(ZSTD_LIBS)" CC="$(CC)" test/self-test.sh

# integration test for --carry: rebuild a cosmo APE's tail-ZIP, keeping its
# entries verbatim while adding our zstd metadata (and dropping .symtab.amd64)
carrycheck:
	ZSTD_CFLAGS="$(ZSTD_CFLAGS)" ZSTD_LIBS="$(ZSTD_LIBS)" CC="$(CC)" test/carry-test.sh

# Validate the shipped runtime path: pack a tree (with a raw shared dict) using
# the libzstd packer, then read it back with a decode-only verifier built from
# the VENDORED zstddeclib.c -- no -lzstd, no zstd runtime closure at all.
vendorcheck: unpin-vfs-pack
	rm -rf /tmp/unpin-vfs-vtree && mkdir -p /tmp/unpin-vfs-vtree/sub
	for i in $$(seq 1 24); do \
	  yes "package Foo$$i; use strict; use warnings; 1; # redundant payload" \
	    | head -40 > /tmp/unpin-vfs-vtree/mod$$i.pm; done
	cp /tmp/unpin-vfs-vtree/mod1.pm /tmp/unpin-vfs-vtree/sub/nested.pm
	cat /tmp/unpin-vfs-vtree/mod*.pm > /tmp/unpin-vfs-vdict     # raw content dict
	./unpin-vfs-pack /tmp/unpin-vfs-v.zip /tmp/unpin-vfs-vtree --dict /tmp/unpin-vfs-vdict
	$(CC) $(CFLAGS) -DMINIZ_USE_ZSTD -DUNPIN_ZSTD_VENDORED -Isrc \
	    test/unpack-verify.c src/miniz.c src/unpin_zstd.c -o unpack-verify-vendored
	./unpack-verify-vendored /tmp/unpin-vfs-v.zip /tmp/unpin-vfs-vtree

# end-to-end: pack DIR (optionally DICT=path) and verify byte-for-byte
e2e: unpin-vfs-pack unpack-verify
	@test -n "$(DIR)" || { echo "usage: make e2e DIR=<tree> [DICT=<dict>]"; exit 2; }
	./unpin-vfs-pack /tmp/unpin-vfs-e2e.zip "$(DIR)" $(if $(DICT),--dict $(DICT),)
	./unpack-verify /tmp/unpin-vfs-e2e.zip "$(DIR)"

clean:
	rm -f unpin-vfs-pack unpack-verify roundtrip unpack-verify-vendored

.PHONY: all check dircheck selfcheck carrycheck vendorcheck e2e clean

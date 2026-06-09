# unpin-vfs

[![CI](https://github.com/unpins/unpin-vfs/actions/workflows/ci.yml/badge.svg)](https://github.com/unpins/unpin-vfs/actions)

One reusable virtual-filesystem core for single self-contained binaries that
embed a runtime tree — an interpreter's library path, a compiler's sysroot, an
editor's runtime files — as a ZIP blob compiled into the executable. A matched
path under the mount root (`/zip/` by default) is inflated on demand and handed
to the program as a real, seekable fd; everything else falls through to libc
untouched.

The program needs no source changes: its file calls are intercepted
transparently — `ld --wrap` on Linux, symbol redefinition on macOS, a path
marker on Windows. The container is a structurally standard `.zip`, optionally
taught **Zstandard** for a smaller blob.

## Container format

A structurally standard `.zip` — any tool (`unzip -l`, `zipinfo`, python
`zipfile`) lists it. Entries use either:

- **DEFLATE** (method 8) — the status quo, read by stock miniz; or
- **Zstandard** (method 93) — when built with `-DMINIZ_USE_ZSTD`. The PKWARE
  APPNOTE 6.3.7 method; non-zstd tools list the entries but can't decompress them.

An optional **shared zstd dictionary** (from `zstd --train`) rides in the blob as
the reserved **STORED** entry `.unpin/zdict` and is auto-loaded at init. It is the
only non-interoperable bit: a vanilla zstd-zip reader sees dictionary-ID frames it
lacks the dict for. Omit `--dict` for fully interoperable output.

### Validated results

Measured end-to-end on a real 65 MB runtime tree (4867 files), packed then read
back with **byte-for-byte** comparison against the originals (0 mismatches):

| blob | size | vs current `zip -9` (17.43 MB) |
|---|---:|---:|
| `zip -9` (current, deflate) | 17.43 MB | — |
| zstd-in-zip, no dict (interoperable) | 15.97 MB | −8.4% |
| zstd-in-zip + shared dict | 12.79 MB | −26.6% |

## Layout

```
src/miniz.{c,h}      vendored miniz, patched for zstd method 93 (read + write)
src/zstddeclib.c     vendored zstd decompress-only amalgamation (-DUNPIN_ZSTD_VENDORED)
src/unpin_zstd.{c,h} thin zstd shim -- the ONLY TU that pulls in zstd (libzstd or the above)
src/vfs.{c,h}        the VFS core: open/stat/lstat/access, 3 OS backends, dict auto-load
                     + opendir/readdir/closedir/fopen superset under -DUNPIN_VFS_DIRS
tools/unpin-vfs-pack.c  build-time packer: directory tree -> zstd-in-zip blob
test/roundtrip.c     in-memory write+read validation (mixed zstd/deflate/stored + dict)
test/unpack-verify.c  read a blob back, CRC-check or byte-compare to a source tree
test/dir-fopen.c + dir-test.sh  integration test for the DIRS superset (links --wrap)
Makefile             local dev build (check / dircheck / vendorcheck / e2e)
```

### The miniz patch (small, additive)

- `MZ_ZSTD_METHOD` (93) accepted at the reader method gate, only under
  `-DMINIZ_USE_ZSTD`.
- Decode branch in `mz_zip_reader_extract_to_mem_no_alloc1`: a method-93 entry is
  one contiguous frame, so it decodes in **one shot** via the shim (no streaming).
  The VFS always reads an in-memory blob, so the frame is already mapped.
- Writer: `MZ_ZIP_FLAG_ZSTD_DATA` labels caller-supplied pre-compressed bytes as
  method 93 instead of deflate (reuses the existing `MZ_ZIP_FLAG_COMPRESSED_DATA`
  path). The deflate path is untouched — old deflate blobs still read.

## Using it in a package

Build time — produce the blob from a staged tree, then embed it:

```sh
# optional: train a shared dict over the tree first
zstd --train $(find stage -type f) -o stage.dict --maxdict=112640
unpin-vfs-pack incblob stage --dict stage.dict      # -> a .zip named "incblob"
$CC -c blob.S -o incblob.o     # blob.S: .incbin "incblob" between start/end labels
                               # (ELF: _binary_incblob_{start,end}; Mach-O/PE: bare)
```

Link time (transparent `--wrap` style — no source edits):

```
$CC ... vfs.o miniz.o unpin_zstd.o incblob.o \
    -Wl,--wrap=open,--wrap=stat,--wrap=lstat,--wrap=access
# 32-bit musl (i686/armv7l): also --wrap=__stat_time64,--wrap=__lstat_time64
#   and compile vfs.c with -DUNPIN_WRAP_TIME64
# macOS: no --wrap; objcopy --redefine-sym _open=_unpinvfs_open (etc.) on the
#   program's archives, leaving this TU calling the real libc.
# Windows: --wrap=win32_open,win32_stat,win32_lstat,win32_access (mingw).
```

Code paths `--wrap` can't reach can call `unpin_vfs_open/stat/lstat/access`
directly (same core).

### Runtime zstd, dependency-free

The dev `Makefile` and the build-time packer link the system `libzstd` (they
need to compress). The shipped single-binary instead builds `unpin_zstd.c` with
`-DUNPIN_ZSTD_VENDORED`, which compiles the vendored `src/zstddeclib.c` — zstd's
**decompress-only** amalgamation (`combine.py` over zstd 1.5.7's
`build/single_file_libs`) — straight into the one zstd TU. The runtime then
carries zstd with no shared-object closure, and the compress helpers compile out.

`make vendorcheck` exercises exactly this path: it packs a tree (with a shared
dict) using `libzstd`, then reads it back with a verifier built
`-DUNPIN_ZSTD_VENDORED` that links no `libzstd` at all.

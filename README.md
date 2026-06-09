# unpin-vfs

[![CI](https://github.com/unpins/unpin-vfs/actions/workflows/ci.yml/badge.svg)](https://github.com/unpins/unpin-vfs/actions)

One reusable virtual-filesystem core for unpin single-binaries that embed a
runtime tree — perl/biber `@INC`, python stdlib, tcc sysroot, vim runtime — as a
ZIP blob compiled into the executable. A matched path under the mount root
(`/zip/` by default) is inflated on demand and handed to the program as a real,
seekable fd; everything else falls through to libc untouched.

This code began copy-pasted across the single-binary packages that embed a tree.
**vim** and **gvim** now build on this consolidated core; `perl`, `biber`,
`python`, and `tcc` still carry their own copy and are being migrated onto it. It
also teaches the container **Zstandard** while staying a standard `.zip`.

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

Measured end-to-end on the real 65 MB biber `@INC` blob (4867 files), packed then
read back with **byte-for-byte** comparison against the originals (0 mismatches):

| blob | size | vs current `zip -9` (17.43 MB) |
|---|---:|---:|
| `zip -9` (current, deflate) | 17.43 MB | — |
| zstd-in-zip, no dict (interoperable) | 15.97 MB | −8.4% |
| zstd-in-zip + shared dict | 12.79 MB | −26.6% |

(Independent of compression: ~half of a perl-family blob is POD/`.pod`
documentation — stripping it is a larger, orthogonal lever, deferred.)

## Layout

```
src/miniz.{c,h}     vendored miniz, patched for zstd method 93 (read + write)
src/unpin_zstd.{c,h} thin zstd shim — the ONLY file that includes <zstd.h>
src/vfs.{c,h}       the VFS core: open/stat/lstat/access, 3 OS backends, dict auto-load
                    + opendir/readdir/closedir/fopen superset under -DUNPIN_VFS_DIRS
tools/unpin-vfs-pack.c  build-time packer: directory tree -> zstd-in-zip blob
test/roundtrip.c    in-memory write+read validation (mixed zstd/deflate/stored + dict)
test/unpack-verify.c  read a blob back, CRC-check or byte-compare to a source tree
test/dir-fopen.c + dir-test.sh  integration test for the DIRS superset (links --wrap)
Makefile            local dev build (links nix libzstd)
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

The dev `Makefile` links nix's `libzstd`. The shipped single-binary instead
vendors zstd's **decompress-only** single file (`zstddeclib.c`, generated from
zstd's `build/single_file_libs`) so there is no runtime closure — `unpin_zstd.c`
is the only thing that would change (its `<zstd.h>` calls map 1:1). The packer
keeps full `libzstd` (build host only).

## Status

- ✅ zstd-in-zip read + write through patched miniz — **validated** (`make check`),
  cross-checked against `unzip`/`zipinfo`/python `zipfile`.
- ✅ End-to-end pack → read-back byte-exact on the real biber tree, dict and no-dict
  (`make e2e DIR=… [DICT=…]`).
- ✅ `vfs.c` core (open/stat/lstat/access, Linux/macOS/Windows, dict auto-load) —
  compiles with and without zstd; generalised from the proven `vfs_miniz.c`.
- ✅ `readdir`/`opendir`/`closedir` + `fopen` + dir-aware `stat` superset
  (`-DUNPIN_VFS_DIRS`). Validated by `make dircheck` (packs a tree, links `--wrap`,
  checks listing / dir-stat / fopen / real-path fall-through).
- ✅ Windows marker mode (`-DUNPIN_VFS_WIN_MARKER`): for consumers with no `win32_*`
  layer to `--wrap` that canonicalise virtual paths to `C:\<marker>\…`, `is_virtual`
  matches the marker anywhere and the explicit `unpin_vfs_*` API materialises to a
  temp file and serves it from the CRT.
- ✅ **vim and gvim build on this core** — they dropped their bespoke `unpins_vfs.c`
  + `mch_*` macro hooks for this `-DUNPIN_VFS_DIRS` build (Linux `--wrap`, macOS
  `objcopy --redefine-sym`, Windows marker mode), validated across the full release
  matrix (6 Linux + macOS + Windows).
- ⬜ Vendor `zstddeclib.c` for a dependency-free runtime; `nix-lib` glue; migrate the
  remaining copies (`perl`, `biber`, `python`, `tcc`).

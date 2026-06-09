#!/usr/bin/env bash
# Integration test for the UNPIN_VFS_DIRS superset: pack a tree into the blob,
# link vfs.c with `ld --wrap`, and run test/dir-fopen.c against it.
#
#   ZSTD_CFLAGS=... ZSTD_LIBS=... test/dir-test.sh
set -euo pipefail
cd "$(dirname "$0")/.."

: "${CC:=cc}"
ZSTD_CFLAGS="${ZSTD_CFLAGS:-$(pkg-config --cflags libzstd 2>/dev/null || true)}"
ZSTD_LIBS="${ZSTD_LIBS:-$(pkg-config --libs libzstd 2>/dev/null || echo -lzstd)}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tree="$work/tree"

# 1. a small tree
mkdir -p "$tree/a/b" "$tree/a/c"
printf 'top\n'               > "$tree/top.txt"
printf 'hello from file1\n'  > "$tree/a/b/file1.txt"   # 17 bytes
printf 'file2\n'             > "$tree/a/b/file2.txt"
printf 'file3\n'             > "$tree/a/c/file3.txt"

# 2. pack it (zstd-in-zip)
$CC -O2 -DMINIZ_USE_ZSTD -Isrc $ZSTD_CFLAGS \
    tools/unpin-vfs-pack.c src/miniz.c src/unpin_zstd.c -o "$work/pack" $ZSTD_LIBS
"$work/pack" "$work/incblob" "$tree"

# 3. embed the blob as _binary_incblob_{start,end}
cat > "$work/blob.S" <<EOF
.section .rodata
.global _binary_incblob_start
.global _binary_incblob_end
_binary_incblob_start:
.incbin "$work/incblob"
_binary_incblob_end:
EOF
$CC -c "$work/blob.S" -o "$work/blob.o"

# 4. link the test, wrapping libc at the linker
WRAPS="-Wl,--wrap=open,--wrap=stat,--wrap=lstat,--wrap=access"
WRAPS="$WRAPS,--wrap=opendir,--wrap=readdir,--wrap=closedir,--wrap=fopen"
$CC -O2 -Wall -Wextra -DMINIZ_USE_ZSTD -DUNPIN_VFS_DIRS -Isrc $ZSTD_CFLAGS \
    test/dir-fopen.c src/vfs.c src/miniz.c src/unpin_zstd.c "$work/blob.o" \
    $WRAPS -o "$work/dir-fopen" $ZSTD_LIBS

# 5. run
"$work/dir-fopen"

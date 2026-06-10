#!/usr/bin/env bash
# Integration test for the self-EOF mode (-DUNPIN_VFS_SELF): build the test
# binary with NO blob, pack a tree as a file-adjusted (--base) ZIP that also
# carries unpin/* metadata entries, append it to the binary, and run it.
# Exercises exactly the shipped shape: one ZIP per binary at EOF, absolute
# offsets, the VFS serving the runtime tree while hiding unpin's namespaces.
#
#   ZSTD_CFLAGS=... ZSTD_LIBS=... test/self-test.sh
set -euo pipefail
cd "$(dirname "$0")/.."

: "${CC:=cc}"
ZSTD_CFLAGS="${ZSTD_CFLAGS:-$(pkg-config --cflags libzstd 2>/dev/null || true)}"
ZSTD_LIBS="${ZSTD_LIBS:-$(pkg-config --libs libzstd 2>/dev/null || echo -lzstd)}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tree="$work/tree"

# 1. the dir-fopen tree, plus unpin metadata entries the VFS must hide
mkdir -p "$tree/a/b" "$tree/a/c" "$tree/unpin/man"
printf 'top\n'               > "$tree/top.txt"
printf 'hello from file1\n'  > "$tree/a/b/file1.txt"   # 17 bytes
printf 'file2\n'             > "$tree/a/b/file2.txt"
printf 'file3\n'             > "$tree/a/c/file3.txt"
printf 'xxd\n'               > "$tree/unpin/aliases"
printf '.TH VFS-TEST 1\n'    > "$tree/unpin/man/vfs-test.1"

# 2. build the test binary in self mode: no blob.o, ZIP comes from its own EOF
WRAPS="-Wl,--wrap=open,--wrap=stat,--wrap=lstat,--wrap=access"
WRAPS="$WRAPS,--wrap=opendir,--wrap=readdir,--wrap=closedir,--wrap=fopen"
$CC -O2 -Wall -Wextra -DMINIZ_USE_ZSTD -DUNPIN_VFS_DIRS -DUNPIN_VFS_SELF \
    -Isrc $ZSTD_CFLAGS \
    test/dir-fopen.c src/vfs.c src/miniz.c src/unpin_zstd.c \
    $WRAPS -o "$work/self-fopen" $ZSTD_LIBS

# 3. no overlay yet: init must fail soft (virtual paths ENOENT, no crash)
"$work/self-fopen" >/dev/null 2>&1 && {
    echo "FAIL: expected failures before the overlay is appended"; exit 1; }

# 4. pack with absolute offsets (--base = binary size) and append
$CC -O2 -DMINIZ_USE_ZSTD -Isrc $ZSTD_CFLAGS \
    tools/unpin-vfs-pack.c src/miniz.c src/unpin_zstd.c -o "$work/pack" $ZSTD_LIBS
size=$(stat -c %s "$work/self-fopen" 2>/dev/null || stat -f %z "$work/self-fopen")
"$work/pack" "$work/overlay.zip" "$tree" --base "$size" --deflate unpin/aliases
cat "$work/overlay.zip" >> "$work/self-fopen"

# 5. the binary+ZIP must be one clean archive (sfx convention), if unzip exists
if command -v unzip >/dev/null 2>&1; then
    unzip -Z1 "$work/self-fopen" | grep -qx 'a/b/file1.txt' \
        || { echo "FAIL: unzip cannot read the appended overlay"; exit 1; }
fi

# 6. run: serves the tree from its own EOF, hides unpin/*
"$work/self-fopen"

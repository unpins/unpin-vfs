#!/usr/bin/env bash
# Integration test for the three BINDINGS (how a consumer's own libc calls reach
# the VFS): `ld --wrap`, symbol rename, and dlsym. test/binding-libc.c calls the
# bare libc names, so a binding that does not bind produces a run of failures
# rather than a link error -- which is the failure mode worth a test.
#
# Runs every binding the host can link: --wrap and rename need GNU ld/objcopy,
# dlsym needs neither and is what macOS ships (zsh).
#
#   ZSTD_CFLAGS=... ZSTD_LIBS=... test/binding-test.sh
set -euo pipefail
cd "$(dirname "$0")/.."

: "${CC:=cc}"
ZSTD_CFLAGS="${ZSTD_CFLAGS:-$(pkg-config --cflags libzstd 2>/dev/null || true)}"
ZSTD_LIBS="${ZSTD_LIBS:-$(pkg-config --libs libzstd 2>/dev/null || echo -lzstd)}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tree="$work/tree"

mkdir -p "$tree/a/b" "$tree/a/c"
printf 'top\n'               > "$tree/top.txt"
printf 'hello from file1\n'  > "$tree/a/b/file1.txt"   # 17 bytes
printf 'file2\n'             > "$tree/a/b/file2.txt"
printf 'file3\n'             > "$tree/a/c/file3.txt"

# the packer is the only piece that needs a real libzstd; the readers below are
# built -DUNPIN_ZSTD_VENDORED, exactly as the shipped binaries are
$CC -O2 -DMINIZ_USE_ZSTD -Isrc $ZSTD_CFLAGS \
    tools/unpin-vfs-pack.c src/miniz.c src/unpin_zstd.c -o "$work/pack" $ZSTD_LIBS

CORE="src/vfs.c src/miniz.c src/unpin_zstd.c"
CFLAGS="-O2 -Wall -Wextra -DMINIZ_USE_ZSTD -DUNPIN_ZSTD_VENDORED"
CFLAGS="$CFLAGS -DUNPIN_VFS_DIRS -DUNPIN_VFS_SELF -Isrc"

# self-EOF offsets are absolute, so the ZIP can only be packed once the binary
# it will be appended to has its final size
append_overlay() {
    local bin=$1 size
    size=$(stat -c %s "$bin" 2>/dev/null || stat -f %z "$bin")
    "$work/pack" "$work/overlay-$(basename "$bin").zip" "$tree" --base "$size"
    cat "$work/overlay-$(basename "$bin").zip" >> "$bin"
}

ran=0 failed=0
run() {
    ran=$((ran + 1))
    echo "== binding: $1 =="
    append_overlay "$2"
    "$2" || failed=$((failed + 1))
    echo
}

# 1. --wrap: GNU ld redirects the consumer's calls into __wrap_*
if echo 'int main(void){return 0;}' | $CC -Wl,--wrap=open -x c - -o "$work/probe" 2>/dev/null; then
    W="-Wl,--wrap=open,--wrap=stat,--wrap=lstat,--wrap=access"
    W="$W,--wrap=opendir,--wrap=readdir,--wrap=closedir,--wrap=fopen"
    # shellcheck disable=SC2086
    $CC $CFLAGS test/binding-libc.c $CORE $W -o "$work/bind-wrap"
    run "--wrap" "$work/bind-wrap"
else
    echo "== binding: --wrap == SKIPPED (linker has no --wrap)"; echo
fi

# 2. rename: the interceptors are named unpinvfs_*, and the CONSUMER's calls are
#    renamed onto them. The engine does this in IR; objcopy is the same edit on
#    an object file, and is what makes the binding testable here.
if command -v objcopy >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    $CC $CFLAGS -DUNPIN_VFS_NOWRAP -c test/binding-libc.c -o "$work/consumer.o"
    R=""
    for s in open stat lstat access opendir readdir closedir fopen; do
        R="$R --redefine-sym $s=unpinvfs_$s"
    done
    # shellcheck disable=SC2086
    objcopy $R "$work/consumer.o" "$work/consumer-renamed.o"
    # shellcheck disable=SC2086
    $CC $CFLAGS -DUNPIN_VFS_NOWRAP "$work/consumer-renamed.o" $CORE -o "$work/bind-rename"
    run "rename (objcopy)" "$work/bind-rename"
else
    echo "== binding: rename == SKIPPED (no objcopy)"; echo
fi

# 3. dlsym: vfs.c defines open/stat/... itself; the real ones come back through
#    RTLD_NEXT. No linker feature and no rename pass -- the macOS binding.
# shellcheck disable=SC2086
$CC $CFLAGS -DUNPIN_VFS_DLSYM test/binding-libc.c $CORE -o "$work/bind-dlsym" \
    $(uname -s | grep -qi linux && echo -ldl)
run "dlsym" "$work/bind-dlsym"

echo "$ran binding(s) run, $failed failed"
[ "$failed" -eq 0 ]

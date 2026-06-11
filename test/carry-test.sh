#!/usr/bin/env bash
# Integration test for `unpin-vfs-pack --carry`: rebuild an existing tail-ZIP
# (a Cosmopolitan APE's `/zip/` store) into a fresh archive that keeps those
# entries VERBATIM while adding our own as zstd. Models what nix-lib's embed
# does to a cosmo binary.
#
#   ZSTD_CFLAGS=... ZSTD_LIBS=... test/carry-test.sh
set -euo pipefail
cd "$(dirname "$0")/.."

: "${CC:=cc}"
ZSTD_CFLAGS="${ZSTD_CFLAGS:-$(pkg-config --cflags libzstd 2>/dev/null || true)}"
ZSTD_LIBS="${ZSTD_LIBS:-$(pkg-config --libs libzstd 2>/dev/null || echo -lzstd)}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fails=0
ok()   { printf '  ok   %s\n' "$1"; }
fail() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

# build the packer
# shellcheck disable=SC2086
$CC -O2 -Wall -Wextra -DMINIZ_USE_ZSTD -Isrc $ZSTD_CFLAGS \
    tools/unpin-vfs-pack.c src/miniz.c src/unpin_zstd.c -o "$work/pack" $ZSTD_LIBS

# 1. a fake cosmo APE: a 1000-byte "executable" + a DEFLATE tail-ZIP carrying
#    a `.cosmo` marker, a couple of zoneinfo files, and the `.symtab.amd64`
#    that the carry must drop.
head -c 1000 /dev/urandom > "$work/exe"
store="$work/store"
mkdir -p "$store/usr/share/zoneinfo"
printf 'COSMO-MARKER\n'            > "$store/.cosmo"
printf 'TZif-fake-utc-zoneinfo\n'  > "$store/usr/share/zoneinfo/UTC"
head -c 4096 /dev/urandom          > "$store/.symtab.amd64"
"$work/pack" "$work/store.zip" "$store" --base 1000 \
    --deflate .cosmo --deflate usr/share/zoneinfo/UTC --deflate .symtab.amd64 >/dev/null
cp "$work/exe" "$work/ape"; cat "$work/store.zip" >> "$work/ape"

# 2. carry it, adding our own zstd man + a deflate alias.
meta="$work/meta"
mkdir -p "$meta/unpin/man"
printf 'xxd\n'                      > "$meta/unpin/aliases"
printf '.TH FOO 1\nfoo bar baz\n'   > "$meta/unpin/man/foo.1"
base=$("$work/pack" "$work/out.zip" "$meta" --carry "$work/ape" --deflate unpin/aliases)
truncate -s "$base" "$work/ape"; cat "$work/out.zip" >> "$work/ape"

echo "== unpin-vfs-pack --carry =="

# base is where the carried ZIP began (== the 1000-byte exe).
[ "$base" = 1000 ] && ok "prints the carried ZIP's start offset ($base)" \
                   || fail "base offset wrong: $base (want 1000)"

# cosmo's store survives verbatim and still extracts.
[ "$(unzip -p "$work/ape" .cosmo)" = "COSMO-MARKER" ] \
  && ok "carried .cosmo extracts verbatim" || fail ".cosmo content changed"
[ "$(unzip -p "$work/ape" usr/share/zoneinfo/UTC)" = "TZif-fake-utc-zoneinfo" ] \
  && ok "carried zoneinfo extracts verbatim" || fail "zoneinfo content changed"

# the unused cosmo symbol table is gone.
unzip -Z1 "$work/ape" 2>/dev/null | grep -qx .symtab.amd64 \
  && fail ".symtab.amd64 was kept" || ok ".symtab.amd64 dropped"

# our metadata is present; man as zstd (method 93), alias still deflate-readable.
unzip -Z1 "$work/ape" 2>/dev/null | grep -qx unpin/man/foo.1 \
  && ok "our unpin/man entry is present" || fail "unpin/man entry missing"
unzip -v "$work/ape" 2>/dev/null | grep -q 'Unk:093 .*unpin/man/foo.1' \
  && ok "unpin/man stored as zstd (method 93)" || fail "unpin/man not method 93"

# the whole binary+ZIP is one clean SFX archive: every deflate/stored entry
# (everything but the one zstd man page) tests clean. `unzip -t` exits non-zero
# just for skipping the unsupported method-93 entry, so capture rather than pipe
# under `set -o pipefail`.
tout=$(unzip -t "$work/ape" 2>&1 || true)
case "$tout" in
  *"No errors detected"*) ok "binary+ZIP is one clean SFX archive" ;;
  *)                      fail "archive does not test clean" ;;
esac

echo
if [ "$fails" -eq 0 ]; then echo "PASSED (0 failures)"; else echo "FAILED ($fails)"; exit 1; fi

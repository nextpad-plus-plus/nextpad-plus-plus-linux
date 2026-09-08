#!/bin/bash
# Build a minimal STATIC libxml2 for the deb/rpm packaging builds.
#
# WHY (v1.1.1 launch day): the GUI links libxml2 for Paste with Hyperlinks
# (GAP-171, offline HTML parse). Ubuntu 26 took the libxml2 2.14 soname
# break — the `libxml2` package (and libxml2.so.2 itself) no longer exist
# there, so a deb built on 24.04 could not even resolve dependencies.
# Ubuntu's own libxml2.a is compiled against ICU, whose soname churns every
# release — linking it statically would smuggle the same problem back in
# via libicuXX. So: build our own, --without-icu (HTML parsing uses
# libxml2's built-in encoders + iconv from glibc), zlib/lzma off (clipboard
# HTML is never compressed), python/http off. MIT-licensed; the tarball is
# vendored and hash-pinned in vendor/.
#
# Output: prints the install PREFIX on stdout. Cached per version+arch —
# rebuilds only when missing.
set -euo pipefail
VER=2.12.10
SHA256=c3d8c0c34aa39098f66576fe51969db12a5100b956233dc56506f7a8679be995
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TARBALL="$ROOT/vendor/libxml2-$VER.tar.xz"
PREFIX="$HOME/.cache/npp-libxml2-static/$VER-$(uname -m)"

if [ -f "$PREFIX/lib/libxml2.a" ]; then echo "$PREFIX"; exit 0; fi

echo "── building static libxml2 $VER → $PREFIX" >&2
echo "$SHA256  $TARBALL" | sha256sum -c - >&2
WORK=$(mktemp -d /tmp/npp-libxml2-XXXXXX)
trap 'rm -rf "$WORK"' EXIT
tar -xf "$TARBALL" -C "$WORK"
cd "$WORK/libxml2-$VER"
./configure --prefix="$PREFIX" --disable-shared --enable-static \
    --without-icu --without-zlib --without-lzma --without-python \
    --without-http --without-debug --with-pic > /dev/null
make -j4 > /dev/null
make install > /dev/null
echo "$PREFIX"

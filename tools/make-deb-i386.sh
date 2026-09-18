#!/bin/bash
# Build the 32-bit x86 (i386) .deb inside a Debian BOOKWORM container.
#
# WHY: measured 32-bit demand exists (antiX-26's 386 ISO ≈ 37% of its x64
# downloads) and the app builds 32-bit-clean at zero code cost — so we ship
# the bare app (no plugins; the compile-time catalog gives 32-bit users an
# empty Plugins Admin) and let downloads reveal the market. Full analysis:
# docs/32bit_linux_investigation.md.
#
# WHY BOOKWORM, NOT TRIXIE: bookworm gives the lowest symbol floor
# (glibc 2.36, GTK 4.8 — the codebase's 4.6-tier guards hold there, proven
# by a full container build), so ONE artifact runs on every live 32-bit
# x86 Debian derivative: antiX-26/25 (trixie-generation), antiX-23,
# MX-23.x, Q4OS 5, Debian 12, and trixie i386 chroots.
#
# WHY A CONTAINER: i386 executes natively on any x86_64 CPU — full build
# speed, no emulation. This script therefore runs on the x86_64 build host
# (ubuntu24); the arm64 dev VM cannot run i386 containers at all.
#
# Output: dist/Nextpad++v<APP_VERSION>_i386.deb (via tools/make-deb.sh
# inside the container — dpkg reports i386, so naming and control arch are
# automatic; static libxml2 + static ncurses apply as on 64-bit).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

VER=$(sed -n 's/#define APP_VERSION *"\(.*\)"/\1/p' src/branding.h)
[ -n "$VER" ] || { echo "APP_VERSION not found in src/branding.h" >&2; exit 1; }
[ "$(uname -m)" = x86_64 ] || {
    echo "make-deb-i386.sh needs an x86_64 host (i386 runs natively there);" >&2
    echo "run it on the ubuntu24 build host, not this $(uname -m) machine." >&2
    exit 1; }
command -v podman >/dev/null 2>&1 || { echo "podman required" >&2; exit 1; }

# One-time builder image: bookworm i386 + the build dependencies.
IMAGE=nextpad-i386-build
if ! podman image exists "$IMAGE"; then
    echo "── building the $IMAGE container image (one-time)"
    printf '%s\n' \
      "FROM docker.io/i386/debian:bookworm" \
      "RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive \\" \
      "    apt-get install -y -qq build-essential cmake pkg-config \\" \
      "        libgtk-4-dev libglib2.0-dev libuchardet-dev libxml2-dev \\" \
      "        libncurses-dev file xz-utils && \\" \
      "    rm -rf /var/lib/apt/lists/*" \
    | podman build --platform linux/386 -t "$IMAGE" -f - "$ROOT" >/dev/null
fi

# Stage a clean copy. Scrubbing *.o/*.a/*.so is LOAD-BEARING: CMake's
# Scintilla custom target treats an existing bin/libscintilla.so as
# up-to-date and would link the foreign-arch one (undefined references
# against the wrong distro's GTK — see docs/32bit_linux_investigation.md).
STAGE="$HOME/npp-i386-build"
rsync -a --delete \
      --exclude='build*/' --exclude=dist/ --exclude=.git/ \
      --exclude='*.o' --exclude='*.a' --exclude='*.so' \
      --exclude=parts/ --exclude=stage/ --exclude=prime/ \
      "$ROOT/" "$STAGE/"

echo "── Building Nextpad++ $VER (i386, bookworm container)"
podman run --rm --platform linux/386 -v "$STAGE":/src "$IMAGE" \
    bash -c "cd /src && tools/make-deb.sh"

mkdir -p "$ROOT/dist"
cp "$STAGE/dist/Nextpad++v${VER}_i386.deb" "$ROOT/dist/"
echo "── dist/Nextpad++v${VER}_i386.deb"

#!/bin/bash
# Build the 32-bit ARM (armhf) .deb inside a Debian TRIXIE container.
#
# TARGET: Raspberry Pi OS 32-bit (trixie-based, the current release) and
# Debian 13 armhf — the largest 32-bit Linux population by device count
# (Pi 1 / Pi Zero v1 / original Pi 2 are hardware-locked to 32-bit ARM).
# See docs/32bit_linux_investigation.md.
#
# WHY TRIXIE, NOT BOOKWORM (unlike make-deb-i386.sh): Debian's 64-bit
# time_t transition changed the armhf ABI of 1200+ libraries WITHOUT
# soname bumps, so one binary cannot safely span the bookworm/trixie
# boundary on armhf. We build against trixie's t64 ABI for the current
# generation; the legacy (bookworm) Raspberry Pi OS 32-bit image is NOT
# covered.
#
# HOW IT RUNS: qemu-user emulation. armhf is a different instruction set —
# x86_64 hosts need qemu-user-static + binfmt (installed on ubuntu24),
# and the arm64 dev VM cannot run armhf AT ALL (Apple Silicon has no
# AArch32). Expect ~2 hours wall time (≈15x native); the build is fully
# unattended, so launch it detached (nohup) and check back.
#
# Output: dist/Nextpad++v<APP_VERSION>_armhf.deb via tools/make-deb.sh
# inside the container (dpkg reports armhf → naming/control automatic;
# static libxml2 + static ncurses as on every packaged build).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

VER=$(sed -n 's/#define APP_VERSION *"\(.*\)"/\1/p' src/branding.h)
[ -n "$VER" ] || { echo "APP_VERSION not found in src/branding.h" >&2; exit 1; }
command -v podman >/dev/null 2>&1 || { echo "podman required" >&2; exit 1; }
podman run --rm --platform linux/arm/v7 docker.io/library/debian:trixie true \
    >/dev/null 2>&1 || {
    echo "cannot run armhf containers here — need qemu-user-static + binfmt" >&2
    echo "(x86_64: apt install qemu-user-static; Apple-Silicon arm64: impossible," >&2
    echo " AArch32 is not implemented — use the ubuntu24 build host)" >&2
    exit 1; }

# One-time builder image: trixie armhf + the build dependencies.
IMAGE=nextpad-armhf-build
if ! podman image exists "$IMAGE"; then
    echo "── building the $IMAGE container image (one-time, emulated — slow)"
    printf '%s\n' \
      "FROM docker.io/library/debian:trixie" \
      "RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive \\" \
      "    apt-get install -y -qq build-essential cmake pkg-config \\" \
      "        libgtk-4-dev libglib2.0-dev libuchardet-dev libxml2-dev \\" \
      "        libncurses-dev file xz-utils && \\" \
      "    rm -rf /var/lib/apt/lists/*" \
    | podman build --platform linux/arm/v7 -t "$IMAGE" -f - "$ROOT" >/dev/null
fi

# Clean staging copy; scrubbing *.o/*.a/*.so is LOAD-BEARING (the Scintilla
# custom target would link a foreign-arch libscintilla.so — see the doc).
STAGE="$HOME/npp-armhf-build"
rsync -a --delete \
      --exclude='build*/' --exclude=dist/ --exclude=.git/ \
      --exclude='*.o' --exclude='*.a' --exclude='*.so' \
      --exclude=parts/ --exclude=stage/ --exclude=prime/ \
      "$ROOT/" "$STAGE/"

echo "── Building Nextpad++ $VER (armhf, trixie container, emulated ~2 h)"
# make-deb.sh's 500 MB/job self-cap is tuned for NATIVE compiles; qemu
# inflates per-process memory ~2-3x, so on a 4 GB host its pick would OOM.
# Default to the proven -j4; override with NPP_JOBS for bigger hosts.
podman run --rm --platform linux/arm/v7 -e NPP_JOBS="${NPP_JOBS:-4}" \
    -v "$STAGE":/src "$IMAGE" \
    bash -c "cd /src && tools/make-deb.sh"

mkdir -p "$ROOT/dist"
cp "$STAGE/dist/Nextpad++v${VER}_armhf.deb" "$ROOT/dist/"
echo "── dist/Nextpad++v${VER}_armhf.deb"

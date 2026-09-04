#!/bin/bash
# Build the installable Nextpad++ Arch Linux package (.pkg.tar.zst) —
# the Arch counterpart of make-deb.sh / make-rpm.sh.
#
#   tools/make-arch.sh                 # build for THIS machine's architecture
#   NPP_ARCH_REFRESH=1 tools/make-arch.sh   # re-pull Arch, rebuild builder image
#
# Output: dist/Nextpad++v<VER>_<debarch>.pkg.tar.zst, named like
# the .deb/.rpm so the download listing stays uniform. The metadata inside
# keeps the canonical Arch identity (nextpad-plus-plus, x86_64/aarch64).
#
# WHY A CONTAINER, unlike make-rpm.sh. The .rpm builds natively on Ubuntu
# because rpmbuild is packaged there and a BINARY rpm is just metadata over
# already-compiled files. Arch has no such escape hatch: makepkg and pacman
# are not packaged for Ubuntu, and — more importantly — an Arch package is
# expected to be linked against ARCH's libraries. Arch is rolling and runs
# far ahead of the build hosts (GCC 16 / GTK 4.22 / glibc 2.44 against
# Ubuntu 24.04's GCC 13 / GTK 4.14 / glibc 2.39), so the compile is a real
# portability test, not a repackaging step. Everything therefore happens
# inside archlinux:base-devel, and the host only needs podman.
#
# WHY THE SOURCE TREE IS COPIED AND FILTERED, never bind-mounted as-is.
# This is the load-bearing part of the script. The working tree carries the
# HOST's build products: ~45 .o files under scintilla/gtk4/ and a finished
# scintilla/bin/libscintilla.so. `make shared` sees those as up to date and
# does nothing, so a container build over the live tree would silently
# package the host's Ubuntu-built (and possibly foreign-architecture)
# Scintilla. The build-*/ dirs are equally poisonous — their CMakeCache
# pins this machine's absolute paths and compiler. Same excludes, and the
# same reasoning, as the rsync in release-all.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

VER=$(sed -n 's/#define APP_VERSION *"\(.*\)"/\1/p' src/branding.h)
[ -n "$VER" ] || { echo "APP_VERSION not found in src/branding.h" >&2; exit 1; }

OCI=${NPP_OCI:-podman}
command -v "$OCI" >/dev/null || {
    echo "$OCI not found — install it with:  sudo apt install podman" >&2
    echo "(no podman on this machine? build on one that has it, e.g. ubuntu24)" >&2
    exit 1; }

# Official Arch is x86_64 only. On aarch64 the same recipe builds against
# Arch Linux ARM, a separate distribution with its own images.
case "$(uname -m)" in
  x86_64)  BASE_IMAGE=docker.io/library/archlinux:base-devel; DEBARCH=amd64 ;;
  aarch64) BASE_IMAGE=${NPP_ARCH_IMAGE:-docker.io/menci/archlinuxarm:base-devel}; DEBARCH=arm64 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac
BUILDER_IMAGE=localhost/nextpad-arch-build:latest

# ── builder image ───────────────────────────────────────────────────────
# Cached so repeat builds skip the ~250 MB dependency download. Arch is
# rolling and partial upgrades are unsupported, so the image runs a full
# -Syu at creation; refresh it with NPP_ARCH_REFRESH=1 when it goes stale.
if [ -n "${NPP_ARCH_REFRESH:-}" ] || ! "$OCI" image exists "$BUILDER_IMAGE" 2>/dev/null; then
    echo "── Building the Arch builder image (one-off, a few minutes)"
    # Retagging leaves the PREVIOUS builder image dangling (~1.8 GB — real
    # space on the tight VMs), so note its id and delete it after a
    # successful rebuild.
    OLD_IMG=$("$OCI" image inspect --format '{{.Id}}' "$BUILDER_IMAGE" 2>/dev/null || true)
    "$OCI" pull "$BASE_IMAGE"
    # Empty build context on purpose: the Containerfile copies nothing, and
    # handing podman the repo root would upload the whole tree for nothing.
    CTX=$(mktemp -d /tmp/npp-arch-ctx-XXXXXX)
    trap 'rm -rf "$CTX"' EXIT
    printf '%s\n' \
      "FROM $BASE_IMAGE" \
      "# pacman >= 7 sandboxes downloads (Landlock + an unprivileged alpm" \
      "# user). Rootless podman cannot grant either inside the container, so" \
      "# -Syu dies with 'Landlock ruleset could not be applied'. Disable it." \
      "RUN sed -i '/^\\[options\\]/a DisableSandbox' /etc/pacman.conf" \
      "RUN pacman -Syu --noconfirm && \\" \
      "    pacman -S --needed --noconfirm base-devel cmake pkgconf \\" \
      "        gtk4 libadwaita glib2 glib2-devel uchardet libxml2 libx11 && \\" \
      "    pacman -Scc --noconfirm" \
      "# makepkg refuses to run as root, by design." \
      "RUN useradd -m builder" \
    | "$OCI" build -t "$BUILDER_IMAGE" -f - "$CTX" >/dev/null
    rm -rf "$CTX"; trap - EXIT
    NEW_IMG=$("$OCI" image inspect --format '{{.Id}}' "$BUILDER_IMAGE")
    if [ -n "$OLD_IMG" ] && [ "$OLD_IMG" != "$NEW_IMG" ]; then
        "$OCI" rmi "$OLD_IMG" >/dev/null 2>&1 || true
    fi
    echo "── Builder image ready: $BUILDER_IMAGE"
fi

# ── filtered source copy (see header) ───────────────────────────────────
STAGE=$(mktemp -d /tmp/npp-arch-XXXXXX)
trap 'rm -rf "$STAGE"' EXIT
SRCDIR="$STAGE/nextpad-plus-plus-$VER"

echo "── Staging a clean source copy"
rsync -a \
      --exclude='build/' --exclude='build-release/' --exclude='build-pkg/' \
      --exclude='build-arch/' --exclude='.git/' --exclude='dist/' \
      --exclude='tmp/' \
      --exclude='*.o' --exclude='*.a' --exclude='*.so' \
      --exclude='*.deb' --exclude='*.rpm' --exclude='*.pkg.tar.zst' \
      "$ROOT/" "$SRCDIR/"

# Sanity-check the filter rather than trusting it: a leaked libscintilla.so
# is exactly the failure this script exists to prevent, and it would be
# invisible in the finished package.
if [ -e "$SRCDIR/scintilla/bin/libscintilla.so" ] || \
   compgen -G "$SRCDIR/scintilla/gtk4/*.o" >/dev/null; then
    echo "ERROR: host build artifacts leaked into the source copy" >&2; exit 1
fi

tar -C "$STAGE" -czf "$STAGE/nextpad-plus-plus-$VER.tar.gz" "nextpad-plus-plus-$VER"
rm -rf "$SRCDIR"
cp packaging/PKGBUILD packaging/nextpad-plus-plus.install "$STAGE/"
sed -i "s/^pkgver=.*/pkgver=$VER/" "$STAGE/PKGBUILD"

# ── build ───────────────────────────────────────────────────────────────
echo "── Building Nextpad++ $VER in $BASE_IMAGE"
"$OCI" run --rm -v "$STAGE:/work" -w /work "$BUILDER_IMAGE" bash -c '
    chown -R builder:builder /work
    # --nodeps: dependencies are baked into the builder image, and makepkg
    # would otherwise try to install them as an unprivileged user.
    # PKGEXT: Arch proper defaults to .pkg.tar.zst but Arch Linux ARM still
    # ships makepkg.conf with .pkg.tar.xz — pin it so both architectures
    # produce the same format and the glob below finds the result.
    su builder -c "cd /work && PKGEXT=.pkg.tar.zst makepkg -f --noconfirm --nodeps --noprogressbar"
    rc=$?
    # Hand the tree back to the invoking user, on the failure path too.
    # Under ROOTLESS podman the host user maps to container root, so
    # anything left owned by builder lands on a subuid that the host can
    # neither read nor delete — the finished package included, and the
    # staging directory along with it (mktemp -d makes it mode 700, so the
    # host cannot even list it while the build owns it).
    chown -R root:root /work
    exit $rc
'

# Skip any -debug- sibling: the PKGBUILD suppresses it, but selecting on
# name rather than timestamp means a future options= change cannot silently
# ship a package containing nothing but debug symbols.
PKG=$(ls -t "$STAGE"/*.pkg.tar.zst 2>/dev/null | grep -v -- '-debug-' | head -1 || true)
[ -n "$PKG" ] || { echo "makepkg produced no package" >&2; exit 1; }

mkdir -p "$ROOT/dist"   # all packaging scripts write to dist/ (see make-deb.sh)
OUT="dist/Nextpad++v${VER}_${DEBARCH}.pkg.tar.zst"
cp "$PKG" "$ROOT/$OUT"
echo "── Built $OUT"

# ── report ──────────────────────────────────────────────────────────────
"$OCI" run --rm -v "$STAGE:/w:ro" "$BUILDER_IMAGE" \
    bash -c "pacman -Qip '/w/$(basename "$PKG")'" 2>/dev/null | sed -n '1,14p'

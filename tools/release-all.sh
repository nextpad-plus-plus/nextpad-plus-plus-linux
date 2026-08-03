#!/bin/bash
# Build every release artifact for every architecture, in one command.
#
#   tools/release-all.sh                  # local arch + the default remotes
#   tools/release-all.sh --local-only     # skip the remote hosts
#   NPP_REMOTES="ubuntu24 fedora-vm" tools/release-all.sh
#
# Produces, in dist/:
#   Nextpad++v<VER>_arm64.deb   nextpad-plus-plus-<VER>-1.aarch64.rpm   (this host)
#   Nextpad++v<VER>_amd64.deb   nextpad-plus-plus-<VER>-1.x86_64.rpm    (ubuntu24)
#
# WHY REMOTE HOSTS AND NOT CROSS-COMPILATION: each package embeds natively
# compiled binaries, so every architecture must be built on a machine of that
# architecture. The remotes are pure COMPILE TARGETS — all source lives and is
# edited here, is pushed to them by rsync, and nothing is ever authored there
# (they do not even have a git checkout).
#
# The rsync EXCLUDES are load-bearing, not cosmetic: build*/ dirs carry a
# CMakeCache pinned to this machine's absolute paths and compiler, and the
# vendored scintilla tree has in-tree .o files plus a libscintilla.so — all of
# this host's architecture. Copying them would break the remote build. What DOES
# travel is everything else including gitignored material, so the remote gets
# the vendored Scintilla with our patch series already applied and needs no
# patch step.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

VER=$(sed -n 's/#define APP_VERSION *"\(.*\)"/\1/p' src/branding.h)
[ -n "$VER" ] || { echo "APP_VERSION not found in src/branding.h" >&2; exit 1; }

REMOTES="${NPP_REMOTES:-ubuntu24}"
[ "${1:-}" = "--local-only" ] && REMOTES=""

DIST="$ROOT/dist"
mkdir -p "$DIST"
echo "══ Nextpad++ $VER — building all release artifacts into dist/"

# ── local architecture ──────────────────────────────────────────────────
echo
echo "══ LOCAL ($(uname -m))"
./tools/make-deb.sh > /tmp/npp-rel-deb.log 2>&1 \
    || { echo "  deb FAILED — see /tmp/npp-rel-deb.log"; tail -15 /tmp/npp-rel-deb.log; exit 1; }
echo "  ✓ $(ls -t Nextpad++v${VER}_*.deb | head -1)"
./tools/make-rpm.sh > /tmp/npp-rel-rpm.log 2>&1 \
    || { echo "  rpm FAILED — see /tmp/npp-rel-rpm.log"; tail -15 /tmp/npp-rel-rpm.log; exit 1; }
echo "  ✓ $(ls -t nextpad-plus-plus-${VER}-1.*.rpm | head -1)"
mv -f Nextpad++v${VER}_*.deb nextpad-plus-plus-${VER}-1.*.rpm "$DIST/" 2>/dev/null || true

# ── remote architectures ────────────────────────────────────────────────
REMOTE_PATH=/home/ubuntu/development/npp/nextpad-plus-plus-gtk4
for H in $REMOTES; do
    echo
    echo "══ REMOTE $H"
    if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$H" true 2>/dev/null; then
        echo "  ✗ unreachable (key auth?) — skipping"; continue
    fi
    echo "  → syncing source"
    ssh -o BatchMode=yes "$H" "mkdir -p $REMOTE_PATH"
    rsync -a --delete-after \
          --exclude='build/' --exclude='build-release/' --exclude='build-pkg/' \
          --exclude='.git/' --exclude='dist/' \
          --exclude='*.o' --exclude='*.a' --exclude='*.so' \
          --exclude='*.deb' --exclude='*.rpm' \
          "$ROOT/" "$H:$REMOTE_PATH/"

    echo "  → building (this takes a few minutes)"
    ssh -o BatchMode=yes "$H" "cd $REMOTE_PATH && \
        (cd scintilla/gtk4 && make shared -j4 >/tmp/npp-sci.log 2>&1) && \
        ./tools/make-deb.sh >/tmp/npp-deb.log 2>&1 && \
        ./tools/make-rpm.sh >/tmp/npp-rpm.log 2>&1" \
      || { echo "  ✗ build FAILED — logs on $H: /tmp/npp-{sci,deb,rpm}.log"
           ssh -o BatchMode=yes "$H" "tail -12 /tmp/npp-rpm.log /tmp/npp-deb.log 2>/dev/null" | tail -20
           continue; }

    echo "  → fetching artifacts"
    scp -q "$H:$REMOTE_PATH/Nextpad++v${VER}_*.deb" \
           "$H:$REMOTE_PATH/nextpad-plus-plus-${VER}-1.*.rpm" "$DIST/" 2>/dev/null || true
    ssh -o BatchMode=yes "$H" "rm -f $REMOTE_PATH/*.deb $REMOTE_PATH/*.rpm" 2>/dev/null || true
    echo "  ✓ done"
done

# ── summary ─────────────────────────────────────────────────────────────
echo
echo "══ dist/"
ls -1sh "$DIST" | tail -n +2 | sed 's/^/  /'
echo
echo "Verify before publishing:"
echo "  dpkg-deb -I dist/*.deb | head"
echo "  rpm -qp --requires dist/*.rpm | grep -v rpmlib"
echo "  podman run --rm -v \$PWD/dist:/d fedora:43 rpm -i --test /d/*.x86_64.rpm"
echo
echo "GitHub upload note: URL-encode '+' as %2B in the asset-name query,"
echo "or the '++' in the .deb filename becomes spaces."

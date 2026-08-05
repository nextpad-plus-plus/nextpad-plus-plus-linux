#!/bin/bash
# Build the signed static apt repository for Nextpad++ releases.
#
# Input:  dist/*.deb            (any mix of architectures/versions)
# Output: dist/apt/             (upload this tree verbatim to the web host,
#                                e.g. so it is reachable as https://nextpad.org/apt)
#
#   dist/apt/
#   ├── pool/main/n/nextpad-plus-plus/nextpad-plus-plus_<ver>_<arch>.deb
#   └── dists/stable/
#       ├── main/binary-<arch>/Packages{,.gz}
#       ├── Release            (apt-ftparchive, SHA256 index of the indexes)
#       ├── InRelease          (GPG clearsigned Release — what modern apt reads)
#       └── Release.gpg        (detached signature, for older apt)
#
# Signing key: the "Nextpad++ Release Signing" GPG key (see README.md for
# one-time generation). Select with NPP_SIGN_KEY=<key-id-or-uid>, default
# release@nextpad.org. The COMMITTED public half must match:
# packaging/apt-repo/nextpad-archive-keyring.gpg (shipped inside the .deb).
#
# Debs are renamed into the pool using the Debian convention
# (nextpad-plus-plus_1.1.0_amd64.deb) — among other things this avoids the
# documented '+ in URL' encoding trap the release-asset names hit.
set -euo pipefail
cd "$(dirname "$0")/../.."

SIGN_KEY="${NPP_SIGN_KEY:-release@nextpad.org}"
SUITE=stable
REPO=dist/apt

command -v apt-ftparchive >/dev/null || { echo "apt-ftparchive missing (apt install apt-utils)" >&2; exit 1; }
command -v gpg >/dev/null            || { echo "gpg missing" >&2; exit 1; }

shopt -s nullglob
DEBS=(dist/*.deb)
[ ${#DEBS[@]} -gt 0 ] || { echo "no .deb files in dist/ — build with tools/make-deb.sh first" >&2; exit 1; }

gpg --list-secret-keys "$SIGN_KEY" >/dev/null 2>&1 || {
    echo "signing key '$SIGN_KEY' not in the GPG keyring — see packaging/apt-repo/README.md" >&2
    exit 1
}
[ -f packaging/apt-repo/nextpad-archive-keyring.gpg ] || \
    echo "── NOTE: packaging/apt-repo/nextpad-archive-keyring.gpg is absent —" \
         "export it (README step 2) so the .deb can subscribe machines to this repo" >&2

# ── Pool ────────────────────────────────────────────────────────────────────
rm -rf "$REPO"
POOL="$REPO/pool/main/n/nextpad-plus-plus"
mkdir -p "$POOL"

ARCHES=()
for deb in "${DEBS[@]}"; do
    PKG=$(dpkg-deb -f "$deb" Package)
    VER=$(dpkg-deb -f "$deb" Version)
    ARCH=$(dpkg-deb -f "$deb" Architecture)
    [ "$PKG" = "nextpad-plus-plus" ] || { echo "skipping $deb (package '$PKG')" >&2; continue; }
    cp "$deb" "$POOL/${PKG}_${VER}_${ARCH}.deb"
    case " ${ARCHES[*]-} " in *" $ARCH "*) ;; *) ARCHES+=("$ARCH");; esac
    echo "── pooled ${PKG}_${VER}_${ARCH}.deb"
done
[ ${#ARCHES[@]} -gt 0 ] || { echo "no nextpad-plus-plus debs found" >&2; exit 1; }

# ── Indexes ─────────────────────────────────────────────────────────────────
for ARCH in "${ARCHES[@]}"; do
    BIN="$REPO/dists/$SUITE/main/binary-$ARCH"
    mkdir -p "$BIN"
    # Filename: fields must be relative to the repo root → run from $REPO.
    (cd "$REPO" && apt-ftparchive --arch "$ARCH" packages pool) > "$BIN/Packages"
    gzip -9 -c "$BIN/Packages" > "$BIN/Packages.gz"
done

# ── Release + signatures ────────────────────────────────────────────────────
apt-ftparchive \
    -o "APT::FTPArchive::Release::Origin=Nextpad++" \
    -o "APT::FTPArchive::Release::Label=Nextpad++" \
    -o "APT::FTPArchive::Release::Suite=$SUITE" \
    -o "APT::FTPArchive::Release::Codename=$SUITE" \
    -o "APT::FTPArchive::Release::Components=main" \
    -o "APT::FTPArchive::Release::Architectures=${ARCHES[*]}" \
    release "$REPO/dists/$SUITE" > "$REPO/dists/$SUITE/Release"

gpg --batch --yes -u "$SIGN_KEY" --digest-algo SHA256 \
    --clearsign -o "$REPO/dists/$SUITE/InRelease" "$REPO/dists/$SUITE/Release"
gpg --batch --yes -u "$SIGN_KEY" --digest-algo SHA256 \
    -abs -o "$REPO/dists/$SUITE/Release.gpg" "$REPO/dists/$SUITE/Release"

echo "── Signed repo ready: $REPO  (suite=$SUITE, arch: ${ARCHES[*]})"
echo "── Upload the tree so it is served at the URIs in packaging/apt-repo/nextpad.sources"

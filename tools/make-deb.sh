#!/bin/bash
# Build the installable Nextpad++ .deb.
#
# Codifies the release-packaging recipe (previously performed by hand):
#   fresh Release configure with the FINAL resource path baked in
#   (RESOURCES_DIR is used as a compile-time literal all over the codebase,
#   so a relocatable package MUST be built with the install path, not just
#   installed there), DESTDIR staging, hand-rolled DEBIAN metadata, dpkg-deb.
#
# Output: Nextpad++v<APP_VERSION>_<arch>.deb in the repo root.
# When uploading to a GitHub release, URL-encode '+' as %2B in the asset
# name query or it becomes a space.
set -euo pipefail
cd "$(dirname "$0")/.."

VER=$(sed -n 's/#define APP_VERSION *"\(.*\)"/\1/p' src/branding.h)
[ -n "$VER" ] || { echo "APP_VERSION not found in src/branding.h" >&2; exit 1; }
ARCH=$(dpkg --print-architecture)

# Memory-aware job cap. An unbounded parallel build once OOM-killed a whole
# desktop session (~122 lexilla TUs at ~50-60 MB per cc1plus), and the build
# VMs have as little as 1.9 GB RAM. Budget 500 MB per job, never exceed
# nproc. Override with NPP_JOBS=<n>.
if [ -n "${NPP_JOBS:-}" ]; then
    JOBS=$NPP_JOBS
else
    MEM_MB=$(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo)
    JOBS=$(nproc); CAP=$(( MEM_MB / 500 )); [ "$CAP" -lt 1 ] && CAP=1
    [ "$JOBS" -gt "$CAP" ] && JOBS=$CAP
fi

STAGE=$(mktemp -d /tmp/npp-pkg-XXXXXX)
trap 'rm -rf "$STAGE"' EXIT

echo "── Building Nextpad++ $VER ($ARCH, -j$JOBS)"
cmake -B build-pkg -S . \
      -DCMAKE_BUILD_TYPE=Release \
      -DNPP_RES_DIR=/usr/share/nextpad-plus-plus \
      -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-pkg -j"$JOBS"
DESTDIR="$STAGE" cmake --install build-pkg > /dev/null

# AppStream metadata — App Center / GNOME Software rich card for the
# installed app (and required for repo-catalog listings later).
install -Dm644 packaging/org.nextpad.NextpadPP.metainfo.xml \
    "$STAGE/usr/share/metainfo/org.nextpad.NextpadPP.metainfo.xml"

# APT-repo subscription (Chrome-style): ship the release public keyring +
# a sources.list.d entry so machines that installed once from the website
# receive signed updates via the normal update mechanism. Only when the
# committed public keyring exists (packaging/apt-repo/README.md step 2) —
# keyless builds still produce a valid standalone package.
SOURCES_CONFFILE=""
if [ -f packaging/apt-repo/nextpad-archive-keyring.gpg ]; then
    install -Dm644 packaging/apt-repo/nextpad-archive-keyring.gpg \
        "$STAGE/usr/share/keyrings/nextpad-archive-keyring.gpg"
    install -Dm644 packaging/apt-repo/nextpad.sources \
        "$STAGE/etc/apt/sources.list.d/nextpad.sources"
    SOURCES_CONFFILE="/etc/apt/sources.list.d/nextpad.sources"
    echo "── apt-repo subscription included (keyring + nextpad.sources)"
else
    echo "── NOTE: no packaging/apt-repo/nextpad-archive-keyring.gpg —" \
         "building WITHOUT the apt-repo subscription"
fi

mkdir -p "$STAGE/DEBIAN"

# The binary links gtk4/glib/uchardet/libstdc++ (NOT libadwaita — checked
# via ldd 2026-07-23); libscintilla.so ships inside the package itself.
# unzip: Plugins Admin archive extraction. git: the Git side panel.
cat > "$STAGE/DEBIAN/control" <<EOF
Package: nextpad-plus-plus
Version: $VER
Section: editors
Priority: optional
Architecture: $ARCH
Maintainer: Andrew Letov <aletik@gmail.com>
Depends: libgtk-4-1, libglib2.0-0t64, libuchardet0, libstdc++6, libc6
Recommends: unzip, git
Description: Multi-tab text editor (Linux port of Notepad++)
 Nextpad++ is a native Linux port of the Notepad++ text editor, built on
 Scintilla and Lexilla with GTK 4. Multi-tab editing, syntax highlighting
 for 50+ languages, user-defined languages, regex find/replace, macros,
 session save/restore, auto-backup, dockable side panels, API
 autocompletion with calltips, Plugins Admin, and a plugin SDK compatible
 with the Nextpad++ macOS port.
EOF

# /etc/apparmor.d/nextpad-plus-plus is admin-editable config — mark it a
# conffile so local edits survive upgrades. Same for the apt-repo
# subscription: deleting it is the supported "no more updates" opt-out and
# must survive upgrades (which arrive VIA that repo).
cat > "$STAGE/DEBIAN/conffiles" <<EOF
/etc/apparmor.d/nextpad-plus-plus
EOF
if [ -n "$SOURCES_CONFFILE" ]; then
    echo "$SOURCES_CONFFILE" >> "$STAGE/DEBIAN/conffiles"
fi

# postinst: ldconfig for the bundled libscintilla.so, then load the AppArmor
# userns profile so WebKitGTK-based plugins can run their sandbox on
# userns-restricted systems (Ubuntu 24.04+). Both guarded — the package
# must install cleanly on systems without AppArmor.
cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
ldconfig
if command -v apparmor_parser >/dev/null 2>&1 && \
   [ -d /sys/kernel/security/apparmor ] && \
   [ -f /etc/apparmor.d/nextpad-plus-plus ]; then
    apparmor_parser -r -T -W /etc/apparmor.d/nextpad-plus-plus || true
fi
exit 0
EOF

# prerm: unload the profile while its file still exists (dpkg deletes files
# BEFORE postrm runs, so removal must happen here).
cat > "$STAGE/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "remove" ] && \
   command -v apparmor_parser >/dev/null 2>&1 && \
   [ -d /sys/kernel/security/apparmor ] && \
   [ -f /etc/apparmor.d/nextpad-plus-plus ]; then
    apparmor_parser -R /etc/apparmor.d/nextpad-plus-plus 2>/dev/null || true
fi
exit 0
EOF

cat > "$STAGE/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
ldconfig
exit 0
EOF

chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/prerm" "$STAGE/DEBIAN/postrm"
chmod 644 "$STAGE/DEBIAN/conffiles"

OUT="Nextpad++v${VER}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT"
echo "── Built $OUT"
dpkg-deb -I "$OUT" | sed -n '1,12p'

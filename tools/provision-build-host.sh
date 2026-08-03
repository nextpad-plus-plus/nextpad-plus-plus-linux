#!/bin/bash
# Turn a fresh Ubuntu VM into a Nextpad++ build host — idempotent, safe to
# re-run, and safe to run against a host that is already provisioned.
#
#   tools/provision-build-host.sh              # provision THIS machine
#   tools/provision-build-host.sh ubuntu24     # provision a remote host over ssh
#
# Exists so a rebuilt/crashed VM can be brought back in one command instead
# of rediscovering the setup. Encodes two things learned the hard way:
#
#  1. THE APT SOURCES TRAP. Both VMs shipped with the "-updates" pocket
#     MISSING from their apt sources, while their installed runtime libs came
#     FROM that pocket. The install-time package lists still held updates
#     data, so candidate versions looked fine — until the first `apt-get
#     update` rewrote them, after which every -dev package resolved to the
#     older release version and conflicted with the installed runtime
#     ("held broken packages", a wall of unmet dependencies). So: fix the
#     sources BEFORE the first update. Handles both the deb822 format
#     (24.04+: ubuntu.sources) and the classic one (22.04: sources.list).
#
#  2. Lock contention. PackageKit / unattended-upgrades grab the dpkg lock
#     sporadically on desktop images, so apt calls wait for it.
set -euo pipefail

REMOTE="${1:-}"
if [ -n "$REMOTE" ]; then
    echo "── Provisioning remote host: $REMOTE"
    ssh -o BatchMode=yes "$REMOTE" 'bash -s' < "$0"      # NOTE: no ssh -n; it
    exit $?                                              # would eat the script
fi

# ── everything below runs ON the target host ────────────────────────────
command -v sudo >/dev/null || { echo "sudo required" >&2; exit 1; }
sudo -n true 2>/dev/null || {
    echo "ERROR: passwordless sudo is required for unattended provisioning." >&2
    echo "  enable with:  echo \"\$USER ALL=(ALL) NOPASSWD:ALL\" | sudo tee /etc/sudoers.d/99-\$USER" >&2
    exit 1; }

CODENAME=$(. /etc/os-release && echo "${VERSION_CODENAME:-}")
echo "── Host: $(. /etc/os-release && echo "$PRETTY_NAME") ($(uname -m)), codename=$CODENAME"

apt_wait() {  # PackageKit/unattended-upgrades hold the lock on desktop images
    for _ in $(seq 1 90); do
        sudo -n fuser /var/lib/dpkg/lock-frontend /var/lib/apt/lists/lock \
             /var/cache/apt/archives/lock >/dev/null 2>&1 || return 0
        sleep 2
    done
}

# ── 1. repair apt sources BEFORE any apt-get update ─────────────────────
DEB822=/etc/apt/sources.list.d/ubuntu.sources
CLASSIC=/etc/apt/sources.list
fixed=0
if [ -f "$DEB822" ] && ! grep -qE "^Suites:.*${CODENAME}-updates" "$DEB822"; then
    echo "── apt sources: ${CODENAME}-updates MISSING (deb822) — restoring"
    sudo -n cp -a "$DEB822" "${DEB822}.bak-$(date +%Y%m%d-%H%M%S)"
    sudo -n sed -i "s|^Suites: ${CODENAME}\$|Suites: ${CODENAME} ${CODENAME}-updates ${CODENAME}-backports|" "$DEB822"
    fixed=1
elif [ ! -f "$DEB822" ] && [ -f "$CLASSIC" ] && ! grep -qE "^deb .*${CODENAME}-updates" "$CLASSIC"; then
    echo "── apt sources: ${CODENAME}-updates MISSING (classic) — restoring"
    sudo -n cp -a "$CLASSIC" "${CLASSIC}.bak-$(date +%Y%m%d-%H%M%S)"
    MIRROR=$(awk "/^deb .*${CODENAME} main/{print \$2; exit}" "$CLASSIC")
    MIRROR=${MIRROR:-http://archive.ubuntu.com/ubuntu/}
    sudo -n tee -a "$CLASSIC" >/dev/null <<EOF

# added by provision-build-host.sh: the -updates pocket was absent
deb $MIRROR ${CODENAME}-updates main restricted universe multiverse
deb $MIRROR ${CODENAME}-backports main restricted universe multiverse
EOF
    fixed=1
fi
[ "$fixed" = 1 ] && echo "── sources repaired" || echo "── apt sources already correct"

# ── 2. packages ─────────────────────────────────────────────────────────
apt_wait
sudo -n DEBIAN_FRONTEND=noninteractive apt-get update -qq
apt_wait
echo "── Installing toolchain + dependencies"
sudo -n DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential cmake pkg-config \
    libgtk-4-dev libadwaita-1-dev libglib2.0-dev libuchardet-dev \
    git rsync rpm \
    xvfb xdotool imagemagick dbus-x11

# ── 3. verify — compile AND run, don't just trust pkg-config ────────────
echo "── Verifying"
printf '  %-16s %s\n' gcc "$(gcc -dumpfullversion 2>/dev/null)" \
                      cmake "$(cmake --version | head -1 | awk '{print $3}')" \
                      rpmbuild "$(rpmbuild --version 2>/dev/null | awk '{print $3}')"
for m in gtk4 libadwaita-1 glib-2.0 uchardet; do
    printf '  %-16s %s\n' "$m" "$(pkg-config --modversion "$m" 2>&1)"
done
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/p.c" <<'EOF'
#include <gtk/gtk.h>
#include <adwaita.h>
#include <uchardet/uchardet.h>
int main(void){ uchardet_t u=uchardet_new(); uchardet_delete(u);
  g_print("  probe OK       gtk %d.%d.%d glib %d.%d.%d\n",
    gtk_get_major_version(),gtk_get_minor_version(),gtk_get_micro_version(),
    GLIB_MAJOR_VERSION,GLIB_MINOR_VERSION,GLIB_MICRO_VERSION); return 0; }
EOF
gcc "$T/p.c" -o "$T/p" $(pkg-config --cflags --libs gtk4 libadwaita-1 uchardet)
"$T/p"
echo 'int main(){auto f=[](auto x){return x;};return f(0);}' > "$T/p.cpp"
g++ -std=c++17 "$T/p.cpp" -o "$T/pp" && echo "  c++17 OK"

MEM_MB=$(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo)
echo "── Ready. RAM ${MEM_MB} MB / $(nproc) cores → builds will self-cap at -j$(( MEM_MB/500 < $(nproc) ? MEM_MB/500 : $(nproc) )) (500 MB per job)"

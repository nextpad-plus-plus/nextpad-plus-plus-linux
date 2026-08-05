# 10 — Distribution: .deb behavior on Ubuntu, signing, and the apt repository

Date: 2026-08-05. Records the investigation triggered by first real-world
install tests of `Nextpad++v1.1.0_amd64.deb` on x86 Ubuntu machines
(double-click install, one machine online, one offline), the deb-signing
question ("can debs be signed like macOS apps?"), and the design + test
evidence for the signed apt repository kit that came out of it
(commits `478696a`, `7a4bc0d`). The operational runbook lives in
`packaging/apt-repo/README.md`; this document is the *why* and the
decision record.

---

## 1. What was observed installing the .deb, and why

### 1.1 "Install needs WiFi?"

No — the .deb itself installs from the local file; the network is needed
only for **dependencies that are missing on the target machine**. Our
control fields:

```
Depends:    libgtk-4-1, libglib2.0-0t64, libuchardet0, libstdc++6, libc6
Recommends: unzip, git
```

On a stock Ubuntu 24.04 Desktop, everything in `Depends` except possibly
`libuchardet0` is already installed (GNOME itself runs on GTK4/GLib). The
"extra packages downloaded from the internet" seen during a terminal
install are the **Recommends** — `apt` installs recommends by default, and
`git` (with `git-man`, `liberror-perl`) plus `unzip` are the usual
culprits.

**Fully offline install**: `sudo dpkg -i Nextpad++v…deb` — dpkg skips
Recommends entirely and succeeds whenever the five `Depends` are present.
Graceful degradation without the recommends: no `git` → the Source
Control panel reports no repository; no `unzip` → Plugins Admin cannot
extract plugin archives. Everything else works.

Future option if offline-first ever matters: statically vendor uchardet
(small, MPL, license-compatible) so `Depends` is 100 % stock-desktop and
`dpkg -i` succeeds offline on every Ubuntu Desktop out of the box.

### 1.2 App Center behavior (double-click)

Double-clicking hands the file to Ubuntu's App Center (the Flutter
snap-store), which is known-poor with local debs:

- It tries to fetch metadata online; **offline it hangs with no status**
  (observed). Not a Linux rule — an App Center defect. Terminal installs
  are the reliable path.
- The card shows a **generic grey pie-chart icon**, "Unknown publisher",
  "Download size 0 B", "License unknown" for *any* local third-party
  .deb — App Center never extracts icons/metadata from inside a deb
  archive; it only shows rich cards for packages it can match to Snap
  Store or AppStream (DEP-11) repo catalogs. Google Chrome's official
  .deb gets the identical treatment. Our package is fine: it ships
  hicolor icons (48/128/256) + .desktop + metainfo.xml, so after install
  the real icon appears everywhere (dock, app grid, alt-tab).
- The **"Potentially unsafe / third party" warning** is standard for any
  deb installed outside a configured signed repository (Windows
  SmartScreen equivalent). Removable only by distribution channel, not by
  anything inside the package.
- The **admin password prompt** is unremovable by design: installing a
  system package writes to `/usr`, and polkit prompts even for packages
  from Ubuntu's own archive. macOS `.pkg` installers prompt identically;
  only drag-to-/Applications bundles skip it, and Linux has no system-
  package analog of that.

---

## 2. Can .deb files be signed like macOS apps? (investigation result)

**There is no Gatekeeper/notarization equivalent for standalone debs.
Debian-world trust attaches to the distribution channel (a signed apt
repository), not to the file.** Checked on Ubuntu 24.04:

| Mechanism | Status | Verdict |
|---|---|---|
| Embedded per-file signatures (`debsigs` 0.1.26 + `debsig-verify` 0.29) | Tools exist; dpkg does **not** verify by default, no `/etc/debsig/` policy on stock systems (verified), App Center ignores them, `dpkg-sig` dropped from 24.04 | Dead end — changes nothing user-visible |
| **Repository signing** (GPG-signed `InRelease`; SHA256 chains to every deb) | The standard mechanism; what Chrome/VS Code/Docker use | **Adopted — see §3** |
| Snap Store | Real publisher identity, icon, no warning, auto-updates in App Center | Closest to notarization UX; requires snap packaging/confinement work; not pursued now |

Key nuance of the Chrome pattern: the **first** downloaded .deb always
shows the third-party warning (even Google's); that install then
subscribes the machine to the vendor's signed repo, and everything
afterward flows verified and silent.

---

## 3. The signed apt repository kit (`packaging/apt-repo/`, commit 478696a)

Fits the intended distribution exactly: **nextpad.org keeps a single
download button**; the .deb it serves bootstraps the update channel.

### 3.1 Pieces

- **`make-apt-repo.sh`** — builds `dist/apt/` from `dist/*.deb`:
  - pool with Debian-canonical names
    (`pool/main/n/nextpad-plus-plus/nextpad-plus-plus_<ver>_<arch>.deb`
    — which also permanently sidesteps the documented `+`-in-URL
    encoding trap of the `Nextpad++v…` asset names);
  - per-arch `dists/stable/main/binary-<arch>/Packages{,.gz}`
    (apt-ftparchive, `Filename:` relative to repo root);
  - `Release` (apt-ftparchive; Origin/Label/Suite/Codename/Components/
    Architectures) + GPG `InRelease` (clearsigned, what modern apt reads)
    + `Release.gpg` (detached, older apt).
  - Signing key selected by `NPP_SIGN_KEY` (default
    `release@nextpad.org`).
- **`nextpad.sources`** — deb822 subscription installed by the .deb at
  `/etc/apt/sources.list.d/nextpad.sources`, `Signed-By:` pinned to the
  shipped keyring. Marked **conffile**: deleting it is the supported
  update opt-out and survives upgrades (which arrive *via* that repo).
- **`tools/make-deb.sh`** — additionally stages
  `/usr/share/keyrings/nextpad-archive-keyring.gpg` + the sources file
  **iff** `packaging/apt-repo/nextpad-archive-keyring.gpg` exists;
  keyless builds remain valid standalone packages (explicit build note).
- **`README.md`** — the runbook: one-time key generation (ed25519,
  sign-only, no expiry, revocation cert), public-keyring export/commit,
  per-release workflow, website wiring, CLI install path, local
  verification recipe, key rotation.

### 3.2 End-user flow

1. Button → `.deb` → first install (warning + password, unavoidable,
   same as Chrome).
2. Machine now trusts the repo. Every future release published to
   `nextpad.org/apt` reaches all installed users through Software
   Updater / `apt upgrade` / unattended-upgrades — GPG-verified, no
   warnings, no site revisit. The button becomes a one-time bootstrap;
   the repo is the controlled auto-update channel.

### 3.3 Release workflow (after the one-time key setup)

```sh
tools/make-deb.sh                      # per arch (amd64 + arm64 build hosts)
cp Nextpad++v*_*.deb dist/
packaging/apt-repo/make-apt-repo.sh    # → dist/apt/ (signed)
# upload dist/apt/ so it serves at https://nextpad.org/apt
```

Button URL can point straight into the pool (single artifact copy). If
the repo is served from a different URL, update `URIs:` in
`nextpad.sources` **before** building the release debs.

### 3.4 Outstanding (maintainer-only)

The real release key does not exist yet. README steps 1–2: generate the
key, export the public half to
`packaging/apt-repo/nextpad-archive-keyring.gpg`, commit. Until then,
debs build without the subscription.

---

## 4. Test evidence (2026-08-05, throwaway key)

- Repo generated from both existing v1.1.0 debs; tree verified
  (pool ×2 arches, per-arch indexes, Release/InRelease/Release.gpg).
- **Isolated apt sandbox** (own `APT_CONFIG` with redirected
  Dir::State/Cache/Etc + empty dpkg status; local `python3 -m
  http.server`): `apt-get update` fetched and **GPG-verified**
  `InRelease`; `apt-cache policy` shows candidate 1.1.0 at priority 500
  from the repo; `apt-get download` fetched the 5.5 MB pool deb with
  hash verification.
  - Note: `apt-get -o Dir::…` command-line overrides were silently
    ignored (apt hit the system dirs); the `APT_CONFIG` file approach is
    the reliable one — recorded in the README's verification recipe.
- **Negative tests (both must refuse, both did):**
  - tampered `InRelease` body → `BADSIG …` → "repository is not signed";
  - `Signed-By` keyring without the signing key → `NO_PUBKEY …` →
    refused.
- **Package integration:** full `make-deb.sh` with a test keyring →
  keyring/sources present, keyring byte-identical to the source file,
  conffiles lists both entries; keyless rebuild → note printed, no
  subscription files, single conffile.
- gpg batch keygen headless needs
  `--pinentry-mode loopback --passphrase ''` (default pinentry times
  out without a TTY).

---

## 5. Corrections and small fixes made along the way

- **metainfo.xml was never missing from the .deb.** An earlier claim to
  the contrary came from a `head`-truncated `dpkg-deb -c` listing; the
  CMake install rule has shipped `/usr/share/metainfo/…` since the
  initial commit. The redundant copy added in `478696a` was removed in
  `7a4bc0d`. (Lesson: never grep a head-cut archive listing.)
- metainfo **content** was stale and is now fixed: "GTK 3" → "GTK 4",
  bugtracker/vcs URLs → `nextpad-plus-plus-linux`, and a 1.1.0 release
  entry added (was stuck at 1.0.6).
- `.gitignore`: `dist/apt/` (generated tree).

## 6. Future options (not scheduled)

- **AppStream (DEP-11) catalog in the repo** → full rich card (icon,
  screenshots, publisher) in App Center/GNOME Software for users browsing
  the repo. The per-app metainfo.xml prerequisite already ships.
- **Snap Store** publication for a warning-free, store-grade install page.
- **Vendored uchardet** for fully offline `dpkg -i` on stock desktops.
- Serving the repo from GitHub Pages of the public linux repo
  (CNAME `apt.nextpad.org`) instead of nextpad.org hosting, if preferred.

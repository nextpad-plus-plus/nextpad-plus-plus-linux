# Nextpad++ signed apt repository

Chrome-style distribution: the website button serves a single `.deb`; that
first install is manual (and shows Ubuntu's standard third-party warning —
unavoidable for any local deb, Google's included), but it also installs

- `/usr/share/keyrings/nextpad-archive-keyring.gpg` — the release public key
- `/etc/apt/sources.list.d/nextpad.sources` — the repo subscription
  (a conffile; users opt out of updates by deleting it)

so every machine that installed once receives all future releases through
Ubuntu's normal update mechanism (Software Updater / `apt upgrade` /
unattended-upgrades), cryptographically verified, without revisiting the
site.

## One-time setup (you)

**1. Generate the release signing key** (private key never leaves your
machine; only `make-apt-repo.sh` uses it):

```sh
gpg --quick-generate-key "Nextpad++ Release Signing <release@nextpad.org>" ed25519 sign never
gpg --output nextpad-release-revoke.asc --gen-revocation release@nextpad.org   # store safely offline
```

**2. Export the PUBLIC half into the repo** (binary keyring format — this
file is committed and shipped inside the .deb):

```sh
gpg --export release@nextpad.org > packaging/apt-repo/nextpad-archive-keyring.gpg
git add packaging/apt-repo/nextpad-archive-keyring.gpg && git commit
```

**3. Back up the private key** (offline media):

```sh
gpg --export-secret-keys --armor release@nextpad.org > nextpad-release-PRIVATE.asc
```

## Per-release workflow

```sh
tools/make-deb.sh                      # on each arch build host (amd64 + arm64)
cp Nextpad++v*_*.deb dist/             # collect both debs
packaging/apt-repo/make-apt-repo.sh    # pool + indexes + signed InRelease → dist/apt/
# upload dist/apt/ so it is served at https://nextpad.org/apt
```

`tools/make-deb.sh` embeds the keyring + sources file automatically when
`nextpad-archive-keyring.gpg` exists (and still builds a valid standalone
package when it doesn't). `NPP_SIGN_KEY=<id>` overrides the signing key
selection (default `release@nextpad.org`).

## Website wiring

- The download button can point directly into the repo pool (one copy of
  the artifact):
  `https://nextpad.org/apt/pool/main/n/nextpad-plus-plus/nextpad-plus-plus_1.1.0_amd64.deb`
- Any static host works (the tree is plain files). If served from a
  different URL than `https://nextpad.org/apt`, update `URIs:` in
  `nextpad.sources` BEFORE building the release .debs.

## CLI install path (for docs / fleets)

```sh
curl -fsSL https://nextpad.org/apt/nextpad-archive-keyring.gpg \
  | sudo tee /usr/share/keyrings/nextpad-archive-keyring.gpg > /dev/null
echo 'Types: deb
URIs: https://nextpad.org/apt
Suites: stable
Components: main
Signed-By: /usr/share/keyrings/nextpad-archive-keyring.gpg' \
  | sudo tee /etc/apt/sources.list.d/nextpad.sources > /dev/null
sudo apt update && sudo apt install nextpad-plus-plus
```

(For this to work, also copy the keyring into the uploaded tree root:
`cp packaging/apt-repo/nextpad-archive-keyring.gpg dist/apt/`.)

## Verifying a build locally (no system changes)

```sh
cd dist/apt && python3 -m http.server 8931 &
T=$(mktemp -d); mkdir -p $T/{etc/apt/{sources.list.d,preferences.d},var/lib/apt/lists/partial,var/cache/apt/archives/partial}
printf 'Types: deb\nURIs: http://127.0.0.1:8931\nSuites: stable\nComponents: main\nSigned-By: %s\n' \
  "$PWD/../../packaging/apt-repo/nextpad-archive-keyring.gpg" > $T/etc/apt/sources.list.d/test.sources
apt-get update  -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts=$T/etc/apt/sources.list.d \
                -o Dir::State=$T/var/lib/apt -o Dir::Cache=$T/var/cache/apt -o Dir::Etc::preferencesparts=$T/etc/apt/preferences.d
apt-get install -o ... --print-uris nextpad-plus-plus   # same overrides; resolves + verifies
```

A tampered `InRelease` must make `apt-get update` fail — apt refuses
unsigned/invalid repos by default.

## Notes

- No AppStream (DEP-11) catalog is generated yet; adding one later gives
  App Center / GNOME Software the full card (icon, screenshots) for repo
  browsing. The .deb already ships the per-app metainfo.xml.
- Key rotation: generate the new key, export BOTH keys into the shipped
  keyring (`gpg --export old new > keyring.gpg`), release once, then drop
  the old key in a later release.

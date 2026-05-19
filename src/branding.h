/*
 * branding.h — single source of truth for the user-facing app name and config
 * directory. Grafted files reference these macros instead of hardcoded
 * strings so a future fork (e.g. notetux-plus-plus upstream) can flip the
 * brand with a one-line change.
 *
 * Current settings: Nextpad++ (pre-handoff to Andrea Coi).
 */
#ifndef BRANDING_H
#define BRANDING_H

#define APP_NAME          "Nextpad++"
/* Release version — single source for the About dialog and the
 * Help > Check for Updates comparison. */
#define APP_VERSION       "1.0.6"
/* User config dir name — matches macOS port: $HOME/.nextpad++ (NOT XDG).
 * Use the npp_user_*() helpers in paths.h instead of building this manually. */
#define APP_CONFIG_DIR    ".nextpad++"
/* System data dir name (used for /usr/share/<name>, /usr/lib/<name>). */
#define APP_DATA_DIR      "nextpad-plus-plus"

#endif /* BRANDING_H */

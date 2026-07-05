/*
 * paths.h — single source of truth for filesystem paths.
 *
 * User data lives at $XDG_DATA_HOME/nextpad++ (default
 * ~/.local/share/nextpad++) — the Linux analog of macOS's
 * ~/Library/Application Support/Nextpad++ (issue #67). A one-time
 * migration moves any legacy ~/.nextpad++ tree on first launch.
 * System data (bundled themes / model XMLs / icons) lives at
 * RESOURCES_DIR (compile-time) or /usr/share/nextpad-plus-plus/.
 *
 * Reference: macOS `NppPaths.mm` (Locations service).
 */
#ifndef PATHS_H
#define PATHS_H

#include <glib.h>

/* Returns the user data dir — caller MUST free with g_free(). */
gchar *npp_user_dir(void);

/* Returns "<user data dir>/<subpath>" — caller MUST free. */
gchar *npp_user_subdir(const char *subpath);

/* Returns "<user data dir>/<sub>/<leaf>" — caller MUST free. */
gchar *npp_user_file(const char *subdir_or_null, const char *leaf);

/* Returns RESOURCES_DIR if it exists, else /usr/share/nextpad-plus-plus.
 * Returned string is owned by the library; do not free. */
const char *npp_bundle_dir(void);

/* Returns RESOURCES_DIR/<sub>/<leaf>... — caller MUST free. */
gchar *npp_bundle_file(const char *subdir_or_null, const char *leaf);

/* First-run materialiser. Idempotent. Creates these subdirs and (if
 * missing) copies the corresponding model XML from the bundle:
 *
 *   backup/
 *   themes/
 *   functionList/
 *   userDefineLangs/
 *   plugins/
 *   toolbarIcons/
 *   shortcuts.xml         ← from bundle/shortcuts.xml
 *   langs.xml             ← from bundle/langs.model.xml
 *   stylers.xml           ← from bundle/stylers.model.xml
 *   contextMenu.xml            ← from bundle/contextMenu.xml (active)
 *   tabContextMenu_example.xml ← from bundle/tabContextMenu.xml
 *                                (inactive template — user renames it
 *                                 to tabContextMenu.xml to activate)
 *   toolbarButtonsConf_example.xml ← from bundle/toolbarButtonsConf.xml
 *
 * Mirrors macOS `ensureNppDirs()`.
 */
void npp_ensure_user_dirs(void);

#endif /* PATHS_H */

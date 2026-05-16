/*
 * paths.h — single source of truth for filesystem paths.
 *
 * Matches macOS port: user config lives at $HOME/.nextpad++/ (NOT XDG
 * $XDG_CONFIG_HOME). System data (bundled themes / model XMLs / icons)
 * lives at RESOURCES_DIR (compile-time) or /usr/share/nextpad-plus-plus/.
 *
 * Reference: macOS `MainWindowController.mm:87 — nppConfigDir()`.
 */
#ifndef PATHS_H
#define PATHS_H

#include <glib.h>

/* Returns "$HOME/.nextpad++" — caller MUST free with g_free(). */
gchar *npp_user_dir(void);

/* Returns "$HOME/.nextpad++/<subpath>" — caller MUST free. */
gchar *npp_user_subdir(const char *subpath);

/* Returns "$HOME/.nextpad++/<sub>/<leaf>" — caller MUST free. */
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
 *   contextMenu.xml       ← from bundle/contextMenu.xml
 *   tabContextMenu.xml    ← from bundle/tabContextMenu.xml
 *   toolbarButtonsConf.xml← from bundle/toolbarButtonsConf.xml
 *
 * Mirrors macOS `ensureNppDirs()`.
 */
void npp_ensure_user_dirs(void);

#endif /* PATHS_H */

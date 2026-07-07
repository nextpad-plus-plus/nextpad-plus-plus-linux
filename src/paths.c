#include "paths.h"
#include "gtk_compat.h"
#include "branding.h"
#include "prefs.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>

#ifndef RESOURCES_DIR
#define RESOURCES_DIR "/usr/share/nextpad-plus-plus"
#endif

/* ALWAYS-local base — never redirected to the cloud. Session, backup,
 * plugins and caches resolve through here so a "Settings on cloud"
 * redirect can't move machine-local data off-device (macOS
 * NppLocalConfigDir, GAP-17 / ef295c3). */
/* GAP-62 — -settingsDir=DIR overrides the settings base entirely
 * (both the local base and, transitively, the cloud pointer under it). */
static char s_dir_override[512];

void npp_paths_set_override(const char *dir)
{
    if (dir && *dir) g_strlcpy(s_dir_override, dir, sizeof(s_dir_override));
}

gchar *npp_local_dir(void) {
    if (s_dir_override[0]) return g_strdup(s_dir_override);
    /* $XDG_DATA_HOME/nextpad++ (default ~/.local/share/nextpad++) —
     * the Linux analog of macOS's move to ~/Library/Application
     * Support/Nextpad++ (issue #67). The one-time migration from the
     * legacy ~/.nextpad++ runs in npp_ensure_user_dirs(). */
    return g_build_filename(g_get_user_data_dir(), APP_USER_SUBDIR, NULL);
}

gchar *npp_local_file(const char *subdir_or_null, const char *leaf) {
    gchar *base = npp_local_dir();   /* honours -settingsDir override */
    gchar *p = (subdir_or_null && *subdir_or_null)
        ? g_build_filename(base, subdir_or_null, leaf, NULL)
        : g_build_filename(base, leaf, NULL);
    g_free(base);
    return p;
}

/* "Settings on cloud" (GAP-17, Windows/macOS parity): the user's cloud
 * folder if set AND usable, else NULL. The pointer lives in the LOCAL
 * file cloud/choice — never in the cloud folder itself (bootstrap:
 * config.xml may already be cloud-redirected). Resolved ONCE at first
 * use; a change applies on the next launch ("restart to apply"). */
static gchar    *s_cloud_dir;
static gboolean  s_cloud_resolved;

const char *npp_cloud_choice_file(void) {
    static gchar *p;
    if (!p) {
        gchar *local = npp_local_dir();
        p = g_build_filename(local, "cloud", "choice", NULL);
        g_free(local);
    }
    return p;
}

static const char *cloud_dir_resolved(void) {
    if (s_cloud_resolved) return s_cloud_dir;
    s_cloud_resolved = TRUE;

    gchar *raw = NULL;
    if (!g_file_get_contents(npp_cloud_choice_file(), &raw, NULL, NULL))
        return NULL;
    g_strstrip(raw);
    if (!raw[0]) { g_free(raw); return NULL; }

    g_mkdir_with_parents(raw, 0700);
    if (g_file_test(raw, G_FILE_TEST_IS_DIR) &&
        g_access(raw, W_OK) == 0) {
        s_cloud_dir = raw;   /* owned for process lifetime */
    } else {
        g_warning("cloud settings path unusable (%s) — using local", raw);
        g_free(raw);
    }
    return s_cloud_dir;
}

/* Settings base — cloud when configured & usable, else local. */
gchar *npp_user_dir(void) {
    const char *cloud = cloud_dir_resolved();
    if (cloud) return g_strdup(cloud);
    return npp_local_dir();
}

gchar *npp_user_subdir(const char *subpath) {
    if (!subpath || !*subpath) return npp_user_dir();
    gchar *base = npp_user_dir();
    gchar *p = g_build_filename(base, subpath, NULL);
    g_free(base);
    return p;
}

gchar *npp_user_file(const char *subdir_or_null, const char *leaf) {
    gchar *base = npp_user_dir();
    gchar *p = (subdir_or_null && *subdir_or_null)
        ? g_build_filename(base, subdir_or_null, leaf, NULL)
        : g_build_filename(base, leaf, NULL);
    g_free(base);
    return p;
}

gchar *npp_backup_dir(void)
{
    /* Custom path wins when it is usable — re-evaluated on every call
     * so a fixed permission or replugged drive recovers without a
     * restart (macOS NppBackupDir semantics). */
    const char *custom = g_prefs.backup_custom_dir;
    if (custom && custom[0]) {
        g_mkdir_with_parents(custom, 0700);
        if (g_file_test(custom, G_FILE_TEST_IS_DIR) &&
            g_access(custom, W_OK) == 0)
            return g_strdup(custom);
    }
    gchar *local = npp_local_dir();   /* backup: local, never cloud */
    gchar *dflt = g_build_filename(local, "backup", NULL);
    g_free(local);
    g_mkdir_with_parents(dflt, 0700);
    return dflt;
}

const char *npp_bundle_dir(void) {
    static const char *cached = NULL;
    if (cached) return cached;
    if (g_file_test(RESOURCES_DIR, G_FILE_TEST_IS_DIR))
        cached = RESOURCES_DIR;
    else
        cached = "/usr/share/nextpad-plus-plus";
    return cached;
}

gchar *npp_bundle_file(const char *subdir_or_null, const char *leaf) {
    if (subdir_or_null && *subdir_or_null)
        return g_build_filename(npp_bundle_dir(), subdir_or_null, leaf, NULL);
    return g_build_filename(npp_bundle_dir(), leaf, NULL);
}

/* Copy a file from bundle → user if the user copy doesn't exist. */
static void seed_file(const char *bundle_sub, const char *bundle_leaf,
                      const char *user_sub, const char *user_leaf)
{
    gchar *target = npp_user_file(user_sub, user_leaf);
    if (g_file_test(target, G_FILE_TEST_EXISTS)) { g_free(target); return; }

    gchar *src = npp_bundle_file(bundle_sub, bundle_leaf);
    if (!g_file_test(src, G_FILE_TEST_EXISTS)) {
        g_free(src); g_free(target); return;
    }

    GFile *gs = g_file_new_for_path(src);
    GFile *gt = g_file_new_for_path(target);
    /* Make sure parent dir exists. */
    gchar *parent = g_path_get_dirname(target);
    g_mkdir_with_parents(parent, 0700);
    g_free(parent);

    g_file_copy(gs, gt, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL);
    g_object_unref(gs); g_object_unref(gt);
    g_free(src); g_free(target);
}

/* Recursively copy every regular file in bundle/<sub> to user/<sub>,
 * skipping files that already exist in user. */
static void seed_directory(const char *sub) {
    gchar *bsrc = npp_bundle_file(sub, NULL);
    gchar *udst = npp_user_subdir(sub);
    g_mkdir_with_parents(udst, 0700);

    GDir *d = g_dir_open(bsrc, 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            gchar *src = g_build_filename(bsrc, name, NULL);
            if (g_file_test(src, G_FILE_TEST_IS_REGULAR))
                seed_file(sub, name, sub, name);
            g_free(src);
        }
        g_dir_close(d);
    }
    g_free(bsrc); g_free(udst);
}

/* ------------------------------------------------------------------ */
/* One-time migration: ~/.nextpad++ → $XDG_DATA_HOME/nextpad++          */
/* Mirrors macOS NppPaths.mm (issue #67): rename the whole tree when    */
/* possible (atomic, instant on the same filesystem); otherwise merge-  */
/* move non-clobbering — existing files in the new location win, and    */
/* whatever could not move stays behind in ~/.nextpad++.                */
/* ------------------------------------------------------------------ */

/* Move src's entries into dst without overwriting anything that already
 * exists in dst. Directories recurse; emptied source dirs are removed.
 * Returns TRUE if the source directory is empty (fully migrated). */
static gboolean merge_move_tree(const char *src, const char *dst)
{
    g_mkdir_with_parents(dst, 0700);
    GDir *d = g_dir_open(src, 0, NULL);
    if (!d) return FALSE;

    gboolean all_moved = TRUE;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        gchar *s = g_build_filename(src, name, NULL);
        gchar *t = g_build_filename(dst, name, NULL);
        if (g_file_test(s, G_FILE_TEST_IS_DIR) &&
            !g_file_test(s, G_FILE_TEST_IS_SYMLINK)) {
            if (!merge_move_tree(s, t)) all_moved = FALSE;
            else g_rmdir(s);
        } else if (!g_file_test(t, G_FILE_TEST_EXISTS)) {
            if (g_rename(s, t) != 0) {
                /* Cross-device or permission issue → copy + delete. */
                GFile *gs = g_file_new_for_path(s);
                GFile *gt = g_file_new_for_path(t);
                if (g_file_copy(gs, gt, G_FILE_COPY_NONE,
                                NULL, NULL, NULL, NULL))
                    g_unlink(s);
                else
                    all_moved = FALSE;
                g_object_unref(gs); g_object_unref(gt);
            }
        } else {
            /* Exists in the new location — non-clobber: new wins, the
             * old copy stays behind for the user to inspect. */
            all_moved = FALSE;
        }
        g_free(s); g_free(t);
    }
    g_dir_close(d);
    return all_moved;
}

static void migrate_legacy_config_dir(void)
{
    gchar *oldp = g_build_filename(g_get_home_dir(),
                                   APP_LEGACY_CONFIG_DIR, NULL);
    if (!g_file_test(oldp, G_FILE_TEST_IS_DIR)) { g_free(oldp); return; }

    gchar *newp = npp_user_dir();

    if (!g_file_test(newp, G_FILE_TEST_EXISTS)) {
        /* Fast path: whole-tree rename — atomic on the same filesystem,
         * so a crash mid-migration leaves either the old or the new
         * tree fully intact, never a half state. */
        gchar *parent = g_path_get_dirname(newp);
        g_mkdir_with_parents(parent, 0700);
        g_free(parent);
        if (g_rename(oldp, newp) == 0) {
            g_message("nextpad++: migrated user data %s -> %s", oldp, newp);
            g_free(oldp); g_free(newp);
            return;
        }
        /* rename failed (EXDEV etc.) → fall through to merge-move. */
    }

    if (merge_move_tree(oldp, newp)) {
        g_rmdir(oldp);
        g_message("nextpad++: migrated user data %s -> %s (merge)", oldp, newp);
    } else {
        g_message("nextpad++: partially migrated %s -> %s "
                  "(existing files in the new location were kept; "
                  "leftovers remain in the old directory)", oldp, newp);
    }
    g_free(oldp); g_free(newp);
}

void npp_ensure_user_dirs(void) {
    /* Move a legacy ~/.nextpad++ tree BEFORE anything reads or seeds
     * the new location — must be the very first filesystem touch. */
    migrate_legacy_config_dir();

    /* Create the root. */
    gchar *root = npp_user_dir();
    g_mkdir_with_parents(root, 0700);
    g_free(root);

    /* Create user subdirs that macOS expects. */
    /* Settings-class dirs follow the cloud redirect; backup/ and
     * plugins/ are pinned local (GAP-17). */
    static const char *subs[] = {
        "themes", "functionList", "userDefineLangs", "toolbarIcons", NULL
    };
    for (int i = 0; subs[i]; i++) {
        gchar *d = npp_user_subdir(subs[i]);
        g_mkdir_with_parents(d, 0700);
        g_free(d);
    }
    static const char *local_subs[] = { "backup", "plugins", NULL };
    for (int i = 0; local_subs[i]; i++) {
        gchar *d = npp_local_file(local_subs[i], NULL);
        g_mkdir_with_parents(d, 0700);
        g_free(d);
    }

    /* Seed individual model XMLs from the bundle. */
    /* macOS ensureNppDirs(): shortcuts.xml, contextMenu.xml,
     *                       langs.model.xml → langs.xml,
     *                       stylers.model.xml → stylers.xml,
     *                       tabContextMenu_example.xml,
     *                       toolbarButtonsConf_example.xml. */
    seed_file(NULL, "shortcuts.xml",        NULL, "shortcuts.xml");
    seed_file(NULL, "contextMenu.xml",      NULL, "contextMenu.xml");
    /* macOS copies tabContextMenu_example.xml / toolbarButtonsConf_example.xml.
     * Bundle stores them without the _example suffix, so target uses the
     * macOS user-side name. */
    seed_file(NULL, "tabContextMenu.xml",   NULL, "tabContextMenu_example.xml");
    seed_file(NULL, "toolbarButtonsConf.xml", NULL, "toolbarButtonsConf_example.xml");
    seed_file(NULL, "langs.model.xml",      NULL, "langs.xml");
    seed_file(NULL, "stylers.model.xml",    NULL, "stylers.xml");

    /* Seed themes, UDLs, functionList parsers. */
    seed_directory("themes");
    seed_directory("userDefineLangs");
    seed_directory("functionList");
}

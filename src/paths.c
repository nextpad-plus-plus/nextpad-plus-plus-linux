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

gchar *npp_user_dir(void) {
    /* $XDG_DATA_HOME/nextpad++ (default ~/.local/share/nextpad++) —
     * the Linux analog of macOS's move to ~/Library/Application
     * Support/Nextpad++ (issue #67). The one-time migration from the
     * legacy ~/.nextpad++ runs in npp_ensure_user_dirs(). */
    return g_build_filename(g_get_user_data_dir(), APP_USER_SUBDIR, NULL);
}

gchar *npp_user_subdir(const char *subpath) {
    if (!subpath || !*subpath) return npp_user_dir();
    return g_build_filename(g_get_user_data_dir(), APP_USER_SUBDIR,
                            subpath, NULL);
}

gchar *npp_user_file(const char *subdir_or_null, const char *leaf) {
    if (subdir_or_null && *subdir_or_null)
        return g_build_filename(g_get_user_data_dir(), APP_USER_SUBDIR,
                                subdir_or_null, leaf, NULL);
    return g_build_filename(g_get_user_data_dir(), APP_USER_SUBDIR,
                            leaf, NULL);
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
    gchar *dflt = npp_user_subdir("backup");
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
    static const char *subs[] = {
        "backup", "themes", "functionList", "userDefineLangs",
        "plugins", "toolbarIcons", NULL
    };
    for (int i = 0; subs[i]; i++) {
        gchar *d = npp_user_subdir(subs[i]);
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

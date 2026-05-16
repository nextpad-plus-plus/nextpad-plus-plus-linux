#include "paths.h"
#include "gtk_compat.h"
#include "branding.h"
#include <gio/gio.h>
#include <sys/stat.h>
#include <string.h>

#ifndef RESOURCES_DIR
#define RESOURCES_DIR "/usr/share/nextpad-plus-plus"
#endif

gchar *npp_user_dir(void) {
    return g_build_filename(g_get_home_dir(), APP_CONFIG_DIR, NULL);
}

gchar *npp_user_subdir(const char *subpath) {
    if (!subpath || !*subpath) return npp_user_dir();
    return g_build_filename(g_get_home_dir(), APP_CONFIG_DIR, subpath, NULL);
}

gchar *npp_user_file(const char *subdir_or_null, const char *leaf) {
    if (subdir_or_null && *subdir_or_null)
        return g_build_filename(g_get_home_dir(), APP_CONFIG_DIR,
                                subdir_or_null, leaf, NULL);
    return g_build_filename(g_get_home_dir(), APP_CONFIG_DIR, leaf, NULL);
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

void npp_ensure_user_dirs(void) {
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

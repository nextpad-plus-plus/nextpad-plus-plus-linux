/*
 * recent.c — Recent Files persistence.
 * Algorithm from notetux-plus-plus/linux/src/main.c (GPL-3, Andrea Coi).
 */
#include "recent.h"
#include "paths.h"
#include "gtk_compat.h"
#include "branding.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static GPtrArray *s_recent = NULL;

static char *recent_file_path(void)
{
    return npp_local_file(NULL, "recentfiles.txt");   /* machine paths: local */
}

static void ensure_init(void)
{
    if (!s_recent) s_recent = g_ptr_array_new();
}

void recent_load(void)
{
    ensure_init();
    char *path = recent_file_path();
    char *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i] && s_recent->len < RECENT_MAX; i++) {
            if (lines[i][0] != '\0')
                g_ptr_array_add(s_recent, g_strdup(lines[i]));
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(path);
}

void recent_save(void)
{
    ensure_init();
    /* Create the config dir if needed. */
    char *dir = npp_user_dir();
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GString *buf = g_string_new(NULL);
    for (guint i = 0; i < s_recent->len; i++)
        g_string_append_printf(buf, "%s\n", (char *)s_recent->pdata[i]);
    char *path = recent_file_path();
    g_file_set_contents(path, buf->str, (gssize)buf->len, NULL);
    g_free(path);
    g_string_free(buf, TRUE);
}

void recent_files_add(const char *path)
{
    if (!path || !*path) return;
    ensure_init();

    /* Remove any existing entry for this path. */
    for (guint i = 0; i < s_recent->len; i++) {
        if (strcmp((char *)s_recent->pdata[i], path) == 0) {
            g_free(s_recent->pdata[i]);
            g_ptr_array_remove_index(s_recent, i);
            break;
        }
    }
    /* Prepend. */
    g_ptr_array_insert(s_recent, 0, g_strdup(path));
    /* Trim. */
    while (s_recent->len > RECENT_MAX) {
        g_free(s_recent->pdata[s_recent->len - 1]);
        g_ptr_array_remove_index(s_recent, s_recent->len - 1);
    }
    recent_save();
}

void recent_files_clear(void)
{
    ensure_init();
    for (guint i = 0; i < s_recent->len; i++)
        g_free(s_recent->pdata[i]);
    g_ptr_array_set_size(s_recent, 0);
    recent_save();
}

GPtrArray *recent_files_get(void)
{
    ensure_init();
    return s_recent;
}

guint recent_files_count(void)
{
    ensure_init();
    return s_recent->len;
}

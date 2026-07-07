/* recent.c — Recent Files list.
 *
 * A small MRU list persisted as one absolute path per line in the
 * machine-local recentfiles.txt (never cloud-redirected — the paths
 * only mean something on this computer). Matches the macOS port's
 * behaviour: RECENT_MAX entries, most recent first, re-opening a file
 * moves it to the top.
 */
#include "recent.h"
#include "paths.h"

#include <glib.h>
#include <string.h>

static GPtrArray *s_list;   /* char* entries, owned, index 0 = newest */

static GPtrArray *list(void)
{
    if (!s_list) s_list = g_ptr_array_new_with_free_func(g_free);
    return s_list;
}

static gint find_entry(const char *path)
{
    GPtrArray *l = list();
    for (guint i = 0; i < l->len; i++)
        if (strcmp(g_ptr_array_index(l, i), path) == 0)
            return (gint)i;
    return -1;
}

void recent_load(void)
{
    gchar *file = npp_local_file(NULL, "recentfiles.txt");
    gchar *data = NULL;

    if (g_file_get_contents(file, &data, NULL, NULL)) {
        for (char *line = strtok(data, "\n");
             line && list()->len < RECENT_MAX;
             line = strtok(NULL, "\n")) {
            if (*line)
                g_ptr_array_add(list(), g_strdup(line));
        }
        g_free(data);
    }
    g_free(file);
}

void recent_save(void)
{
    gchar *dir = npp_local_dir();
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GString *out = g_string_new(NULL);
    for (guint i = 0; i < list()->len; i++) {
        g_string_append(out, g_ptr_array_index(list(), i));
        g_string_append_c(out, '\n');
    }

    gchar *file = npp_local_file(NULL, "recentfiles.txt");
    g_file_set_contents(file, out->str, (gssize)out->len, NULL);
    g_free(file);
    g_string_free(out, TRUE);
}

void recent_files_add(const char *path)
{
    if (!path || !*path) return;

    gint at = find_entry(path);
    if (at >= 0)
        g_ptr_array_remove_index(list(), (guint)at);

    g_ptr_array_insert(list(), 0, g_strdup(path));
    g_ptr_array_set_size(list(), MIN(list()->len, RECENT_MAX));
    recent_save();
}

void recent_files_clear(void)
{
    g_ptr_array_set_size(list(), 0);
    recent_save();
}

GPtrArray *recent_files_get(void)
{
    return list();
}

guint recent_files_count(void)
{
    return list()->len;
}

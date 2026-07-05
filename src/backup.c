/* backup.c — Periodic auto-backup for the Linux GTK3 port.
 *
 * Every g_prefs.backup_interval_secs seconds, any modified (unsaved) document
 * is written to <user data dir>/backup/<basename>.  On a clean save or on tab
 * close the backup file is removed.
 *
 * Naming:
 *   Saved file  → backup/<basename>
 *   Unsaved doc → backup/new_<N>
 *
 * The timer runs on the GLib main loop; no threading needed.
 */
#include "backup.h"
#include "paths.h"
#include "gtk_compat.h"
#include "branding.h"
#include "prefs.h"
#include "sci_c.h"
#include <string.h>
#include <stdio.h>
#include <glib/gstdio.h>

static guint s_timer_id = 0;

/* ------------------------------------------------------------------ */
/* Path helper                                                         */
/* ------------------------------------------------------------------ */

static void backup_path_for(NppDoc *doc, char *out, gsize size)
{
    const char *leaf;
    char tmp[32];
    if (doc->filepath) {
        leaf = g_path_get_basename(doc->filepath);
    } else {
        snprintf(tmp, sizeof(tmp), "new_%d", doc->new_index);
        leaf = tmp;
    }
    gchar *dir = npp_user_subdir("backup");
    snprintf(out, size, "%s/%s", dir, leaf);
    g_free(dir);
}

/* ------------------------------------------------------------------ */
/* Write one backup                                                    */
/* ------------------------------------------------------------------ */

static void backup_write(NppDoc *doc)
{
    sptr_t len = scintilla_send_message(SCINTILLA(doc->sci), SCI_GETLENGTH, 0, 0);
    gchar *buf = g_new(gchar, len + 1);
    scintilla_send_message(SCINTILLA(doc->sci), SCI_GETTEXT,
                           (uptr_t)(len + 1), (sptr_t)buf);

    char path[1024];
    backup_path_for(doc, path, sizeof(path));

    GError *err = NULL;
    if (!g_file_set_contents(path, buf, len, &err)) {
        g_warning("backup: cannot write %s: %s", path, err->message);
        g_error_free(err);
    }
    g_free(buf);
}

/* ------------------------------------------------------------------ */
/* Timer callback                                                      */
/* ------------------------------------------------------------------ */

static gboolean backup_tick(gpointer data)
{
    (void)data;
    if (!g_prefs.backup_enabled) return G_SOURCE_CONTINUE;

    /* All notebooks — a modified tab moved to a split view must be
     * backed up too (same blind spot as macOS issue #162). */
    GPtrArray *docs = editor_all_docs();
    for (guint i = 0; i < docs->len; i++) {
        NppDoc *doc = g_ptr_array_index(docs, i);
        if (doc->modified)
            backup_write(doc);
    }
    g_ptr_array_free(docs, TRUE);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void backup_init(void)
{
    /* Ensure the backup directory exists */
    gchar *dir = npp_user_subdir("backup");
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    int interval = g_prefs.backup_interval_secs;
    if (interval < 1) interval = 60;
    s_timer_id = g_timeout_add_seconds((guint)interval, backup_tick, NULL);
}

void backup_restart_timer(void)
{
    if (s_timer_id) {
        g_source_remove(s_timer_id);
        s_timer_id = 0;
    }
    if (!g_prefs.backup_enabled) return;
    int interval = g_prefs.backup_interval_secs;
    if (interval < 1) interval = 60;
    s_timer_id = g_timeout_add_seconds((guint)interval, backup_tick, NULL);
}

void backup_clean(NppDoc *doc)
{
    char path[1024];
    backup_path_for(doc, path, sizeof(path));
    g_remove(path);  /* silent if file does not exist */
}

/* Force-write a snapshot regardless of timer. Called by editor.c on
 * close-all-quit so the backup persists into the next launch. Path is
 * stored on NppDoc->backup_filepath so session.xml round-trips it. */
void backup_snapshot_now(NppDoc *doc)
{
    if (!doc || !doc->sci) return;
    char path[1024];
    backup_path_for(doc, path, sizeof(path));
    backup_write(doc);
    g_free(doc->backup_filepath);
    doc->backup_filepath = g_strdup(path);
}

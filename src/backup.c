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
#include <time.h>

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
    gchar *dir = npp_backup_dir();
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
    /* Ensure the backup directory exists (honours the custom path). */
    gchar *dir = npp_backup_dir();
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

/* ------------------------------------------------------------------ */
/* GAP-116a — non-destructive stale-backup quarantine                  */
/* (macOS 5e1c814, NppBackupPrune.mm — semantics mirrored exactly)     */
/* ------------------------------------------------------------------ */

/* Days a quarantined run is kept in .trash before it is aged out. */
#define BACKUP_TRASH_RETENTION_DAYS 7
#define BACKUP_TRASH_DIR            ".trash"

/* Recursive delete, symlink-safe: a symlink inside .trash is unlinked
 * (never followed), so a planted link can not make the age-out reach
 * outside the quarantine. */
static void backup_rm_rf(const char *path)
{
    if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK) &&
        g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *d = g_dir_open(path, 0, NULL);
        if (d) {
            const char *n;
            while ((n = g_dir_read_name(d))) {
                gchar *child = g_build_filename(path, n, NULL);
                backup_rm_rf(child);
                g_free(child);
            }
            g_dir_close(d);
        }
        g_rmdir(path);
    } else {
        g_remove(path);
    }
}

/* Parse a run-folder stamp ("yyyymmdd-HHMMSS", written by us in local
 * time) back to a time_t. Returns (time_t)-1 when the name is not a
 * full, exact stamp — extra characters mean "not our folder". */
static time_t backup_trash_stamp_parse(const char *name)
{
    int Y, M, D, h, m, sec;
    char extra;
    if (sscanf(name, "%4d%2d%2d-%2d%2d%2d%c",
               &Y, &M, &D, &h, &m, &sec, &extra) != 6)
        return (time_t)-1;
    if (strlen(name) != 15) return (time_t)-1;
    struct tm tm = {0};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h;        tm.tm_min = m;     tm.tm_sec  = sec;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* Age out quarantine folders older than the retention window. Age comes
 * from the folder's stamp name; if that does not parse (user-created
 * folder?), fall back to the filesystem mtime (Linux has no portable
 * creation date — macOS used birth time here); if neither is available,
 * never delete. */
static void backup_trash_age_prune(const char *trash_dir, time_t now)
{
    GDir *d = g_dir_open(trash_dir, 0, NULL);
    if (!d) return;
    const double retention = BACKUP_TRASH_RETENTION_DAYS * 86400.0;
    const char *run;
    while ((run = g_dir_read_name(d))) {
        gchar *full = g_build_filename(trash_dir, run, NULL);
        if (!g_file_test(full, G_FILE_TEST_IS_DIR) ||
            g_file_test(full, G_FILE_TEST_IS_SYMLINK)) {
            g_free(full);
            continue;
        }
        time_t born = backup_trash_stamp_parse(run);
        if (born == (time_t)-1) {
            GStatBuf st;
            if (g_stat(full, &st) == 0) born = st.st_mtime;
        }
        if (born != (time_t)-1 && difftime(now, born) > retention)
            backup_rm_rf(full);
        g_free(full);
    }
    g_dir_close(d);
}

/* Core sweep, dependency-free for testability (macOS injects `now` the
 * same way): every plain file in backup_dir NOT present in active_paths
 * is MOVED to backup_dir/.trash/<yyyymmdd-HHMMSS>/ — never deleted.
 * Skipped entirely: dot-entries (.trash itself, stray dotfiles),
 * subdirectories, and symlinks. Returns the number of files
 * quarantined; on failure to create the quarantine folder NOTHING is
 * touched (leave-in-place beats destroy). */
int backup_quarantine_stale_at(const char *backup_dir,
                               GHashTable *active_paths, time_t now)
{
    if (!backup_dir || !*backup_dir ||
        !g_file_test(backup_dir, G_FILE_TEST_IS_DIR))
        return 0;

    gchar *trash_dir = g_build_filename(backup_dir, BACKUP_TRASH_DIR, NULL);
    char stamp[16];
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    gchar *run_dir = g_build_filename(trash_dir, stamp, NULL);

    int quarantined = 0;
    GDir *d = g_dir_open(backup_dir, 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            if (name[0] == '.') continue;              /* .trash, dotfiles */
            gchar *full = g_build_filename(backup_dir, name, NULL);
            if (g_file_test(full, G_FILE_TEST_IS_DIR) ||
                g_file_test(full, G_FILE_TEST_IS_SYMLINK) ||
                (active_paths &&
                 g_hash_table_contains(active_paths, full))) {
                g_free(full);
                continue;
            }
            /* Lazily create this run's folder on the first move; if that
             * fails and it does not exist, quarantining is impossible —
             * leave every file in place. */
            if (quarantined == 0 &&
                g_mkdir_with_parents(run_dir, 0700) != 0 &&
                !g_file_test(run_dir, G_FILE_TEST_IS_DIR)) {
                g_free(full);
                g_dir_close(d);
                g_free(run_dir); g_free(trash_dir);
                return 0;
            }
            gchar *dest = g_build_filename(run_dir, name, NULL);
            for (unsigned i = 2; g_file_test(dest, G_FILE_TEST_EXISTS); i++) {
                g_free(dest);
                gchar *suffixed = g_strdup_printf("%s-%u", name, i);
                dest = g_build_filename(run_dir, suffixed, NULL);
                g_free(suffixed);
            }
            if (g_rename(full, dest) == 0) {
                quarantined++;
            } else {
                g_warning("backup: could not quarantine stale backup %s",
                          name);
            }
            g_free(dest);
            g_free(full);
        }
        g_dir_close(d);
    }

    if (g_file_test(trash_dir, G_FILE_TEST_IS_DIR))
        backup_trash_age_prune(trash_dir, now);

    g_free(run_dir);
    g_free(trash_dir);
    return quarantined;
}

/* Session-save entry point. Builds the active set from every open doc
 * (its computed backup path + any recorded backup_filepath) and sweeps
 * the DEFAULT backup dir only — a user-chosen custom backup directory
 * is never swept at all: it may hold files that were never ours (the
 * macOS issue #215 custom-dir wipe). */
void backup_quarantine_stale(void)
{
    gchar *dir = npp_backup_dir();

    /* Only the default dir is ever swept (macOS compares against the
     * local-config "backup" subpath the same way). */
    gchar *local = npp_local_dir();
    gchar *dflt  = g_build_filename(local, "backup", NULL);
    g_free(local);
    gboolean is_default = (strcmp(dir, dflt) == 0);
    g_free(dflt);
    if (!is_default) {
        g_free(dir);
        return;
    }

    GHashTable *active = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    GPtrArray *docs = editor_all_docs();
    for (guint i = 0; i < docs->len; i++) {
        NppDoc *doc = g_ptr_array_index(docs, i);
        char path[1024];
        backup_path_for(doc, path, sizeof(path));
        g_hash_table_add(active, g_strdup(path));
        if (doc->backup_filepath && doc->backup_filepath[0])
            g_hash_table_add(active, g_strdup(doc->backup_filepath));
    }
    g_ptr_array_free(docs, TRUE);

    int n = backup_quarantine_stale_at(dir, active, time(NULL));
    if (n > 0)
        g_message("session: quarantined %d stale backup file(s) to "
                  "backup/" BACKUP_TRASH_DIR, n);

    g_hash_table_destroy(active);
    g_free(dir);
}

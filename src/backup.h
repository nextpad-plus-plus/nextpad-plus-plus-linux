#ifndef BACKUP_H
#define BACKUP_H

#include "editor.h"

/* Start the periodic backup timer (call once after editor_init). */
void backup_init(void);

/* Restart the timer with the current g_prefs interval (call when prefs change). */
void backup_restart_timer(void);

/* Remove the backup file for a document (call on clean save and on close). */
void backup_clean(NppDoc *doc);

/* Force-write a snapshot regardless of timer; updates doc->backup_filepath
 * so session.xml round-trips it for crash recovery. */
void backup_snapshot_now(NppDoc *doc);

/* GAP-116a (macOS 5e1c814) — non-destructive stale-backup quarantine.
 * backup_quarantine_stale(): sweep the DEFAULT backup dir at session
 * save; unreferenced files move to backup/.trash/<stamp>/ (7-day
 * age-out). Never touches a custom backup directory.
 * backup_quarantine_stale_at() is the dependency-free core (dir +
 * active-path set + injected `now`), exported for testing. */
void backup_quarantine_stale(void);
int  backup_quarantine_stale_at(const char *backup_dir,
                                GHashTable *active_paths, time_t now);

#endif /* BACKUP_H */

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

#endif /* BACKUP_H */

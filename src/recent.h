/*
 * recent.h — Recent Files persistence (G3.5).
 *
 * Algorithm grafted from notetux-plus-plus/linux/src/main.c (Andrea Coi,
 * GPL-3). UI binding rewritten for GMenuModel / GAction.
 *
 * Store: ~/<APP_CONFIG_DIR>/recentfiles.txt (one path per line).
 * Cap:   15 entries (matches macOS port; notetux upstream caps at 10).
 */
#ifndef RECENT_H
#define RECENT_H

#include <glib.h>

#define RECENT_MAX 15

/* Load from disk into the in-memory list. Call once at startup. */
void        recent_load(void);

/* Write the in-memory list back to disk (called automatically after add/clear). */
void        recent_save(void);

/* Add `path` to the head, deduping and trimming to RECENT_MAX. */
void        recent_files_add(const char *path);

/* Empty the list and persist. */
void        recent_files_clear(void);

/* Borrow the GPtrArray of g_strdup'd paths (do not modify). */
GPtrArray  *recent_files_get(void);

/* Convenience. */
guint       recent_files_count(void);

#endif /* RECENT_H */

#ifndef WORKSPACE_H
#define WORKSPACE_H

#include <gtk/gtk.h>

/* Create the panel widget; parent_window used for the open-folder dialog. */
GtkWidget *workspace_init(GtkWidget *parent_window);

/* Add a directory as an additional workspace root. */
void workspace_add_folder(const char *path);

/* Back-compat: old name now aliased to add (multi-root mode). */
void workspace_set_folder(const char *path);

/* Remove a single root (no-op if not present). */
void workspace_remove_folder(const char *path);

/* Drop all roots. */
void workspace_clear(void);

/* Re-read every root from disk (used after Rename / on-disk changes). */
void workspace_refresh(void);

/* Returns NULL-terminated array of root paths; caller MUST g_strfreev. */
gchar **workspace_get_roots(void);

/* Show / hide the panel. */
void     workspace_set_visible(gboolean v);
gboolean workspace_is_visible(void);

#endif /* WORKSPACE_H */

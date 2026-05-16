/*
 * gitpanel.h — full Git side-panel (status + log + blame).
 *
 * Per G18 of docs/PORTING_PHASES.md. Equivalent to macOS GitPanel.mm.
 * Async `git` commands via g_spawn / GSubprocess; results rendered in a
 * GtkTreeView. Panel widget is added to the main hpaned in main.c.
 */
#ifndef GITPANEL_H
#define GITPANEL_H

#include <gtk/gtk.h>

/* Create and return the Git panel widget. */
GtkWidget *gitpanel_init(GtkWidget *parent_window);

/* Refresh status / log for the active document's repository. */
void       gitpanel_refresh(void);

/* Show / hide the panel. */
void       gitpanel_set_visible(gboolean v);
gboolean   gitpanel_is_visible(void);

/* Public hooks called from editor.c on doc switch or save. */
void       gitpanel_doc_changed(const char *file_path);

#endif /* GITPANEL_H */

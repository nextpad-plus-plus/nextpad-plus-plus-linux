#ifndef DOCMAP_H
#define DOCMAP_H

#include <gtk/gtk.h>

/* Create the panel widget. */
GtkWidget *docmap_init(void);

/* Retarget the map: copy the editor's content into the map's PRIVATE
 * document and apply styles. (Never shares the document — Scintilla's
 * read-only flag is document-level and would poison the real file.) */
void docmap_update(GtkWidget *sci);

/* Called from SCN_UPDATEUI: sync scroll position and redraw viewport rect. */
void docmap_sync_scroll(GtkWidget *sci);

/* Called from SCN_MODIFIED text changes: debounced re-copy (0.25 s,
 * macOS _scheduleContentUpdate parity). No-op unless the map is visible
 * and `sci` is the tracked editor. */
void docmap_notify_modified(GtkWidget *sci);

/* Show / hide the panel. */
void     docmap_set_visible(gboolean v);
gboolean docmap_is_visible(void);

#endif /* DOCMAP_H */

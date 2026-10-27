/*
 * semantic_search.h
 *
 * Bottom search bar for semantic sentence highlighting. The bar is a
 * revealer packed above the status bar, same slot as incremental search.
 * Heavy work runs off the GTK thread.
 */
#ifndef SEMANTIC_SEARCH_H
#define SEMANTIC_SEARCH_H

#include <gtk/gtk.h>
#include "editor.h"

#ifdef __cplusplus
extern "C" {
#endif

GtkWidget *semantic_search_bar_new(void);
void       semantic_search_toggle(void);

/* sci is the editor that changed. Ignored when the bar is closed. */
void semantic_search_note_modified(GtkWidget *sci);
/* Indicator styles are view-local, including clones and the document map. */
void semantic_search_configure_view(GtkWidget *sci);
/* sci is the editor that just became current. */
void semantic_search_note_activated(GtkWidget *sci);
/* Call while doc->sci is still alive. */
void semantic_search_note_closed(NppDoc *doc);

/* Stop indexing and model downloads, then join the worker. Call this
 * before document widgets are destroyed. Safe to call more than once. */
void semantic_search_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

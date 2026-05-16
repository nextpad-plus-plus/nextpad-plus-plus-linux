/*
 * split.h — G14 split view (secondary editor pane sharing documents).
 *
 * MVP: wraps the primary editor container in a GtkPaned. The secondary
 * side hosts its own GtkNotebook with cloned Scintilla widgets that
 * share document buffers with the primary via SCI_SETDOCPOINTER, so edits
 * stay in sync live.
 *
 * Public actions are routed from main.c via these helpers.
 */
#ifndef SPLIT_H
#define SPLIT_H

#include <gtk/gtk.h>

/* Build the split layout. Pass the primary editor container (return value
 * of editor_init). Returns the new outer widget that should be packed in
 * place of the primary container. The secondary pane starts hidden. */
GtkWidget *split_init(GtkWidget *primary, GtkWidget *parent_window);

/* Toggle the secondary pane's visibility. If it has no tabs yet and is
 * being shown, the current primary document is cloned into it. */
void split_toggle(void);
gboolean split_is_visible(void);

/* Clone the currently active primary document into the secondary view as
 * a new tab. */
void split_clone_current(void);

/* Switch keyboard focus between the two notebooks. */
void split_focus_other(void);

/* Close the active secondary tab. */
void split_close_secondary_tab(void);

/* Access to the secondary Scintilla widget (or NULL if no tabs). Used by
 * the editor module when applying preferences. */
GtkWidget *split_secondary_current_sci(void);

/* Apply prefs / styles to every secondary view. Cheap when empty. */
void split_apply_prefs_to_all(void);

#endif

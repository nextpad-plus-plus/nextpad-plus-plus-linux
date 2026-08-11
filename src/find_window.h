#ifndef FIND_WINDOW_H
#define FIND_WINDOW_H

#include <gtk/gtk.h>

/* Q-align: unified 5-tab Find dialog matching macOS FindWindow.
 * Tabs: Find / Replace / Find in Files / Find in Projects / Mark.
 *
 * If `initial_text` is non-NULL, it pre-fills the Find what field. */
typedef enum {
    FW_TAB_FIND          = 0,
    FW_TAB_REPLACE       = 1,
    FW_TAB_FIND_IN_FILES = 2,
    FW_TAB_FIND_IN_PROJ  = 3,
    FW_TAB_MARK          = 4,
} FwTab;

void find_window_show(GtkWindow *parent, FwTab tab, const char *initial_text);

/* GAP-136 — open on the Find in Files tab with `dir` pre-filled (the
 * Project panel's "Find in Projects…" context item). */
void find_window_show_fif_dir(GtkWindow *parent, const char *dir);

/* GAP-141 — open the Replace tab with "In selection" pre-checked. */
void find_window_show_replace_in_selection(GtkWindow *parent);

#endif

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

#endif

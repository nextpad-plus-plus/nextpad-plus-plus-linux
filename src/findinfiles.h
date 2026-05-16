#ifndef FINDINFILES_H
#define FINDINFILES_H

#include <gtk/gtk.h>

/* Show (or raise) the Find in Files dialog.
 * find_text: pre-fill the search entry if non-NULL. */
void findinfiles_show(GtkWidget *parent, const char *find_text);

/* Headless search — spawn the same background worker the OLD Find in
 * Files dialog uses, but route results straight to the dockable
 * Search Results panel (no separate window). Called by the new 5-tab
 * Find window's Find in Files tab.
 *   mode: 0=Normal, 1=Extended, 2=Regex                                 */
void findinfiles_run(const char *needle, const char *directory,
                     const char *filter, gboolean match_case,
                     gboolean whole_word, int mode, gboolean subdirs);

#endif /* FINDINFILES_H */

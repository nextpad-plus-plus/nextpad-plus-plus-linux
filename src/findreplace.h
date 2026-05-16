#ifndef FINDREPLACE_H
#define FINDREPLACE_H

#include <gtk/gtk.h>

/* Show (or raise) the Find/Replace dialog.
 * parent_window: the main application window (used for positioning).
 * find_text:     pre-fill the "Find what" field if non-NULL.
 * show_replace:  TRUE to show the Replace widgets, FALSE for Find-only. */
void findreplace_show(GtkWidget *parent_window, const char *find_text, gboolean show_replace);

/* Must be called whenever the active Scintilla widget changes. */
void findreplace_set_sci(GtkWidget *sci);

/* Repeat the last search forward / backward without opening the dialog. */
void findreplace_find_next(void);
void findreplace_find_prev(void);

/* Q-align: bridge for the unified find_window.c 5-tab dialog. Pushes
 * search parameters into the legacy state so the existing search/replace
 * code paths can be reused. mode: 0=Normal, 1=Extended, 2=Regex. */
void findreplace_set_options(const char *find_text,
                             const char *replace_text,
                             gboolean match_case,
                             gboolean whole_word,
                             gboolean wrap,
                             int search_mode);

/* Public actions called by the 5-tab Find dialog (find_window.c). They
 * read the currently-installed search options (via findreplace_set_options).
 *   - count: total hits in the current document
 *   - find_all_current: collect every hit, feed Search Results panel
 *   - replace_one / replace_all: same semantics as the legacy buttons
 *   - mark_all: paint indicator 31 on every hit (macOS markAllInView)
 *   - clear_marks: remove all indicator 31 ranges                      */
int  findreplace_count(void);
int  findreplace_find_all_current(void);     /* returns total hits      */
void findreplace_replace_one(void);
int  findreplace_replace_all(void);          /* returns replaced count  */
int  findreplace_mark_all(void);             /* returns mark count      */
void findreplace_clear_marks(void);

#endif /* FINDREPLACE_H */

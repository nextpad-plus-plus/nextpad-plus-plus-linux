#ifndef SEARCHRESULTS_H
#define SEARCHRESULTS_H

#include <gtk/gtk.h>

/* Create the panel widget (call once, pack into the window). */
GtkWidget *searchresults_init(void);

/* Called by findinfiles to feed results.
 * Call begin() once, then add_file()/add_hit() per match,
 * then end() to finalise and show the panel.
 * GAP-143: begin() takes the search options for the header's
 * "[Mode: Case/Word]" suffix (mode: 0=Normal 1=Extended 2=Regex
 * 3=Fuzzy); add_hit() takes the first match's byte span within `text`
 * (match_start -1 = no highlight); end() takes the number of files
 * SCANNED for the "of N searched" clause. */
void searchresults_begin(const char *needle, int mode,
                         gboolean match_case, gboolean whole_word);
void searchresults_add_file(const char *filepath, int hit_count);
void searchresults_add_hit(const char *filepath, int line, const char *text,
                           int match_start, int match_len);
void searchresults_end(int total_hits, int total_files, int files_searched);

/* GAP-143 — re-apply the light/dark palette (called from theme_apply). */
void searchresults_apply_theme(void);

/* Show / hide the panel. */
void     searchresults_set_visible(gboolean v);
gboolean searchresults_is_visible(void);

/* GAP-143 — copy the panel selection when the panel view is focused;
 * returns FALSE (caller falls through to the editor) otherwise. */
gboolean searchresults_copy_if_focused(void);

/* GAP-55 — F4/Shift+F4: step to the next (+1) / previous (-1) hit row,
 * jumping the editor there. Returns FALSE when the panel has no hits. */
gboolean searchresults_nav(int dir);

#endif /* SEARCHRESULTS_H */

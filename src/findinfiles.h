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

/* GAP-122 — Replace in Files, one shot (macOS _replaceInFiles: parity).
 * Runs on a worker thread; `done` is invoked on the main thread with
 *   total_replacements — only replacements that actually reached disk
 *   changed_files      — files successfully written
 *   failures           — GPtrArray of char* "path: error" (owned by the
 *                        callee; valid only during the callback)
 * Semantics (macOS 2eaac03 + 6028ce4 + e033363):
 *   - counts credited only after a successful ATOMIC write
 *   - textual no-op replacements ("foo"->"foo", regex (foo)->\1) skip
 *     the write entirely — mtime untouched, 0 reported for that file
 *   - unreadable files are skipped (same as the search side)           */
typedef void (*FifReplaceDone)(int total_replacements, int changed_files,
                               GPtrArray *failures, gpointer user);
void findinfiles_replace_run(const char *needle, const char *replacement,
                             const char *directory, const char *filter,
                             gboolean match_case, gboolean whole_word,
                             int mode, gboolean subdirs,
                             FifReplaceDone done, gpointer user);

#endif /* FINDINFILES_H */

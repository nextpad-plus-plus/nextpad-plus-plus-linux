#ifndef FINDINFILES_H
#define FINDINFILES_H

#include <gtk/gtk.h>

/* Show (or raise) the Find in Files dialog.
 * find_text: pre-fill the search entry if non-NULL. */
void findinfiles_show(GtkWidget *parent, const char *find_text);

/* GAP-141 — per-file progress during the directory walk (macOS
 * _findInFiles progressBlock: cumulative hits + the file just scanned).
 * Fires on the MAIN thread, throttled to keep the loop responsive. */
typedef void (*FifProgress)(const char *file_basename, int hits_so_far,
                            gpointer user);

/* GAP-136 — completion callback shared by directory and path searches. */
typedef void (*FifSearchDone)(int total_hits, int files_with_hits,
                              gpointer user);

/* Headless search — spawn the same background worker the OLD Find in
 * Files dialog uses, but route results straight to the dockable
 * Search Results panel (no separate window). Called by the new 5-tab
 * Find window's Find in Files tab.
 *   mode:   0=Normal, 1=Extended, 2=Regex
 *   hidden: search hidden files / recurse hidden dirs (GAP-141; macOS
 *           isInHiddenDirs — skips BOTH dot-dirs and dot-files when off)
 *   progress/done: optional, main-thread (GAP-141)                      */
void findinfiles_run(const char *needle, const char *directory,
                     const char *filter, gboolean match_case,
                     gboolean whole_word, int mode, gboolean dot_nl,
                     gboolean subdirs, gboolean hidden,
                     FifProgress progress, gpointer progress_user,
                     FifSearchDone done, gpointer done_user);

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
                             int mode, gboolean dot_nl,
                             gboolean subdirs, gboolean hidden,
                             FifReplaceDone done, gpointer user);

/* GAP-136 — Find/Replace in Projects: the same engine over an EXPLICIT
 * file list instead of a directory walk (macOS SearchEngine
 * findInFilePaths:, the "sister function" of findInDirectory:).
 * `paths` is a GPtrArray of char* the callee TAKES OWNERSHIP of (freed
 * with its g_free free-func after the worker finishes). The filter is
 * applied per basename exactly like the directory walk; results feed
 * the shared Search Results panel. `done` (optional) fires on the main
 * thread with (total_hits, files_with_hits) so the caller can post the
 * macOS-style "Find in Projects: N hit(s) in M file(s)." status. */
void findinfiles_run_paths(const char *needle, GPtrArray *paths,
                           const char *filter, gboolean match_case,
                           gboolean whole_word, int mode, gboolean dot_nl,
                           FifSearchDone done, gpointer user);
void findinfiles_replace_run_paths(const char *needle,
                                   const char *replacement,
                                   GPtrArray *paths, const char *filter,
                                   gboolean match_case, gboolean whole_word,
                                   int mode, gboolean dot_nl,
                                   FifReplaceDone done, gpointer user);

#endif /* FINDINFILES_H */

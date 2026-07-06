#ifndef MACROBATCH_H
#define MACROBATCH_H

#include <gtk/gtk.h>

/* "Run Macro on Files…" — batch macro runner (GAP-21). Port of the macOS
 * NPPBatchDialog / NPPBatchRunner pair (efe7a60, BBEdit Text Factory
 * style): pick a macro, pick a scope, per-file open → run → save → close
 * with progress + cancel and a result summary. */

/* Folder-enumeration mode. preselect_folder pre-fills the folder field
 * (used by the workspace-panel context menu); NULL = user browses. */
void macrobatch_show_dialog(GtkWindow *parent, const char *preselect_folder);

/* Fixed-file-list mode (Project panel: workspaces are virtual trees, so
 * there is no folder to enumerate). `files` (char* elements, owned by the
 * caller) is the input set, narrowed only by the extension filter and
 * size cap; recurse/hidden toggles are hidden. source_desc is shown in
 * place of the folder field ("Project: MyProj"). */
void macrobatch_show_dialog_files(GtkWindow *parent, GPtrArray *files,
                                  const char *source_desc);

/* Enumerate files under root honoring the dialog's filters. globs is a
 * space- or ';'-separated pattern list ("*.txt *.md"; empty/NULL = all).
 * Hidden files/dirs are skipped unless include_hidden. max_size in bytes,
 * 0 = unlimited. Returns a GPtrArray of newly-allocated absolute paths
 * (free with g_ptr_array_free(…, TRUE)). Pure — testable headless. */
GPtrArray *macrobatch_enumerate(const char *root, const char *globs,
                                gboolean recurse, gboolean include_hidden,
                                gint64 max_size);

#endif /* MACROBATCH_H */

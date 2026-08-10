#ifndef PROJECT_H
#define PROJECT_H

#include <gtk/gtk.h>

/* Create the Project Manager panel widget. Call once; embed in main layout. */
GtkWidget *project_init(GtkWidget *window);

/* Show/hide the panel. */
void     project_set_visible(gboolean v);
gboolean project_is_visible(void);

/* Open a .nppproject file (NULL = show open dialog). */
void project_open(const char *path);

/* Save the current project to its current path (or show save dialog). */
void project_save(void);

/* Close the current project. */
void project_close(void);

/* Create a new empty project (shows save dialog for name). */
void project_new(void);

/* GAP-136 — Find in Projects accessors (macOS ProjectPanel parity).
 * has_content: SAVED workspace with ≥1 file (macOS rule).
 * file_paths:  every NK_FILE path in the workspace tree; caller frees
 *              the returned GPtrArray (elements freed by its g_free
 *              free-func).
 * file:        the .workspace path, or NULL while unsaved. */
gboolean    project_workspace_has_content(int idx);
GPtrArray  *project_workspace_file_paths(int idx);
const char *project_workspace_file(int idx);

#endif /* PROJECT_H */

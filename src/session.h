#ifndef SESSION_H
#define SESSION_H

#include <glib.h>

/* Serialise all open (saved) tabs to ~/.config/notetux/session.xml.
 * Call before closing tabs so positions are still readable. */
void session_save(void);

/* Reopen tabs from ~/.config/notetux/session.xml and restore scroll/caret.
 * Silently skips files that no longer exist on disk. */
void session_restore(void);
void session_restore_from(const char *path);
void session_set_disabled(gboolean d);

/* Read the saved main-window frame from session.xml without opening any
 * tabs. Returns FALSE if no session file or no frame element. */
gboolean session_get_saved_geometry(int *width, int *height,
                                    int *x, int *y,
                                    gboolean *maximized);

/* Stash the main-window frame so the next session_save() writes it out.
 * Call from the delete-event handler BEFORE session_save() — at that
 * point the GTK toplevel is still alive and reports correct sizes. */
void session_stash_geometry(int width, int height, int x, int y,
                            gboolean maximized);

#endif /* SESSION_H */

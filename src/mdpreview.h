/*
 * mdpreview.h — G29 Markdown preview side panel.
 *
 * Self-contained Markdown → GtkTextView renderer. Supports a useful
 * subset (ATX headings, bold, italic, code, code fences, bullet and
 * ordered lists, block quotes, horizontal rules, hyperlinks). No
 * external dependencies.
 */
#ifndef MDPREVIEW_H
#define MDPREVIEW_H

#include <gtk/gtk.h>

/* Build the panel widget. Call once. */
GtkWidget *mdpreview_init(GtkWidget *parent_win);

/* Refresh from a raw Markdown buffer. Safe to call from any UI thread; will
 * silently no-op if the panel hasn't been built. */
void mdpreview_render(const char *markdown, size_t len);

/* Show / hide. */
void     mdpreview_set_visible(gboolean v);
gboolean mdpreview_is_visible(void);

#endif /* MDPREVIEW_H */

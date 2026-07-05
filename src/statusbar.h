#ifndef STATUSBAR_H
#define STATUSBAR_H

#include <gtk/gtk.h>

GtkWidget *statusbar_init(void);
void       statusbar_update_from_sci(GtkWidget *sci);
void       statusbar_set_language(const char *lang);
void       statusbar_set_encoding(const char *enc);
void       statusbar_set_overtype(gboolean ovr);

/* Plugin-addressable middle field (NPPM_SETSTATUSBAR) — macOS parity
 * commit b5b73b2: every STATUSBAR_* field id routes to this one label;
 * the built-in left/right blocks are never overwritten by plugins. */
void       statusbar_set_plugin_text(const char *text);

/* Register the double-click handler for the language token (macOS
 * issue #174 — opens the Language menu). main.c owns the menu model,
 * so it registers a callback receiving the label to anchor at. */
void       statusbar_set_language_dblclick(void (*cb)(GtkWidget *anchor));

#endif /* STATUSBAR_H */

#ifndef AUTOCOMPLETE_H
#define AUTOCOMPLETE_H

#include <gtk/gtk.h>

/* Call once per sci widget after setup (sets Scintilla autocomplete options). */
void autocomplete_setup(GtkWidget *sci);

/* Call from on_sci_notify when SCN_CHARADDED fires. */
void autocomplete_on_char_added(GtkWidget *sci, int ch);

/* API-backed function-parameter calltips (macOS 60422d1 port).
 * show: manual trigger (menu / typed start-func char);
 * update_ui: track the active parameter as the caret moves;
 * calltip_click: cycle overloads via the \001/\002 arrows. */
void autocomplete_show_calltip(GtkWidget *sci);
void autocomplete_on_update_ui(GtkWidget *sci);
void autocomplete_on_calltip_click(GtkWidget *sci, int arrow);

#endif /* AUTOCOMPLETE_H */

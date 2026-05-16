#ifndef UDL_EDITOR_H
#define UDL_EDITOR_H
#include <gtk/gtk.h>

/* Open the UDL editor dialog modally on top of `parent`. Returns when user
 * closes via [x] / Save / Cancel. The dialog reads / writes UDL XML files
 * in ~/.nextpad++/userDefineLangs/ and (on save) calls udl_reload() so the
 * Language menu picks up changes. */
void udl_editor_show(GtkWindow *parent);

#endif

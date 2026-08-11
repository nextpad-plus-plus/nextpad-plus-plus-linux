#ifndef RUN_H
#define RUN_H

#include <gtk/gtk.h>

/* Show the Run… dialog. Expands the canonical $(VARIABLE) family (11
 * vars, macOS _expandRunVariables) plus the legacy %TOKEN% forms;
 * http(s):// commands open in the browser. Pass NULL for filepath when
 * no file is open. */
void run_dialog(GtkWindow *parent, const char *filepath);

/* Expand + execute a command string (used by the Run menu's saved
 * commands and the dialog's Run button). */
void run_execute(GtkWindow *parent, const char *cmd, const char *filepath);

/* GAP-145 — dynamic Run-menu section listing shortcuts.xml
 * <UserDefinedCommands> (macOS rebuildRunMenu). main.c hands over the
 * empty GMenu section once at menubar build; rebuild re-reads the store,
 * refills the section and re-registers the commands' accelerators. */
void run_menu_set_section(GtkApplication *app, GMenu *section);
void run_menu_rebuild(void);

#endif /* RUN_H */

/*
 * ctxmenu.h — XML-driven editor + tab context menus.
 *
 * Mirrors macOS port's behavior of reading ~/.nextpad++/contextMenu.xml
 * and ~/.nextpad++/tabContextMenu.xml (or tabContextMenu_example.xml).
 *
 * Each <Item> in the XML references a command from the menu bar by
 * (MenuEntryName, MenuItemName). We walk the live GMenuModel once to
 * build a (entry+item → action-name) hashtable, then translate the XML
 * tree into a runtime GtkMenu.
 */
#ifndef CTXMENU_H
#define CTXMENU_H

#include <gtk/gtk.h>
#include <gio/gio.h>
#include "npp_menu.h"

/* Build the lookup index from the live menu model. Call after the menu
 * bar is fully built and before either append function is first called. */
void ctxmenu_index_from_model(GMenuModel *model);

/* Append the editor (Scintilla) right-click items onto `menu`.
 * Returns the number of action items added. */
int ctxmenu_append_scintilla(NppMenu *menu, GtkApplication *app);

/* Append the tab right-click items onto `menu`. Returns the count added
 * (0 → the XML produced nothing; caller should use its fallback set). */
int ctxmenu_append_tab(NppMenu *menu, GtkApplication *app);

/* Built-in tab actions (referenced by <Item BuiltIn="…"/>). The caller
 * wires these handlers to the corresponding GtkMenuItem. */
typedef enum {
    CTXMENU_BUILTIN_NONE,
    CTXMENU_BUILTIN_PINTAB
} CtxMenuBuiltin;

#endif /* CTXMENU_H */

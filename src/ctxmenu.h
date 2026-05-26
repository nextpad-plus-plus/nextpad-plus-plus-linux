/*
 * ctxmenu.h — XML-driven editor + tab right-click menus.
 *
 * The menus are produced as GMenuModels and shown via GtkPopoverMenu so
 * they inherit all the real-menu behaviour from GTK (hover highlight,
 * hover-to-open submenus, side-arrow disclosure, kbd nav, left-aligned
 * rows, identical spacing/padding to the GtkPopoverMenuBar menus). For
 * items that need custom rendering — Apply Color N (coloured square) and
 * spell-check suggestions — we use GtkPopoverMenu's custom-child slots:
 * the GMenu item carries a "custom" attribute naming a slot, and the
 * builder registers a GtkWidget for that slot before popping the popover.
 *
 * Mirrors macOS MainWindowController._buildEditorContextMenuFromXML and
 * NppTabBar._buildTabContextMenuFromXML.
 */
#ifndef CTXMENU_H
#define CTXMENU_H

#include <gtk/gtk.h>
#include <gio/gio.h>

/* Opaque builder — own its GMenu, its custom-child widget list, and a
 * destruction list for caller-owned data (e.g. spell suggestion ctx).
 * Lives until the popover closes; ctxmenu_popup_at takes ownership. */
typedef struct CtxMenu CtxMenu;

/* Build the (top-menu, optional-submenu, item-label) → action-name lookup
 * table from the live English menubar model. Call once after the menubar
 * is built and before the first context menu is shown. */
void ctxmenu_index_from_model(GMenuModel *model);

/* Build a CtxMenu for the editor (Scintilla) right-click from
 * ~/.nextpad++/contextMenu.xml (bundled fallback). */
CtxMenu *ctxmenu_build_scintilla(GtkApplication *app);

/* Build a CtxMenu for the tab right-click from
 * ~/.nextpad++/tabContextMenu.xml (bundled fallback). */
CtxMenu *ctxmenu_build_tab(GtkApplication *app);

/* Show as a GtkPopoverMenu anchored at (x,y) in widget coords.
 * Takes ownership of `m` (it is freed when the popover closes). */
void ctxmenu_popup_at(CtxMenu *m, GtkWidget *anchor, double x, double y);

/* ---- Helpers for callers that need to splice extra items in (spell). */

/* The root GMenu — caller may insert sections at the front (e.g. spell
 * suggestions go above the XML-defined items). */
GMenu *ctxmenu_root(CtxMenu *m);

/* Register a floating GtkWidget as a custom-child of the popover. Returns
 * a slot-id string (owned by the CtxMenu) the caller sets on a GMenuItem
 * via g_menu_item_set_attribute(it, "custom", "s", slot). The widget is
 * adopted at popup time. */
const char *ctxmenu_register_custom(CtxMenu *m, GtkWidget *floating);

/* Attach a piece of caller-owned data whose `free` runs when the
 * CtxMenu is destroyed. */
void ctxmenu_attach_data(CtxMenu *m, gpointer data, GDestroyNotify free);

#endif /* CTXMENU_H */

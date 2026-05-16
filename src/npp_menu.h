/*
 * npp_menu.h — GTK4 popup / context-menu compatibility layer.
 *
 * GTK4 removed GtkMenu / GtkMenuItem / GtkMenuShell. This builds an
 * equivalent popup from a GtkPopover holding a vertical list of flat
 * buttons (separators are GtkSeparator, submenus are GtkMenuButton rows).
 *
 * Item callbacks keep the classic two-argument shape and fire on the
 * button's "clicked" signal, so a menu callback ported from GTK3 only needs
 * its first parameter retyped GtkMenuItem* -> GtkButton* (the body, which
 * almost always ignores it, is unchanged).
 */
#ifndef NPP_MENU_H
#define NPP_MENU_H

#include <gtk/gtk.h>

typedef struct NppMenu NppMenu;

/* Create a top-level popup menu. */
NppMenu   *npp_menu_new(void);

/* Append a labelled item. `cb` is connected to "clicked"; the menu pops
 * down automatically afterwards. Returns the row button (for set_sensitive). */
GtkWidget *npp_menu_add(NppMenu *m, const char *label,
                        GCallback cb, gpointer data);

/* Append a check item. `cb` is connected to "toggled". Returns the
 * GtkCheckButton. The menu does NOT auto-pop-down on a check toggle. */
GtkWidget *npp_menu_add_check(NppMenu *m, const char *label, gboolean active,
                              GCallback cb, gpointer data);

/* Append a separator. */
void       npp_menu_add_separator(NppMenu *m);

/* Append a submenu row; returns a child NppMenu to populate. */
NppMenu   *npp_menu_add_submenu(NppMenu *m, const char *label);

/* The underlying vertical GtkBox — for callers that splice rows in/out. */
GtkWidget *npp_menu_box(NppMenu *m);

/* Pop the menu up. `_at` points at (x,y) in `anchor`'s coordinates;
 * `_at_widget` points at the whole anchor widget. The NppMenu and its
 * widget tree are freed automatically once the popup closes. */
void       npp_menu_popup_at(NppMenu *m, GtkWidget *anchor,
                             double x, double y);
void       npp_menu_popup_at_widget(NppMenu *m, GtkWidget *anchor);

#endif /* NPP_MENU_H */

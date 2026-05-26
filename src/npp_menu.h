/*
 * npp_menu.h — GtkPopoverMenu-backed context menu.
 *
 * Earlier this layer wrapped a raw GtkPopover + GtkBox of GtkButton rows
 * for pixel-perfect macOS NSMenu styling. That approach hit a hard
 * Wayland-protocol limit: xdg-popup grabs require the popup's parent to
 * be the topmost surface; with our hand-rolled popover and parented
 * sub-popovers, the grab was routinely refused (Gdk-WARNING: "Tried to
 * map a grabbing popup with a non-top most parent"), dismissing the
 * popover ~5 ms after popup and randomly freezing the app.
 *
 * GtkPopoverMenu is GTK4's purpose-built widget for context menus. It
 * uses GMenuModel + GtkModelButton internally and handles xdg-popup
 * grab/dismiss correctly — the same widget the menubar's dropdowns use,
 * which is why the menubar has always worked. We build a GMenu, hand it
 * to gtk_popover_menu_new_from_model(), and let GTK do the rest.
 *
 * To keep callsites simple we preserve the add-by-add API; under the
 * hood each item becomes a GMenuItem referencing a per-menu transient
 * GAction (callback-style) or a directly-named app action.
 */
#ifndef NPP_MENU_H
#define NPP_MENU_H

#include <gtk/gtk.h>
#include <gio/gio.h>

typedef struct NppMenu NppMenu;

/* Create a top-level popup menu. */
NppMenu   *npp_menu_new(void);

/* Append a labelled item bound to a per-popup transient GAction. `cb` is
 * invoked with (NULL, data) on activate — callers that used to rely on
 * the GtkButton* first argument should treat it as opaque. Returns an
 * opaque handle that can be passed to npp_menu_item_set_sensitive(). */
gpointer   npp_menu_add(NppMenu *m, const char *label,
                        GCallback cb, gpointer data);

/* Append an item bound to an existing GAction by full name
 * ("app.foo", "win.bar"). The popover dispatches via the running action
 * map — no per-popup adapter, no g_object_set_data dances. */
void       npp_menu_add_action(NppMenu *m, const char *label,
                               const char *action_full_name);

/* Same as above, with a target value for parameterised actions. Takes
 * ownership of `target` (sinks the variant ref). */
void       npp_menu_add_action_target(NppMenu *m, const char *label,
                                      const char *action_full_name,
                                      GVariant *target);

/* Toggle item enabled state. Pass the handle returned by npp_menu_add. */
void       npp_menu_item_set_sensitive(gpointer handle, gboolean sensitive);

/* Append a check item — rendered with a checkmark when active. */
gpointer   npp_menu_add_check(NppMenu *m, const char *label, gboolean active,
                              GCallback cb, gpointer data);

/* Compat: returns a GObject suitable for attaching per-popup data via
 * g_object_set_data_full(). Lifetime is tied to the menu (data freed
 * when the popup closes). Used by spell.c to keep SuggCtx alive. */
GObject   *npp_menu_data_owner(NppMenu *m);

/* Append a separator. Internally starts a new GMenu section. */
void       npp_menu_add_separator(NppMenu *m);

/* Append a submenu row; returns a child NppMenu to populate. */
NppMenu   *npp_menu_add_submenu(NppMenu *m, const char *label);

/* Pop the menu up. `_at` points at (x,y) in `anchor`'s coordinates;
 * `_at_widget` points at the whole anchor widget. The NppMenu and its
 * widget tree are freed automatically once the popup closes. */
void       npp_menu_popup_at(NppMenu *m, GtkWidget *anchor,
                             double x, double y);
void       npp_menu_popup_at_widget(NppMenu *m, GtkWidget *anchor);

#endif /* NPP_MENU_H */

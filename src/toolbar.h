#ifndef TOOLBAR_H
#define TOOLBAR_H

#include <gtk/gtk.h>

/* Build and return the GtkToolbar widget. Call once, embed in the main vbox. */
GtkWidget *toolbar_init(GtkWidget *parent_window);

/* Refresh toggle-button states (wrap, allchars, indent, monitoring) from sci. */
void toolbar_sync_toggles(GtkWidget *sci);

/* Sync panel toggle buttons to actual panel visibility (call after any show/hide). */
void toolbar_sync_panels(void);
/* GAP-74 — plugin-registered toolbar button (grouped after a divider). */
void toolbar_add_plugin_button(const char *icon_path, int cmd_id,
                               const char *tooltip);

/* Enable/disable macro toolbar buttons based on current recording state. */
void toolbar_update_macro_buttons(void);

/* Re-apply the toolbar base line + reload every icon from the light/dark
 * icon set — called when the appearance changes at runtime. */
void toolbar_apply_theme(void);

/* ── GAP-90 — Preferences ▸ Tahoe toolbar-group editor ──────────────── */
enum { NPP_TB_PLACE_PRIMARY = 0, NPP_TB_PLACE_OVERFLOW = 1,
       NPP_TB_PLACE_HIDDEN = 2 };
/* Snapshot the editable rows (built-ins + plugin commands); returns the
 * count, rows readable until the next count call. */
int  toolbar_tahoe_item_count(void);
void toolbar_tahoe_item_get(int i, const char **group, const char **name,
                            int *placement);
/* Persist one placement change (built-ins apply at restart; the Plugins
 * capsule re-splits live). */
void toolbar_tahoe_set_placement(const char *group, const char *name,
                                 int placement);
/* Drop every per-group customization back to defaults. */
void toolbar_tahoe_reset(void);

#endif /* TOOLBAR_H */

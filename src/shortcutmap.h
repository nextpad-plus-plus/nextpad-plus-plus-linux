#ifndef SHORTCUTMAP_H
#define SHORTCUTMAP_H

#include <gtk/gtk.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Shortcut Mapper — 5-tab dialog mirroring macOS ShortcutMapperWindowController
 *
 *   Tab 0  Main menu          (every app.<action> exposed by GApplication)
 *   Tab 1  Macros             (<Macros>             of shortcuts.xml)
 *   Tab 2  Run commands       (<UserDefinedCommands> of shortcuts.xml)
 *   Tab 3  Plugin commands    (every loaded plugin FuncItem, overlaid with
 *                              <PluginCommands> of shortcuts.xml)
 *   Tab 4  Scintilla commands (hardcoded list + <ScintillaKeys> overrides)
 *
 * Main entry point preserved from the previous implementation so main.c
 * still compiles unchanged.
 * ──────────────────────────────────────────────────────────────────────── */

/* Show (or re-raise) the Shortcut Mapper window. Non-modal. */
void shortcut_mapper_show(GtkWidget *parent);

/* GAP-52 — push the user's ScintillaKeys overrides into one editor's
 * live keymap (called on editor creation and after mapper Save). */
void shortcutmap_apply_sci_overrides(GtkWidget *sci);

/* Bind the user's <InternalCommands> overrides as GtkApplication accels.
 * Call at startup after the built-in default accels are set. */
void shortcutmap_apply_main_accels(void);

/* Bind the user's <PluginCommands> as accels on app.plugin-check-<cmdID>.
 * Must run AFTER plugin_load_all(): (moduleName, internalID) can only be
 * resolved to a cmdID once the plugins are loaded. */
void shortcutmap_apply_plugin_accels(void);

/* ── Legacy API kept for ABI compatibility ─────────────────────────────── */

typedef struct {
    const char     *id;
    const char     *label;
    const char     *category;
    guint           default_key;
    GdkModifierType default_mod;
    guint           current_key;
    GdkModifierType current_mod;
    GtkWidget      *widget;
    gpointer        group;   /* GtkAccelGroup removed in GTK4 — unused field */
} ShortcutEntry;

ShortcutEntry *shortcut_table(int *count);
ShortcutEntry *shortcut_find(const char *id);
void shortcut_register(const char *id, GtkWidget *widget, gpointer group);
void shortcut_load(void);
void shortcut_save(void);

#endif /* SHORTCUTMAP_H */

/*
 * pluginsadmin.h — Plugins Admin dialog for Nextpad++ (Linux/GTK3).
 *
 * Mirrors the macOS PluginsAdminWindowController: a non-modal window with
 * four tabs (Available / Updates / Installed / Incompatible), a search
 * filter, and a per-tab action button. See pluginsadmin.c for details on
 * the catalog format and install/update/remove flows.
 */
#ifndef PLUGINSADMIN_H
#define PLUGINSADMIN_H

#include <gtk/gtk.h>

/* Public entry-point — show (or raise) the singleton Plugins Admin window. */
void pluginsadmin_show(GtkWindow *parent);

/* Canonical name used elsewhere in the codebase. Identical to _show — kept
 * so callers that follow the macOS naming style (`open`) compile unchanged.*/
void pluginsadmin_open(GtkWindow *parent);

#endif /* PLUGINSADMIN_H */

/*
 * toolbarconf.h — parser for toolbarButtonsConf.xml (NPP-compatible).
 *
 * Reads ~/.nextpad++/toolbarButtonsConf.xml (falling back to the
 * `_example` variant seeded on first launch) and exposes a single
 * helper used by toolbar.c during construction:
 *
 *   toolbarconf_is_hidden(macos_button_id)  → TRUE if the user has
 *                                              hidden that button.
 *
 * Button IDs match the macOS port (TB_New, TB_Open, TB_Save, …).
 * Toolbar layout itself stays code-defined; the XML only controls
 * per-button visibility.
 */
#ifndef TOOLBARCONF_H
#define TOOLBARCONF_H

#include <glib.h>

/* Load + parse the XML. Idempotent. */
void toolbarconf_init(void);

/* TRUE if the button with this macOS id should be hidden. Unknown ids
 * default to visible. */
gboolean toolbarconf_is_hidden(const char *macos_id);

/* Convenience for the helpers in toolbar.c that build buttons by
 * icon_name. Maps the Linux icon basename (e.g. "new", "open") to the
 * macOS TB_* id (e.g. "TB_New", "TB_Open"). */
const char *toolbarconf_id_for_icon(const char *icon_name);

#endif /* TOOLBARCONF_H */

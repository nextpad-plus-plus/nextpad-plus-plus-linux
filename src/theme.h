/* theme.h — Dark / Light / Auto appearance manager for Nextpad++ (GTK3).
 *
 * Mirrors NppThemeManager from the macOS port (src/NppThemeManager.{h,mm}):
 * a single seam that the rest of the UI calls through whenever appearance
 * needs to be (re-)applied. The Linux port carries far less per-color UI
 * state than the macOS port — most chrome inherits from GtkSettings'
 * `gtk-application-prefer-dark-theme`, and Scintilla styling is driven by
 * stylers XML — so the surface area here is intentionally small:
 *
 *  - flip the GtkSettings dark-mode flag,
 *  - load the matching theme XML (DarkModeDefault.xml in dark, the user's
 *    selected preset otherwise) into stylestore, and
 *  - force a baseline STYLE_DEFAULT fore/back on every open Scintilla so the
 *    editor body never reads through to a stale light background even when a
 *    theme XML lacks an explicit "Default Style" entry.
 *
 * Public API parity with the macOS NppDarkModeOption enum.
 */
#ifndef THEME_H
#define THEME_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mode the user chose. Persisted via prefs as "auto" / "light" / "dark"
 * (mapped to/from the existing int g_prefs.appearance APPEAR_* values). */
typedef enum {
    THEME_AUTO  = 0,
    THEME_LIGHT = 1,
    THEME_DARK  = 2,
} ThemeMode;

/* Apply the current mode to the running app:
 *  - Switches gtk-application-prefer-dark-theme on the default GtkSettings.
 *  - Loads the matching Scintilla theme XML from ~/.nextpad++/themes/ if one
 *    is named "<base>-dark.xml" / "<base>-light.xml", else falls back to
 *    the bundled default + manually overriding STYLE_DEFAULT background.
 *  - Re-applies styles to every open Scintilla via stylestore_apply_global().
 *
 * Call once at startup AFTER prefs load + editor_init, then on every
 * preference change. Safe to call repeatedly. */
void theme_apply(ThemeMode mode);
/* Resolved light/dark after the last theme_apply (GAP-44 UDL blend). */
gboolean theme_effective_dark(void);

/* Convenience: read mode from g_prefs.appearance. */
ThemeMode theme_mode_from_prefs(void);

/* Convenience: persist + apply. */
void theme_set_and_apply(ThemeMode mode);

/* GAP-70 Phase 0 — Tahoe-inspired "Modern" appearance (CSS only,
 * restart-gated on g_prefs.appearance_style). init once after the main
 * window exists; reload is a no-op unless Modern is active and is called
 * automatically from theme_apply on light/dark switches. Evaluation
 * variants: NPP_MODERN_VARIANT=1 (subtle, default) / 2 (stronger) /
 * 3 (flat, no gradient). */
void theme_modern_init(GtkWidget *main_window);
void theme_modern_reload(void);

#ifdef __cplusplus
}
#endif

#endif /* THEME_H */

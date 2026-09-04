/*
 * prefs_stub.c — the terminal editor's stand-in for src/prefs.c.
 *
 * prefs.c is data + the GTK Preferences dialog in one 626-GTK-ref file, so
 * the TUI cannot link it. The headless modules compiled into npp_core
 * (stylestore.c, udl.c, lexer.c, paths.c) read only these pieces of it:
 *
 *   g_prefs.gov_fg/gov_bg/gov_font/gov_font_size/gov_bold/gov_italic/
 *   gov_underline          stylestore.c, udl.c  — Global-override "Force" flags
 *   g_prefs.tab_width, g_prefs.use_tabs
 *                          lexer.c              — per-document indent defaults
 *   g_prefs.backup_custom_dir
 *                          paths.c              — npp_backup_dir() root
 *   prefs_tab_override_for lexer.c              — per-language tab override
 *   theme_effective_dark   udl.c                — GAP-44 UDL colour blend
 *
 * Phase 2 (main.cpp / shim/interop.cpp) fills g_prefs from the GUI's
 * config.xml (tabWidth/useTabs/… — docs/12 §8.3) and may replace the two
 * functions below with real lookups; the defaults here are the GUI's own
 * (prefs.c prefs_defaults) so an unconfigured TUI behaves like a fresh GUI.
 * The proper host fix is a prefs_data.c split (docs/12 §8.6 risk 2).
 */
#include "prefs.h"   /* NPP_TUI → gtk_compat.h headless seam */
/* src/theme.h is shadowed here by tui/shim/theme.h (quoted includes
 * search this directory first); declare the one symbol udl.c needs. */
gboolean theme_effective_dark(void);

NppPrefs g_prefs = {
    .tab_width          = 4,
    .use_tabs           = FALSE,
    .auto_indent        = AUTO_INDENT_ADVANCED,
    .show_indent_guides = TRUE,
    .cjk_wrap           = TRUE,
    .show_line_numbers  = TRUE,
    .edge_mode          = EDGE_NONE,
    .edge_column        = 80,
    .fold_style         = FOLD_BOX_TREE,
    .default_encoding   = "UTF-8",
    .appearance         = APPEAR_AUTO,
    .theme_preset       = "Default",
    /* gov_* = FALSE (zero-init): no Global-override forcing.
     * backup_custom_dir = "" (zero-init): default <user dir>/backup. */
};

/* No per-language override until interop (phase 2) supplies them. */
const TabOverride *prefs_tab_override_for(const char *lang)
{
    (void)lang;
    return NULL;
}

/* The TUI never writes the GUI's config.xml through this path — the
 * Preferences popup (phase 2) does its own read-modify-write. */
void prefs_save(void)
{
}

/* udl.c blends UDL colours toward the effective appearance. Until the
 * theme shim reports the loaded theme's darkness, follow the preference
 * only (APPEAR_AUTO reads as light, matching a GUI with no dark desktop). */
gboolean theme_effective_dark(void)
{
    return g_prefs.appearance == APPEAR_DARK;
}

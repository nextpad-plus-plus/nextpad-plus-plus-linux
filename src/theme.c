/* theme.c — Dark / Light / Auto appearance manager.
 *
 * Wires three layers together:
 *
 *   1. GTK chrome:           gtk-application-prefer-dark-theme via GtkSettings.
 *   2. Desktop "Auto" sense: org.gnome.desktop.interface / color-scheme.
 *   3. Scintilla colors:     load the matching stylers XML into stylestore,
 *                            seed STYLE_DEFAULT fg/bg on every open editor,
 *                            then trigger editor_reapply_styles() so the
 *                            common path (default → STYLECLEARALL → global →
 *                            lexer) re-runs identically to a tab open.
 *
 * Theme-XML resolution order, for current mode m ∈ {dark, light}:
 *
 *   ~/.nextpad++/themes/<preset>-<m>.xml
 *   <resources>/themes/<preset>-<m>.xml
 *   ~/.nextpad++/themes/<preset>.xml
 *   <resources>/themes/<preset>.xml
 *
 * When preset == "Default" / empty:
 *   dark  -> DarkModeDefault.xml (user dir, then bundle)
 *   light -> reload stylers.model.xml via stylestore_load_theme(NULL)
 *
 * The "auto" mode resolves to dark/light via the GNOME color-scheme key;
 * if GIO can't open the schema (older or non-GNOME desktops), we fall
 * back to light — matching the macOS port's behavior when system
 * appearance is unavailable.
 */
#include "theme.h"
#include "gtk_compat.h"
#include "prefs.h"
#include "paths.h"
#include "stylestore.h"
#include "editor.h"
#include "sci_c.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Baseline STYLE_DEFAULT colors                                       */
/* ------------------------------------------------------------------ */

/* Scintilla COLORREF is BGR. Both endpoints here are gray so the byte
 * order doesn't matter, but spelling it out keeps the convention clear. */
#define BGR(r,g,b)   ( (int)((r) & 0xFF) | ((int)((g) & 0xFF) << 8) | ((int)((b) & 0xFF) << 16) )

#define DARK_BG   BGR(0x1E, 0x1E, 0x1E)
#define DARK_FG   BGR(0xD4, 0xD4, 0xD4)
#define LIGHT_BG  BGR(0xFF, 0xFF, 0xFF)
#define LIGHT_FG  BGR(0x00, 0x00, 0x00)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Resolve "auto" to a concrete dark/light using the GNOME color-scheme.
 * Returns TRUE for dark. */
static gboolean auto_is_dark(void)
{
    gboolean dark = FALSE;
    /* g_settings_schema_source_lookup avoids the abort GSettings would
     * trigger if the schema is missing (non-GNOME, minimal install). */
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    if (!src) return FALSE;
    GSettingsSchema *schema = g_settings_schema_source_lookup(
        src, "org.gnome.desktop.interface", TRUE);
    if (!schema) return FALSE;

    /* The "color-scheme" key was added in GNOME 42 (2022). Older schemas
     * lack it; check before reading so we don't trip g_settings_get_string's
     * fatal "no such key" path. */
    if (g_settings_schema_has_key(schema, "color-scheme")) {
        GSettings *s = g_settings_new("org.gnome.desktop.interface");
        if (s) {
            gchar *v = g_settings_get_string(s, "color-scheme");
            if (v && strcmp(v, "prefer-dark") == 0) dark = TRUE;
            g_free(v);
            g_object_unref(s);
        }
    }
    g_settings_schema_unref(schema);
    return dark;
}

/* Try to locate "<preset>-<suffix>.xml" first in the user themes dir,
 * then in the bundled themes dir. Returns NULL if nothing matches; caller
 * owns the returned string. Pass suffix == NULL for the plain "<preset>.xml"
 * shape. */
static gchar *find_theme_xml(const char *preset, const char *suffix)
{
    if (!preset || !*preset) return NULL;
    gchar *fname = suffix
        ? g_strdup_printf("%s-%s.xml", preset, suffix)
        : g_strdup_printf("%s.xml", preset);

    gchar *user = npp_user_file("themes", fname);
    if (user && g_file_test(user, G_FILE_TEST_EXISTS)) { g_free(fname); return user; }
    g_free(user);

    gchar *bundle = npp_bundle_file("themes", fname);
    if (bundle && g_file_test(bundle, G_FILE_TEST_EXISTS)) { g_free(fname); return bundle; }
    g_free(bundle);

    g_free(fname);
    return NULL;
}

/* Seed STYLE_DEFAULT fg+bg on a single Scintilla so the editor body is
 * always at the right baseline before the lexer re-applies. We deliberately
 * do NOT send SCI_STYLECLEARALL here — editor_reapply_styles() does that
 * right after, and we'd otherwise wipe the per-style entries we just set. */
static void seed_default_style(GtkWidget *sci, int fg, int bg)
{
    if (!sci || !SCINTILLA_IS_OBJECT(sci)) return;
    scintilla_send_message(SCINTILLA(sci), SCI_STYLESETFORE,
                           (uptr_t)STYLE_DEFAULT, (sptr_t)fg);
    scintilla_send_message(SCINTILLA(sci), SCI_STYLESETBACK,
                           (uptr_t)STYLE_DEFAULT, (sptr_t)bg);
}

/* Walk every open editor tab and seed STYLE_DEFAULT. editor.c is the only
 * notebook owner; we reach pages through the public getter to avoid taking
 * a copy of its file-scope s_notebook here. */
static void seed_all_editors(int fg, int bg)
{
    GtkWidget *nb = editor_get_notebook();
    if (!nb) return;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb));
    for (int i = 0; i < n; i++) {
        GtkWidget *sci = gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), i);
        seed_default_style(sci, fg, bg);
    }
}

/* Pick & load the right Scintilla theme XML for `dark` mode given the
 * current g_prefs.theme_preset. Returns TRUE if a theme file was loaded
 * (caller can skip the baseline seed in that case — the theme's own
 * "Default Style" entry will provide colors). FALSE means we fell back to
 * the bundled model and the caller must seed STYLE_DEFAULT manually. */
static gboolean load_theme_for_mode(gboolean dark)
{
    const char *preset = g_prefs.theme_preset;
    const gboolean is_default = (!preset[0] || strcmp(preset, "Default") == 0);

    /* "Default" preset: dark → DarkModeDefault, light → bundled model. */
    if (is_default) {
        if (dark) {
            gchar *p = find_theme_xml("DarkModeDefault", NULL);
            if (p) { stylestore_load_theme(p); g_free(p); return TRUE; }
            return FALSE;  /* dark requested but no DarkModeDefault — seed only */
        }
        stylestore_load_theme(NULL);   /* light = stylers.model.xml */
        return TRUE;
    }

    /* Named preset: try the mode-specific variant first, fall through to
     * the plain "<preset>.xml" so themes that ship without dark/light
     * splits still work. */
    const char *suffix = dark ? "dark" : "light";
    gchar *p = find_theme_xml(preset, suffix);
    if (!p) p = find_theme_xml(preset, NULL);
    if (p) { stylestore_load_theme(p); g_free(p); return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void theme_apply(ThemeMode mode)
{
    /* 1) Resolve effective dark/light. */
    gboolean dark;
    switch (mode) {
        case THEME_DARK:  dark = TRUE;  break;
        case THEME_LIGHT: dark = FALSE; break;
        case THEME_AUTO:
        default:          dark = auto_is_dark(); break;
    }

    /* 2) Flip GtkSettings so all GTK chrome (menus, dialogs, scrollbars,
     *    headerbars, etc.) repaints into the right palette. */
    GtkSettings *s = gtk_settings_get_default();
    if (s) g_object_set(s, "gtk-application-prefer-dark-theme", dark, NULL);

    /* 3) Load matching Scintilla theme. */
    gboolean theme_loaded = load_theme_for_mode(dark);

    /* 4) Seed STYLE_DEFAULT on every open editor whenever the loaded theme
     *    didn't already carry a "Default Style" entry — keeps the editor
     *    body from rendering on a white background while in dark mode. The
     *    theme's own entries still take precedence, because the next call
     *    to editor_reapply_styles() runs stylestore_apply_default() first. */
    if (!theme_loaded) {
        seed_all_editors(dark ? DARK_FG : LIGHT_FG,
                         dark ? DARK_BG : LIGHT_BG);
    }

    /* 5) Re-run the standard styling pipeline on every tab. */
    editor_reapply_styles();
}

ThemeMode theme_mode_from_prefs(void)
{
    /* g_prefs.appearance is the existing int (APPEAR_AUTO/LIGHT/DARK).
     * The enum values line up by design — keep the explicit switch so a
     * future renumber of either side breaks the build, not the runtime. */
    switch (g_prefs.appearance) {
        case APPEAR_DARK:  return THEME_DARK;
        case APPEAR_LIGHT: return THEME_LIGHT;
        case APPEAR_AUTO:
        default:           return THEME_AUTO;
    }
}

void theme_set_and_apply(ThemeMode mode)
{
    switch (mode) {
        case THEME_DARK:  g_prefs.appearance = APPEAR_DARK;  break;
        case THEME_LIGHT: g_prefs.appearance = APPEAR_LIGHT; break;
        case THEME_AUTO:
        default:          g_prefs.appearance = APPEAR_AUTO;  break;
    }
    prefs_save();
    theme_apply(mode);
}

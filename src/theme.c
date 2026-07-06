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

/* Effective dark/light of the last theme_apply — the Modern CSS follows
 * it (GAP-70). */
static gboolean s_effective_dark = FALSE;

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
    s_effective_dark = dark;

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

    /* 6) Keep the Modern appearance CSS (if active) in the right palette. */
    theme_modern_reload();
}

/* ================================================================== */
/* GAP-70 — Tahoe-inspired "Modern" appearance (CSS side).             */
/*                                                                     */
/* Restart-gated: nothing here runs unless appearance_style == 1, so   */
/* Classic stays byte-for-byte untouched. Matches the macOS Tahoe      */
/* screenshot: FLAT light-gray chrome (no gradient), the toolbar as    */
/* white capsule cards per group with a label-▾ overflow underneath    */
/* (built by toolbar.c), a flat tab strip with rounded-top tabs and a  */
/* warm active-tab tint, and an edge-to-edge editor. Colored tabs get  */
/* the full-tab tint instead of Classic's 3px stripe.                  */
/* ================================================================== */

static GtkCssProvider *s_modern_css = NULL;

/* The macOS tab-colour palette (also used by the Classic 3px stripes). */
static const char *const kTabTint[5] =
    { "#FCE386", "#A9F08C", "#7AC9F5", "#F5B67A", "#F08CF0" };

void theme_modern_reload(void)
{
    if (!s_modern_css) return;   /* Classic, or not initialised yet */
    const gboolean dark = s_effective_dark;

    /* Flat Tahoe palette. */
    const char *win_bg      = dark ? "#242427" : "#f2f2f4";
    const char *capsule_bg  = dark ? "#333338" : "#ffffff";
    const char *capsule_bd  = dark ? "alpha(#ffffff, 0.09)" : "alpha(#1a2233, 0.08)";
    const char *shadow      = dark ? "alpha(#000000, 0.35)" : "alpha(#1a2233, 0.10)";
    const char *label_fg    = dark ? "alpha(#e8eaf2, 0.70)" : "alpha(#3c3c43, 0.75)";
    const char *tab_bg      = dark ? "alpha(#ffffff, 0.05)" : "alpha(#ffffff, 0.55)";
    const char *tab_active  = dark ? "#5c4a33"              : "#f6e0c6";
    double      tint_alpha  = dark ? 0.35 : 0.55;

    GString *css = g_string_new(NULL);

    /* Flat window chrome — scoped to the main window only. */
    g_string_append_printf(css,
        "window.npp-modern { background: %s; }\n", win_bg);

    /* Toolbar container: transparent, just spacing — the capsules carry
     * the chrome. Kills Classic's bottom hairline via provider priority. */
    g_string_append(css,
        ".npp-modern .npp-toolbar {\n"
        "  background: transparent; border: none; box-shadow: none;\n"
        "  padding: 6px 8px 4px 8px;\n"
        "}\n");

    /* Capsule cards. */
    g_string_append_printf(css,
        ".npp-modern .npp-capsule {\n"
        "  background: %s;\n"
        "  border: 1px solid %s;\n"
        "  border-radius: 10px;\n"
        "  box-shadow: 0 1px 2px %s;\n"
        "  margin: 0 3px;\n"
        "  padding: 2px 6px 0px 6px;\n"
        "}\n"
        ".npp-modern .npp-capsule button {\n"
        "  padding: 2px 5px; margin: 0 1px; border-radius: 6px;\n"
        "  background: none; border: none; box-shadow: none;\n"
        "}\n"
        ".npp-modern .npp-capsule button:hover { background: alpha(%s, 0.5); }\n"
        ".npp-modern .npp-capsule button:checked { background: alpha(%s, 0.9); }\n",
        capsule_bg, capsule_bd, shadow,
        dark ? "#ffffff" : "#d8dce6",
        dark ? "#5a5a66" : "#d0d8ea");

    /* Capsule group label (plain GtkLabel, or the label-▾ GtkMenuButton). */
    g_string_append_printf(css,
        ".npp-modern .npp-capsule-label,\n"
        ".npp-modern menubutton.npp-capsule-label > button {\n"
        "  font-size: 10.5px;\n"
        "  color: %s;\n"
        "  min-height: 14px;\n"
        "  padding: 0 2px; margin: 0;\n"
        "  background: none; border: none; box-shadow: none;\n"
        "}\n"
        ".npp-modern menubutton.npp-capsule-label arrow {\n"
        "  min-height: 8px; min-width: 8px; -gtk-icon-size: 8px;\n"
        "}\n",
        label_fg);

    /* Flat tab strip: rounded-top tabs on the flat chrome, warm tint on
     * the active tab (screenshot's peach), hairline borders. */
    g_string_append_printf(css,
        ".npp-modern notebook.npp-editor-tabs > header {\n"
        "  background: %s; border: none; box-shadow: none;\n"
        "}\n"
        ".npp-modern notebook.npp-editor-tabs > header > tabs > tab {\n"
        "  background: %s;\n"
        "  background-image: none;\n"           /* kill Classic's gradient */
        "  border: 1px solid %s;\n"
        "  border-bottom: none;\n"
        "  border-radius: 8px 8px 0 0;\n"
        "  margin: 3px 1px 0 1px;\n"
        "  padding: 1px 10px;\n"
        "}\n"
        ".npp-modern notebook.npp-editor-tabs > header > tabs > tab:hover {\n"
        "  background-image: none;\n"
        "  background-color: alpha(%s, 0.75);\n"
        "}\n"
        /* Active tab: FULL warm tint, no stripe — the orange top line was
         * Classic's inset box-shadow leaking through (user-rejected). */
        ".npp-modern notebook.npp-editor-tabs > header > tabs > tab:checked {\n"
        "  background: %s;\n"
        "  background-image: none;\n"
        "  box-shadow: none;\n"
        "}\n",
        win_bg, tab_bg, capsule_bd,
        dark ? "#4a4a52" : "#ffffff",
        tab_active);

    /* Full-tab colour tint (Tahoe) instead of Classic's 3px stripe. The
     * selectors out-specify the stripe rules from install_tab_color_css. */
    for (int i = 0; i < 5; i++) {
        g_string_append_printf(css,
            ".npp-modern notebook.npp-editor-tabs > header > tabs > tab.tab-color-%d,\n"
            ".npp-modern notebook.npp-editor-tabs > header > tabs > tab.tab-color-%d:checked {\n"
            "  box-shadow: none;\n"
            "  background: alpha(%s, %.2f);\n"
            "}\n",
            i + 1, i + 1, kTabTint[i], tint_alpha);
    }

    gtk_css_provider_load_from_data(s_modern_css, css->str, -1);
    g_string_free(css, TRUE);
}

void theme_modern_init(GtkWidget *main_window)
{
    if (g_prefs.appearance_style != 1) return;   /* Classic — do nothing */

    if (main_window)
        gtk_widget_add_css_class(main_window, "npp-modern");

    s_modern_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(s_modern_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    theme_modern_reload();
    g_message("theme: Modern appearance active (Tahoe capsules)");
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

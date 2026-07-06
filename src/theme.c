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
/* GAP-70 Phase 0 — Tahoe-inspired "Modern" appearance (CSS only).     */
/*                                                                     */
/* Restart-gated: nothing here runs unless appearance_style == 1, so   */
/* Classic stays byte-for-byte untouched. The glass *material* is not  */
/* portable (GTK4 has no backdrop blur) — this is the honest flat      */
/* interpretation: gradient backdrop, toolbar pill, flat tab strip     */
/* with full-tab colour tint, rounded editor card.                     */
/*                                                                     */
/* Evaluation variants (pick per launch, no rebuild):                  */
/*   NPP_MODERN_VARIANT=1  subtle gradient      (default)              */
/*   NPP_MODERN_VARIANT=2  stronger tint/colour                        */
/*   NPP_MODERN_VARIANT=3  flat — no gradient, GNOME-ish neutral       */
/* ================================================================== */

static GtkCssProvider *s_modern_css = NULL;
static int             s_modern_variant = 1;

/* One palette entry per (variant, light/dark). */
typedef struct {
    const char *backdrop;      /* window background (gradient or solid) */
    const char *chrome;        /* pill/card chrome base colour          */
    const char *chrome_border;
    double      pill_alpha;    /* toolbar pill fill                     */
    double      tab_alpha;     /* inactive tab fill                     */
    double      tint_alpha;    /* full-tab colour tint                  */
    const char *paper;         /* editor card bg behind the sci corners */
} ModernPalette;

static ModernPalette modern_palette(gboolean dark, int variant)
{
    ModernPalette p;
    p.chrome        = dark ? "#e8eaf2" : "#ffffff";
    p.chrome_border = dark ? "#000000" : "#1a2233";
    p.paper         = dark ? "#1e1e1e" : "#ffffff";
    p.pill_alpha    = dark ? 0.08 : 0.55;
    p.tab_alpha     = dark ? 0.06 : 0.35;
    p.tint_alpha    = 0.40;
    switch (variant) {
        default:
        case 1:   /* subtle diagonal gradient */
            p.backdrop = dark
                ? "linear-gradient(135deg, #23262d 0%, #22242b 45%, #2a2731 100%)"
                : "linear-gradient(135deg, #eef2f7 0%, #e9edf5 45%, #f3efe8 100%)";
            break;
        case 2:   /* stronger colour + chrome */
            p.backdrop = dark
                ? "linear-gradient(135deg, #1f2633 0%, #2a2238 50%, #332632 100%)"
                : "linear-gradient(135deg, #dfe7f5 0%, #e6def2 50%, #f2e5da 100%)";
            p.pill_alpha += dark ? 0.06 : 0.15;
            p.tab_alpha  += dark ? 0.05 : 0.15;
            p.tint_alpha  = 0.60;
            break;
        case 3:   /* flat neutral, GNOME-ish */
            p.backdrop = dark ? "#24262b" : "#f0f1f4";
            break;
    }
    return p;
}

/* The macOS tab-colour palette (also used by the Classic 3px stripes). */
static const char *const kTabTint[5] =
    { "#FCE386", "#A9F08C", "#7AC9F5", "#F5B67A", "#F08CF0" };

void theme_modern_reload(void)
{
    if (!s_modern_css) return;   /* Classic, or not initialised yet */
    const ModernPalette P = modern_palette(s_effective_dark, s_modern_variant);

    GString *css = g_string_new(NULL);

    /* Window backdrop — scoped to the main window only. */
    g_string_append_printf(css,
        "window.npp-modern { background: %s; }\n", P.backdrop);

    /* Toolbar as one rounded pill (Phase 1 would split it into per-group
     * capsules). Wins over toolbar.c's own CSS via provider priority. */
    g_string_append_printf(css,
        ".npp-modern .npp-toolbar {\n"
        "  background: alpha(%s, %.2f);\n"
        "  border: 1px solid alpha(%s, 0.10);\n"
        "  border-radius: 14px;\n"
        "  margin: 6px 8px 3px 8px;\n"
        "  padding: 2px 6px;\n"
        "}\n",
        P.chrome, P.pill_alpha, P.chrome_border);

    /* Flat transparent tab strip; tabs become small pills. */
    g_string_append_printf(css,
        ".npp-modern notebook.npp-editor-tabs > header {\n"
        "  background: transparent; border: none; box-shadow: none;\n"
        "}\n"
        ".npp-modern notebook.npp-editor-tabs > header > tabs > tab {\n"
        "  background: alpha(%s, %.2f);\n"
        "  border: none;\n"
        "  border-radius: 8px;\n"
        "  margin: 3px 2px;\n"
        "  padding: 1px 10px;\n"
        "}\n"
        ".npp-modern notebook.npp-editor-tabs > header > tabs > tab:checked {\n"
        "  background: alpha(%s, %.2f);\n"
        "  box-shadow: 0 1px 2px alpha(%s, 0.18);\n"
        "}\n",
        P.chrome, P.tab_alpha,
        P.chrome, MIN(P.tab_alpha * 2.6, 0.95), P.chrome_border);

    /* Full-tab colour tint (Tahoe) instead of Classic's 3px stripe. The
     * selectors out-specify the stripe rules from install_tab_color_css. */
    for (int i = 0; i < 5; i++) {
        g_string_append_printf(css,
            ".npp-modern notebook.npp-editor-tabs > header > tabs > tab.tab-color-%d,\n"
            ".npp-modern notebook.npp-editor-tabs > header > tabs > tab.tab-color-%d:checked {\n"
            "  box-shadow: none;\n"
            "  background: alpha(%s, %.2f);\n"
            "}\n",
            i + 1, i + 1, kTabTint[i], P.tint_alpha);
    }

    /* Editor "card": rounded, bordered, floating on the backdrop. The
     * ScintillaView itself still paints square corners — the 2px padding
     * in the card's own paper colour makes the bleed invisible on stock
     * themes (prototype limitation, noted for Phase 1). */
    g_string_append_printf(css,
        ".npp-modern notebook.npp-editor-tabs > stack {\n"
        "  margin: 0 8px 8px 8px;\n"
        "  padding: 2px;\n"
        "  border: 1px solid alpha(%s, 0.12);\n"
        "  border-radius: 12px;\n"
        "  background: %s;\n"
        "}\n",
        P.chrome_border, P.paper);

    gtk_css_provider_load_from_data(s_modern_css, css->str, -1);
    g_string_free(css, TRUE);
}

void theme_modern_init(GtkWidget *main_window)
{
    if (g_prefs.appearance_style != 1) return;   /* Classic — do nothing */

    const char *v = g_getenv("NPP_MODERN_VARIANT");
    if (v && (*v == '2' || *v == '3')) s_modern_variant = *v - '0';

    if (main_window)
        gtk_widget_add_css_class(main_window, "npp-modern");

    s_modern_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(s_modern_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    theme_modern_reload();
    g_message("theme: Modern appearance active (variant %d)", s_modern_variant);
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

/* toolbar.c — GTK4 toolbar for the Linux port.
 * Ports the toolbar structure from MainWindowController.mm (toolbarDescriptors).
 * Uses the same Fluent PNG icons (light/dark) as the macOS version.
 *
 * GTK4: GtkToolbar/GtkToolItem/GtkToolButton were removed. The toolbar is now
 * a horizontal GtkBox of GtkButton / GtkToggleButton widgets with GtkSeparator
 * dividers — the standard GTK4 idiom for a button strip.
 */
#include "toolbar.h"
#include "gtk_compat.h"
#include "editor.h"
#include "findreplace.h"
#include "macro.h"
#include "doclist.h"
#include "docmap.h"
#include "workspace.h"
#include "funclist.h"
#include "sci_c.h"
#include "toolbarconf.h"
#include "prefs.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* SCI view-toggle constants                                           */
/* ------------------------------------------------------------------ */
#define SCI_ZOOMIN              2333
#define SCI_ZOOMOUT             2334
/* SCI_SETWRAPMODE / SCI_GETWRAPMODE / SC_WRAP_* defined in sci_c.h */
/* SCI_SETVIEWWS / SCI_GETVIEWWS defined in sci_c.h */
#define SCI_SETINDENTATIONGUIDES 2132
#define SCI_GETINDENTATIONGUIDES 2133
/* SC_WS_INVISIBLE / SC_WS_VISIBLEALWAYS defined in sci_c.h */
#define SC_IV_NONE              0
#define SC_IV_LOOKBOTH          3

/* ------------------------------------------------------------------ */
/* Icon size — base 24px @ 100 % matching macOS 28pt button geometry.   */
/* ------------------------------------------------------------------ */
#define ICON_BASE_PX 24
static double s_icon_scale = 1.0;
static int icon_px(void) {
    int px = (int)(ICON_BASE_PX * s_icon_scale + 0.5);
    if (px < 8)  px = 8;
    if (px > 96) px = 96;
    return px;
}

#ifndef RESOURCES_DIR
#define RESOURCES_DIR "../../resources"
#endif

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */
static GtkWidget *s_window           = NULL;
static GtkWidget *s_toolbar          = NULL;  /* the toolbar box, for theme reloads */
static GtkWidget *s_btn_undo         = NULL;
static GtkWidget *s_btn_redo         = NULL;
static GtkWidget *s_btn_save         = NULL;
static GtkWidget *s_tgl_wrap         = NULL;
static GtkWidget *s_tgl_allchars     = NULL;
static GtkWidget *s_tgl_indent       = NULL;
static GtkWidget *s_btn_startrecord  = NULL;
static GtkWidget *s_btn_stoprecord   = NULL;
static GtkWidget *s_btn_play         = NULL;
static GtkWidget *s_btn_playn        = NULL;
static GtkWidget *s_btn_saverecord   = NULL;
/* Panel toggle buttons */
static GtkWidget *s_tgl_doclist      = NULL;
static GtkWidget *s_tgl_docmap       = NULL;
static GtkWidget *s_tgl_workspace    = NULL;
static GtkWidget *s_tgl_funclist     = NULL;
static GtkWidget *s_tgl_monitoring   = NULL;

/* ------------------------------------------------------------------ */
/* Dark mode detection (mirrors NppThemeManager logic)                */
/* ------------------------------------------------------------------ */
static gboolean is_dark_mode(void)
{
    GtkSettings *s = gtk_settings_get_default();
    gboolean dark = FALSE;
    g_object_get(s, "gtk-application-prefer-dark-theme", &dark, NULL);
    if (!dark) {
        gchar *name = NULL;
        g_object_get(s, "gtk-theme-name", &name, NULL);
        if (name) {
            gchar *lower = g_ascii_strdown(name, -1);
            dark = (strstr(lower, "dark") != NULL);
            g_free(lower);
            g_free(name);
        }
    }
    return dark;
}

/* ------------------------------------------------------------------ */
/* Icon loading — same PNG set as macOS.                               */
/*                                                                      */
/* The source icons are 96×96. We must NOT pre-downscale them to a      */
/* fixed pixbuf: in GTK4 a pixbuf becomes a fixed-size GdkTexture that   */
/* the GSK renderer then resamples again at draw time (linear filter,   */
/* fractional row-centring) — a second lossy step that softens the      */
/* icons. GTK3 avoided this because GtkImage blitted the pixbuf 1:1 via  */
/* Cairo. Instead, hand GTK4 the full-resolution texture and let         */
/* GtkImage downsample it ONCE, at the true device-pixel size           */
/* (scale-factor aware), via gtk_image_set_pixel_size().                 */
/* ------------------------------------------------------------------ */
/* GAP-46 — resolve the colorization target (macOS _nppToolbarTargetColor;
 * palette = Windows fluent list). "System Accent" has no portable GTK4
 * query on this libadwaita, so it maps to the stock fluent blue. */
static void toolbar_target_rgb(double *r, double *g, double *b)
{
    static const struct { double r, g, b; } kPal[7] = {
        { 0xE8/255.0, 0x11/255.0, 0x23/255.0 },   /* red    */
        { 0x00/255.0, 0x8B/255.0, 0x00/255.0 },   /* green  */
        { 0x00/255.0, 0x78/255.0, 0xD4/255.0 },   /* blue   */
        { 0xB1/255.0, 0x46/255.0, 0xC2/255.0 },   /* purple */
        { 0x00/255.0, 0xB7/255.0, 0xC3/255.0 },   /* cyan   */
        { 0x49/255.0, 0x82/255.0, 0x05/255.0 },   /* olive  */
        { 0xFF/255.0, 0xB9/255.0, 0x00/255.0 },   /* yellow */
    };
    int c = g_prefs.toolbar_color_choice;
    if (c >= 0 && c <= 6) { *r = kPal[c].r; *g = kPal[c].g; *b = kPal[c].b; return; }
    if (c == 8) {
        GdkRGBA rgba;
        if (gdk_rgba_parse(&rgba, g_prefs.toolbar_color_custom)) {
            *r = rgba.red; *g = rgba.green; *b = rgba.blue; return;
        }
    }
    *r = 0x00/255.0; *g = 0x78/255.0; *b = 0xD4/255.0;   /* accent fallback */
}

/* Colorize a pixbuf in place. Partial: repaint saturated (accent) pixels
 * with the target hue, leaving greys/blacks; Complete: solid mono fill
 * over the alpha mask (macOS a2ee1ab / Windows "fluent" parity). */
static void toolbar_colorize_pixbuf(GdkPixbuf *pb)
{
    if (g_prefs.toolbar_color_mode == 0 || !gdk_pixbuf_get_has_alpha(pb))
        return;
    double tr, tg, tb;
    toolbar_target_rgb(&tr, &tg, &tb);

    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    gboolean complete = g_prefs.toolbar_color_mode == 2;

    for (int y = 0; y < h; y++) {
        guchar *row = px + (gsize)y * stride;
        for (int x = 0; x < w; x++) {
            guchar *p = row + x * 4;
            if (p[3] == 0) continue;
            if (complete) {
                p[0] = (guchar)(tr * 255); p[1] = (guchar)(tg * 255);
                p[2] = (guchar)(tb * 255);
                continue;
            }
            /* Partial: saturation test — recolor only vivid pixels. */
            int mx = MAX(p[0], MAX(p[1], p[2]));
            int mn = MIN(p[0], MIN(p[1], p[2]));
            if (mx == 0 || (mx - mn) * 255 < mx * 64) continue;  /* grey */
            double lum = mx / 255.0;   /* keep the pixel's brightness */
            p[0] = (guchar)(tr * lum * 255);
            p[1] = (guchar)(tg * lum * 255);
            p[2] = (guchar)(tb * lum * 255);
        }
    }
}

static GtkWidget *load_icon(const char *name)
{
    char path[512];
    gboolean dark = is_dark_mode();
    GError *err = NULL;
    GdkPixbuf *pb = NULL;

    /* GAP-47 — "Standard icons" pref: the mode-agnostic classic set
     * takes priority when enabled (macOS fbf4a86). */
    if (g_prefs.toolbar_standard_icons) {
        snprintf(path, sizeof(path),
                 RESOURCES_DIR "/icons/standard/toolbar/%s.png", name);
        pb = gdk_pixbuf_new_from_file(path, NULL);
    }
    if (!pb) {
        snprintf(path, sizeof(path),
                 RESOURCES_DIR "/icons/%s/toolbar/regular/%s_off.png",
                 dark ? "dark" : "light", name);
        pb = gdk_pixbuf_new_from_file(path, &err);
    }
    if (!pb) {
        if (err) g_clear_error(&err);
        /* Fallback: try standard/ (16×16 classic icons) */
        snprintf(path, sizeof(path),
                 RESOURCES_DIR "/icons/standard/toolbar/%s.png", name);
        pb = gdk_pixbuf_new_from_file(path, &err);
        if (!pb) {
            if (err) g_clear_error(&err);
            GtkWidget *mi = gtk_image_new_from_icon_name("image-missing");
            gtk_image_set_pixel_size(GTK_IMAGE(mi), icon_px());
            return mi;
        }
    }

    /* Colorization needs RGBA pixels. */
    if (!gdk_pixbuf_get_has_alpha(pb)) {
        GdkPixbuf *a = gdk_pixbuf_add_alpha(pb, FALSE, 0, 0, 0);
        g_object_unref(pb);
        pb = a;
    }
    toolbar_colorize_pixbuf(pb);

    GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
    g_object_unref(pb);
    GtkWidget *img = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
    g_object_unref(tex);
    /* Render the high-res texture at the logical icon size; GTK4 applies
     * the display scale factor on top, so it stays crisp on HiDPI. */
    gtk_image_set_pixel_size(GTK_IMAGE(img), icon_px());
    return img;
}

/* Apply the toolbarButtonsConf.xml visibility decision: when the user has
 * hidden a button, we still construct the widget (so static refs in this
 * file remain valid for sensitivity updates) but mark it hidden. */
static void apply_toolbarconf_visibility(GtkWidget *item, const char *icon_name) {
    const char *id = toolbarconf_id_for_icon(icon_name);
    if (id && toolbarconf_is_hidden(id))
        gtk_widget_set_visible(item, FALSE);
}

/* Convenience: GtkButton with icon + tooltip. The icon name is stashed on
 * the widget so toolbar_apply_theme() can reload it from the light/dark
 * set when the appearance changes. */
static GtkWidget *make_btn(const char *icon_name, const char *tooltip,
                           GCallback cb, gpointer data)
{
    GtkWidget *item = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(item), load_icon(icon_name));
    gtk_button_set_has_frame(GTK_BUTTON(item), FALSE);
    gtk_widget_set_tooltip_text(item, tooltip);
    g_object_set_data_full(G_OBJECT(item), "npp-icon",
                           g_strdup(icon_name), g_free);
    if (cb)
        g_signal_connect(item, "clicked", cb, data);
    apply_toolbarconf_visibility(item, icon_name);
    return item;
}

static GtkWidget *make_toggle(const char *icon_name, const char *tooltip,
                              GCallback cb, gpointer data)
{
    GtkWidget *item = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(item), load_icon(icon_name));
    gtk_button_set_has_frame(GTK_BUTTON(item), FALSE);
    gtk_widget_set_tooltip_text(item, tooltip);
    g_object_set_data_full(G_OBJECT(item), "npp-icon",
                           g_strdup(icon_name), g_free);
    if (cb)
        g_signal_connect(item, "toggled", cb, data);
    apply_toolbarconf_visibility(item, icon_name);
    return item;
}

static GtkWidget *make_sep(void)
{
    return gtk_separator_new(GTK_ORIENTATION_VERTICAL);
}

/* ------------------------------------------------------------------ */
/* Button callbacks — mirror macOS toolbar actions                    */
/* ------------------------------------------------------------------ */

static void on_new    (GtkButton *i, gpointer d) { (void)i;(void)d; editor_new_doc(); }
static void on_open   (GtkButton *i, gpointer d) { (void)i;(void)d; editor_open_dialog(); }
static void on_save   (GtkButton *i, gpointer d) { (void)i;(void)d; editor_save(); }

static void on_save_all(GtkButton *i, gpointer d)
{
    (void)i; (void)d;
    int n = editor_page_count();
    for (int p = 0; p < n; p++) {
        NppDoc *doc = editor_doc_at(p);
        if (doc && doc->modified)
            editor_save_at(p);
    }
}

static void on_close(GtkButton *i, gpointer d)    { (void)i;(void)d; editor_close_page(-1); }

static void on_close_all(GtkButton *i, gpointer d)
{
    (void)i; (void)d;
    /* Close tabs from right to left so indices stay valid */
    while (editor_page_count() > 1)
        if (!editor_close_page(editor_page_count() - 1)) break;
    /* last tab: close to an empty new doc */
    editor_close_page(0);
}

static void on_cut  (GtkButton *i, gpointer d) { (void)i;(void)d; editor_cut(); }
static void on_copy (GtkButton *i, gpointer d) { (void)i;(void)d; editor_copy(); }
static void on_paste(GtkButton *i, gpointer d) { (void)i;(void)d; editor_paste(); }
static void on_undo (GtkButton *i, gpointer d) { (void)i;(void)d; editor_undo(); }
static void on_redo (GtkButton *i, gpointer d) { (void)i;(void)d; editor_redo(); }

static void on_find(GtkButton *i, gpointer d)
{
    (void)i;
    NppDoc *doc = editor_current_doc();
    if (doc) findreplace_set_sci(doc->sci);
    findreplace_show((GtkWidget *)d, NULL, FALSE);
}

static void on_replace(GtkButton *i, gpointer d)
{
    (void)i;
    NppDoc *doc = editor_current_doc();
    if (doc) findreplace_set_sci(doc->sci);
    findreplace_show((GtkWidget *)d, NULL, TRUE);
}

static void on_zoom_in (GtkButton *i, gpointer d)
{
    (void)i;(void)d;
    editor_send(SCI_ZOOMIN, 0, 0);
}

static void on_zoom_out(GtkButton *i, gpointer d)
{
    (void)i;(void)d;
    editor_send(SCI_ZOOMOUT, 0, 0);
}

static void on_wrap(GtkToggleButton *item, gpointer d)
{
    (void)d;
    gboolean on = gtk_toggle_button_get_active(item);
    editor_send(SCI_SETWRAPMODE, on ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
}

/* AllChars dropdown menu items (5, per macOS _buildAllCharsMenu).
 * Each sends the appropriate SCI_ message to toggle one whitespace facet.
 * The main toggle (set via the button click) flips ALL facets together. */
static void on_ac_ws_tab(GtkButton *m, gpointer d) {
    (void)m;(void)d;
    sptr_t cur = editor_send(SCI_GETVIEWWS, 0, 0);
    editor_send(SCI_SETVIEWWS,
        cur == SC_WS_VISIBLEALWAYS ? SC_WS_INVISIBLE : SC_WS_VISIBLEALWAYS, 0);
}
static void on_ac_eol(GtkButton *m, gpointer d) {
    (void)m;(void)d;
    sptr_t cur = editor_send(SCI_GETVIEWEOL, 0, 0);
    editor_send(SCI_SETVIEWEOL, cur ? 0 : 1, 0);
}
static void on_ac_non_printing(GtkButton *m, gpointer d) {
    /* macOS folds "non-printing chars" into the WS toggle. */
    on_ac_ws_tab(m, d);
}
static void on_ac_control_chars(GtkButton *m, gpointer d) {
    /* macOS folds "control + Unicode EOL" into the EOL toggle. */
    on_ac_eol(m, d);
}
static void on_ac_all(GtkButton *m, gpointer d) {
    (void)m;(void)d;
    sptr_t ws  = editor_send(SCI_GETVIEWWS, 0, 0);
    sptr_t eol = editor_send(SCI_GETVIEWEOL, 0, 0);
    gboolean both_on = (ws == SC_WS_VISIBLEALWAYS) && eol;
    int target_ws = both_on ? SC_WS_INVISIBLE : SC_WS_VISIBLEALWAYS;
    int target_eol = both_on ? 0 : 1;
    editor_send(SCI_SETVIEWWS,  target_ws,  0);
    editor_send(SCI_SETVIEWEOL, target_eol, 0);
}

/* Build the AllChars dropdown as a GtkPopover holding 5 flat buttons.
 * (GTK4 has no GtkMenu; a popover + buttons is the lightweight idiom for a
 * small custom dropdown that fires plain callbacks.) */
static GtkWidget *make_allchars_popover(void) {
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    static const struct { const char *label; GCallback cb; } items[] = {
        { "Show Space and Tab",                    G_CALLBACK(on_ac_ws_tab) },
        { "Show End of Line",                      G_CALLBACK(on_ac_eol) },
        { "Show Non-Printing Characters",          G_CALLBACK(on_ac_non_printing) },
        { "Show Control Characters & Unicode EOL", G_CALLBACK(on_ac_control_chars) },
        { "Show All Characters",                   G_CALLBACK(on_ac_all) },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *it = gtk_button_new_with_label(items[i].label);
        gtk_button_set_has_frame(GTK_BUTTON(it), FALSE);
        gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(it)), GTK_ALIGN_START);
        g_signal_connect(it, "clicked", items[i].cb, NULL);
        g_signal_connect_swapped(it, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
        gtk_box_append(GTK_BOX(box), it);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    return pop;
}

/* The main toggle handler for the composite AllChars button. */
static void on_allchars_button_toggled(GtkToggleButton *b, gpointer u) {
    (void)u;
    /* GAP-40 — session-wide + persistent. */
    editor_set_show_all_chars(gtk_toggle_button_get_active(b));
}

/* Build the AllChars composite: [toggle][▾] in a GtkBox. The ▾ is a
 * GtkMenuButton whose popover is the 5-item dropdown. Matches macOS
 * makeViewTogglesGroupToolbarItem layout. */
static GtkWidget *make_allchars_dropdown(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    /* Main toggle button — same icon + behaviour as the old plain toggle. */
    GtkWidget *toggle = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(toggle), load_icon("allChars"));
    g_object_set_data_full(G_OBJECT(toggle), "npp-icon",
                           g_strdup("allChars"), g_free);
    gtk_button_set_has_frame(GTK_BUTTON(toggle), FALSE);
    gtk_widget_set_focus_on_click(toggle, FALSE);
    gtk_widget_set_tooltip_text(toggle, "Show All Characters");
    g_signal_connect(toggle, "toggled",
                     G_CALLBACK(on_allchars_button_toggled), NULL);
    s_tgl_allchars = toggle;  /* re-use the existing static for sync code */

    /* Dropdown arrow — a GtkMenuButton carrying the 5-item popover. */
    GtkWidget *arrow_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(arrow_btn), "pan-down-symbolic");
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(arrow_btn), FALSE);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(arrow_btn), make_allchars_popover());
    gtk_widget_set_focus_on_click(arrow_btn, FALSE);
    gtk_widget_set_tooltip_text(arrow_btn,
        "Show specific characters (Space, EOL, Control, Unicode)");

    gtk_box_append(GTK_BOX(box), toggle);
    gtk_box_append(GTK_BOX(box), arrow_btn);
    return box;
}

static void on_indent(GtkToggleButton *item, gpointer d)
{
    (void)d;
    gboolean on = gtk_toggle_button_get_active(item);
    editor_send(SCI_SETINDENTATIONGUIDES, on ? SC_IV_LOOKBOTH : SC_IV_NONE, 0);
}

static void on_print(GtkButton *i, gpointer d)
{
    (void)i; (void)d;
    main_do_print();
}

/* Sync vertical/horizontal scrolling between the split views (#5). */
static void on_syncv(GtkToggleButton *b, gpointer d)
{
    (void)d;
    editor_set_sync_scroll(TRUE, gtk_toggle_button_get_active(b));
}
static void on_synch(GtkToggleButton *b, gpointer d)
{
    (void)d;
    editor_set_sync_scroll(FALSE, gtk_toggle_button_get_active(b));
}

/* User Defined Language dialog (macOS showDefineLanguage:). */
static void on_udl(GtkButton *i, gpointer d)
{
    (void)i; (void)d;
    extern void udl_editor_show(GtkWindow *parent);
    udl_editor_show(s_window ? GTK_WINDOW(s_window) : NULL);
}

static void on_saverecord(GtkButton *i, gpointer d)
{
    (void)i; (void)d;
    NppDoc *doc = editor_current_doc();
    if (doc) macro_save_as_dialog(doc->sci, GTK_WINDOW(s_window));
}

/* ---- Panel toggles ---- */
static void on_tgl_doclist(GtkToggleButton *item, gpointer d)
{
    (void)d;
    doclist_set_visible(gtk_toggle_button_get_active(item));
}

static void on_tgl_docmap(GtkToggleButton *item, gpointer d)
{
    (void)d;
    gboolean on = gtk_toggle_button_get_active(item);
    docmap_set_visible(on);
    if (on) {
        NppDoc *doc = editor_current_doc();
        if (doc) docmap_update(doc->sci);
    }
}

static void on_tgl_workspace(GtkToggleButton *item, gpointer d)
{
    (void)d;
    workspace_set_visible(gtk_toggle_button_get_active(item));
}

static void on_tgl_funclist(GtkToggleButton *item, gpointer d)
{
    (void)d;
    funclist_set_visible(gtk_toggle_button_get_active(item));
}

static void on_tgl_monitoring(GtkToggleButton *item, gpointer d)
{
    (void)d;
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    gboolean on = gtk_toggle_button_get_active(item);
    if (on && !doc->filepath) {
        /* Can't monitor an unsaved document — revert the button */
        g_signal_handlers_block_matched(item, G_SIGNAL_MATCH_FUNC,
            0, 0, NULL, G_CALLBACK(on_tgl_monitoring), NULL);
        gtk_toggle_button_set_active(item, FALSE);
        g_signal_handlers_unblock_matched(item, G_SIGNAL_MATCH_FUNC,
            0, 0, NULL, G_CALLBACK(on_tgl_monitoring), NULL);
        return;
    }
    doc->monitoring = on;
}

static void on_macro_start(GtkButton *i, gpointer d)
{
    (void)i; (void)d;
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    macro_start_recording(doc->sci);
    toolbar_update_macro_buttons();
}

static void on_macro_stop(GtkButton *i, gpointer d)
{
    (void)i; (void)d;
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    macro_stop_recording(doc->sci);
    toolbar_update_macro_buttons();
}

static void on_macro_play(GtkButton *i, gpointer d)
{
    (void)i; (void)d;
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    macro_playback(doc->sci);
}

static void on_macro_playn(GtkButton *i, gpointer d)
{
    (void)i; (void)d;
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    macro_run_multiple_dialog(doc->sci, GTK_WINDOW(s_window));
}

/* (Re)load the .npp-toolbar CSS — the base line colour follows the
 * light/dark appearance. Reusable provider so a theme switch restyles in
 * place instead of stacking. */
static void apply_toolbar_css(void)
{
    static GtkCssProvider *prov = NULL;
    gboolean first = (prov == NULL);
    if (first) prov = gtk_css_provider_new();
    gboolean dark = is_dark_mode();
    char buf[1024];
    snprintf(buf, sizeof(buf),
        ".npp-toolbar { padding: 2px 4px;"
        " border-bottom: 1px solid %s; }"
        ".npp-toolbar button { padding: 2px; margin: 0 1px;"
        " min-width: 24px; min-height: 24px; }"
        ".npp-toolbar separator { margin: 2px 4px; }"
        "%s",
        dark ? "#444444" : "#e1e1e1",
        /* Dark-mode button states: hover / toggled-on / pressed.
         * Ordered hover < checked < active so a pressed or toggled
         * button out-paints the hover colour. */
        dark ?
        ".npp-toolbar button:hover {"
        " background-image: none; background-color: #1e1e1e; }"
        ".npp-toolbar button:checked {"
        " background-image: none; background-color: #1e1e1e; }"
        ".npp-toolbar button:active {"
        " background-image: none; background-color: #141414; }"
        : "");
    gtk_css_provider_load_from_data(prov, buf, -1);
    if (first)
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(prov),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* Reload every toolbar button's icon from the current light/dark set. */
static void reload_icons_recursive(GtkWidget *w)
{
    const char *icon = g_object_get_data(G_OBJECT(w), "npp-icon");
    if (icon && GTK_IS_BUTTON(w))
        gtk_button_set_child(GTK_BUTTON(w), load_icon(icon));
    for (GtkWidget *c = gtk_widget_get_first_child(w); c;
         c = gtk_widget_get_next_sibling(c))
        reload_icons_recursive(c);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Re-apply the toolbar chrome (base line + every icon) after a light/dark
 * appearance switch. */
void toolbar_apply_theme(void)
{
    apply_toolbar_css();
    if (s_toolbar) reload_icons_recursive(s_toolbar);
}

/* ================================================================== */
/* GAP-70 — Tahoe capsule toolbar (Modern appearance).                 */
/* Port of macOS tahoeToolbarGroups(): each semantic group becomes a   */
/* rounded capsule card holding its PRIMARY icon buttons with the      */
/* group label beneath; when the group has overflow commands the       */
/* label is a ▾ menu button exposing them. No separators (by design).  */
/* ================================================================== */

/* One capsule: appends it to `tb`, returns the icon row to fill. When
 * `overflow` is non-NULL the label becomes a ▾ menu button (takes
 * ownership of the menu). */
static GtkWidget *capsule_begin(GtkWidget *tb, const char *label,
                                GMenu *overflow)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(v, "npp-capsule");
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(v), row);

    if (overflow) {
        GtkWidget *mb = gtk_menu_button_new();
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb),
                                       G_MENU_MODEL(overflow));
        gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), label);
        gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(mb), TRUE);
        gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
        gtk_widget_add_css_class(mb, "npp-capsule-label");
        gtk_widget_set_halign(mb, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(v), mb);
        g_object_unref(overflow);
    } else {
        GtkWidget *l = gtk_label_new(label);
        gtk_widget_add_css_class(l, "npp-capsule-label");
        gtk_box_append(GTK_BOX(v), l);
    }
    gtk_box_append(GTK_BOX(tb), v);
    return row;
}

/* Build the Modern layout. Same widgets/callbacks as Classic (the same
 * make_btn/make_toggle calls assign the same statics, so sensitivity and
 * toggle mirroring keep working); only grouping and chrome differ.
 * Groups + primary/overflow split mirror macOS tahoeToolbarGroups(). */
static void build_modern_toolbar(GtkWidget *tb, GtkWidget *parent_window)
{
    GtkWidget *row;
    GMenu *m;

    /* File: primary New Open Save Print · overflow SaveAll Close CloseAll */
    m = g_menu_new();
    g_menu_append(m, "Save All",  "app.save-all");
    g_menu_append(m, "Close",     "app.close");
    g_menu_append(m, "Close All", "app.close-all");
    row = capsule_begin(tb, "File", m);
    gtk_box_append(GTK_BOX(row), make_btn("new",  "New (Ctrl+N)",    G_CALLBACK(on_new),  NULL));
    gtk_box_append(GTK_BOX(row), make_btn("open", "Open… (Ctrl+O)",  G_CALLBACK(on_open), NULL));
    s_btn_save = make_btn("save", "Save (Ctrl+S)", G_CALLBACK(on_save), NULL);
    gtk_box_append(GTK_BOX(row), s_btn_save);
    gtk_box_append(GTK_BOX(row), make_btn("print", "Print… (Ctrl+P)", G_CALLBACK(on_print), NULL));

    /* Edit: primary Copy Paste Undo Redo · overflow Cut */
    m = g_menu_new();
    g_menu_append(m, "Cut", "app.cut");
    row = capsule_begin(tb, "Edit", m);
    gtk_box_append(GTK_BOX(row), make_btn("copy",  "Copy (Ctrl+C)",  G_CALLBACK(on_copy),  NULL));
    gtk_box_append(GTK_BOX(row), make_btn("paste", "Paste (Ctrl+V)", G_CALLBACK(on_paste), NULL));
    s_btn_undo = make_btn("undo", "Undo (Ctrl+Z)",       G_CALLBACK(on_undo), NULL);
    s_btn_redo = make_btn("redo", "Redo (Ctrl+Shift+Z)", G_CALLBACK(on_redo), NULL);
    gtk_box_append(GTK_BOX(row), s_btn_undo);
    gtk_box_append(GTK_BOX(row), s_btn_redo);

    /* Find: primary Find · overflow Replace */
    m = g_menu_new();
    g_menu_append(m, "Replace…", "app.replace");
    row = capsule_begin(tb, "Find", m);
    gtk_box_append(GTK_BOX(row), make_btn("find", "Find… (Ctrl+F)", G_CALLBACK(on_find), parent_window));

    /* Zoom: primary In Out · no overflow */
    row = capsule_begin(tb, "Zoom", NULL);
    gtk_box_append(GTK_BOX(row), make_btn("zoomIn",  "Zoom In",  G_CALLBACK(on_zoom_in),  NULL));
    gtk_box_append(GTK_BOX(row), make_btn("zoomOut", "Zoom Out", G_CALLBACK(on_zoom_out), NULL));

    /* View: primary Wrap IndentGuide · overflow AllChars */
    m = g_menu_new();
    g_menu_append(m, "Show All Characters", "app.show-all-chars");
    row = capsule_begin(tb, "View", m);
    s_tgl_wrap   = make_toggle("wrap",        "Toggle Word Wrap",    G_CALLBACK(on_wrap),   NULL);
    s_tgl_indent = make_toggle("indentGuide", "Toggle Indent Guide", G_CALLBACK(on_indent), NULL);
    gtk_box_append(GTK_BOX(row), s_tgl_wrap);
    gtk_box_append(GTK_BOX(row), s_tgl_indent);

    /* Sync: primary SyncV · overflow SyncH */
    m = g_menu_new();
    g_menu_append(m, "Synchronise Horizontal Scrolling", "app.sync-scroll-h");
    row = capsule_begin(tb, "Sync", m);
    gtk_box_append(GTK_BOX(row), make_toggle("syncV",
        "Synchronise Vertical Scrolling", G_CALLBACK(on_syncv), NULL));

    /* Panels: primary DocList FileBrowser FuncList · overflow the rest */
    m = g_menu_new();
    g_menu_append(m, "Define Your Language…", "app.udl-define");
    g_menu_append(m, "Document Map",          "app.toggle-docmap");
    g_menu_append(m, "Character Panel",       "app.toggle-charpanel");
    g_menu_append(m, "Clipboard History",     "app.toggle-cliphistory");
    g_menu_append(m, "Project Panels",        "app.toggle-project");
    row = capsule_begin(tb, "Panels", m);
    s_tgl_doclist   = make_toggle("docList",     "Document List",       G_CALLBACK(on_tgl_doclist),   NULL);
    s_tgl_workspace = make_toggle("fileBrowser", "Folder as Workspace", G_CALLBACK(on_tgl_workspace), NULL);
    s_tgl_funclist  = make_toggle("funcList",    "Function List",       G_CALLBACK(on_tgl_funclist),  NULL);
    gtk_box_append(GTK_BOX(row), s_tgl_doclist);
    gtk_box_append(GTK_BOX(row), s_tgl_workspace);
    gtk_box_append(GTK_BOX(row), s_tgl_funclist);

    /* Monitor: standalone toggle · no overflow */
    row = capsule_begin(tb, "Monitor", NULL);
    s_tgl_monitoring = make_toggle("monitoring",
        "File Monitoring (tail -f)", G_CALLBACK(on_tgl_monitoring), NULL);
    gtk_box_append(GTK_BOX(row), s_tgl_monitoring);

    /* Macro: primary Start Stop · overflow Play/PlayMulti/Save */
    m = g_menu_new();
    g_menu_append(m, "Playback",                          "app.macro-play");
    g_menu_append(m, "Run a Macro Multiple Times…",       "app.macro-play-n");
    g_menu_append(m, "Save Current Recorded Macro…",      "app.macro-save-as");
    row = capsule_begin(tb, "Macro", m);
    s_btn_startrecord = make_btn("startrecord", "Start Recording (Ctrl+Shift+R)", G_CALLBACK(on_macro_start), NULL);
    s_btn_stoprecord  = make_btn("stoprecord",  "Stop Recording",                 G_CALLBACK(on_macro_stop),  NULL);
    gtk_box_append(GTK_BOX(row), s_btn_startrecord);
    gtk_box_append(GTK_BOX(row), s_btn_stoprecord);
}

GtkWidget *toolbar_init(GtkWidget *parent_window)
{
    s_window = parent_window;

    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(tb, "npp-toolbar");
    s_toolbar = tb;

    apply_toolbar_css();

    /* GAP-70 — Modern appearance: Tahoe capsule layout instead of the
     * flat Classic strip. Same widgets and behavior, different grouping. */
    if (g_prefs.appearance_style == 1) {
        build_modern_toolbar(tb, parent_window);
        toolbar_update_macro_buttons();
        return tb;
    }

#define ADD(item) gtk_box_append(GTK_BOX(tb), (item))

    /* Q3 — Toolbar order matches macOS toolbarDescriptors:
     *   New Open Save SaveAll Close CloseAll Print | Cut Copy Paste | Undo Redo |
     *   Find FindRep | ZoomIn ZoomOut | SyncV SyncH | Wrap AllChars IndentGuide |
     *   UDL DocMap DocList FuncList FileBrowser Monitor |
     *   StartRecord StopRecord PlayRecord PlayRecordM SaveRecord */

    /* File group. */
    ADD(make_btn("new",      "New (Ctrl+N)",   G_CALLBACK(on_new),      NULL));
    ADD(make_btn("open",     "Open… (Ctrl+O)", G_CALLBACK(on_open),     NULL));
    s_btn_save = make_btn("save", "Save (Ctrl+S)", G_CALLBACK(on_save), NULL);
    ADD(s_btn_save);
    ADD(make_btn("saveall",  "Save All",            G_CALLBACK(on_save_all), NULL));
    ADD(make_btn("close",    "Close (Ctrl+W)",      G_CALLBACK(on_close),    NULL));
    ADD(make_btn("closeall", "Close All",           G_CALLBACK(on_close_all),NULL));
    ADD(make_btn("print",    "Print… (Ctrl+P)",     G_CALLBACK(on_print),    NULL));
    ADD(make_sep());

    /* Clipboard group. */
    ADD(make_btn("cut",   "Cut (Ctrl+X)",   G_CALLBACK(on_cut),   NULL));
    ADD(make_btn("copy",  "Copy (Ctrl+C)",  G_CALLBACK(on_copy),  NULL));
    ADD(make_btn("paste", "Paste (Ctrl+V)", G_CALLBACK(on_paste), NULL));
    ADD(make_sep());

    /* Undo / Redo. */
    s_btn_undo = make_btn("undo", "Undo (Ctrl+Z)",       G_CALLBACK(on_undo), NULL);
    s_btn_redo = make_btn("redo", "Redo (Ctrl+Shift+Z)", G_CALLBACK(on_redo), NULL);
    ADD(s_btn_undo);
    ADD(s_btn_redo);
    ADD(make_sep());

    /* Find / Replace. */
    ADD(make_btn("find",    "Find… (Ctrl+F)",    G_CALLBACK(on_find),    parent_window));
    ADD(make_btn("findrep", "Replace… (Ctrl+H)", G_CALLBACK(on_replace), parent_window));
    ADD(make_sep());

    /* Zoom. */
    ADD(make_btn("zoomIn",  "Zoom In",  G_CALLBACK(on_zoom_in),  NULL));
    ADD(make_btn("zoomOut", "Zoom Out", G_CALLBACK(on_zoom_out), NULL));
    ADD(make_sep());

    /* Sync scrolling between the split views (#5). */
    ADD(make_toggle("syncV", "Synchronise Vertical Scrolling",
                    G_CALLBACK(on_syncv), NULL));
    ADD(make_toggle("syncH", "Synchronise Horizontal Scrolling",
                    G_CALLBACK(on_synch), NULL));
    ADD(make_sep());

    /* View toggles. TB_AllChars is a composite [toggle][▾] (5-item dropdown). */
    s_tgl_wrap   = make_toggle("wrap",        "Toggle Word Wrap",    G_CALLBACK(on_wrap),   NULL);
    s_tgl_indent = make_toggle("indentGuide", "Toggle Indent Guide", G_CALLBACK(on_indent), NULL);
    ADD(s_tgl_wrap);
    ADD(make_allchars_dropdown());          /* sets s_tgl_allchars internally */
    ADD(s_tgl_indent);
    ADD(make_sep());

    /* Panel toggles. */
    ADD(make_btn("udl", "Define Your Language…", G_CALLBACK(on_udl), NULL));
    s_tgl_docmap     = make_toggle("docMap",      "Document Map",        G_CALLBACK(on_tgl_docmap),    NULL);
    s_tgl_doclist    = make_toggle("docList",     "Document List",       G_CALLBACK(on_tgl_doclist),   NULL);
    s_tgl_funclist   = make_toggle("funcList",    "Function List",       G_CALLBACK(on_tgl_funclist),  NULL);
    s_tgl_workspace  = make_toggle("fileBrowser", "Folder as Workspace", G_CALLBACK(on_tgl_workspace), NULL);
    ADD(s_tgl_docmap);
    ADD(s_tgl_doclist);
    ADD(s_tgl_funclist);
    ADD(s_tgl_workspace);
    ADD(make_sep());

    /* Monitor — standalone. */
    s_tgl_monitoring = make_toggle("monitoring", "File Monitoring (tail -f)", G_CALLBACK(on_tgl_monitoring), NULL);
    ADD(s_tgl_monitoring);
    ADD(make_sep());

    /* Macro group. */
    s_btn_startrecord = make_btn("startrecord", "Start Recording (Ctrl+Shift+R)", G_CALLBACK(on_macro_start), NULL);
    s_btn_stoprecord  = make_btn("stoprecord",  "Stop Recording",                 G_CALLBACK(on_macro_stop),  NULL);
    s_btn_play        = make_btn("playrecord",  "Playback (Ctrl+Shift+P)",        G_CALLBACK(on_macro_play),  NULL);
    s_btn_playn       = make_btn("playrecord_m","Run Macro Multiple Times…",      G_CALLBACK(on_macro_playn), NULL);
    s_btn_saverecord  = make_btn("saverecord",  "Save Current Recorded Macro As…",G_CALLBACK(on_saverecord),  NULL);
    ADD(s_btn_startrecord);
    ADD(s_btn_stoprecord);
    ADD(s_btn_play);
    ADD(s_btn_playn);
    ADD(s_btn_saverecord);

#undef ADD

    toolbar_update_macro_buttons();
    return tb;
}

void toolbar_update_macro_buttons(void)
{
    if (!s_btn_startrecord) return;
    gboolean recording = macro_is_recording();
    gboolean has_macro = macro_has_macro();
    gtk_widget_set_sensitive(s_btn_startrecord, !recording);
    gtk_widget_set_sensitive(s_btn_stoprecord,   recording);
    /* Play/PlayN/Save live in the Macro capsule's overflow menu under the
     * Modern layout (GAP-70) — the widgets are NULL there. */
    if (s_btn_play)
        gtk_widget_set_sensitive(s_btn_play,     !recording && has_macro);
    if (s_btn_playn)
        gtk_widget_set_sensitive(s_btn_playn,    !recording && has_macro);
    if (s_btn_saverecord)
        gtk_widget_set_sensitive(s_btn_saverecord, !recording && has_macro);
}

/* Helper: set a GtkToggleButton state without firing its callback */
static void sync_toggle(GtkWidget *btn, GCallback cb, gboolean active)
{
    if (!btn) return;
    g_signal_handlers_block_matched(btn, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, cb, NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), active);
    g_signal_handlers_unblock_matched(btn, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, cb, NULL);
}

static void on_plugin_toolbar_clicked(GtkButton *b, gpointer u)
{
    (void)u;
    extern gboolean plugin_run_command_by_id(int cmd_id);
    plugin_run_command_by_id(GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(b), "plugin-cmd-id")));
}

/* GAP-74 — plugin toolbar buttons: one trailing group after a divider
 * (macOS default-mode plugin grouping). Icons are PNG paths supplied by
 * the plugin; the colorization pref's "apply to plugin icons" toggle
 * routes them through the same pipeline. */
static GtkWidget *s_plugin_group;   /* box appended on first button */

void toolbar_add_plugin_button(const char *icon_path, int cmd_id,
                               const char *tooltip)
{
    if (!s_toolbar) return;
    if (!s_plugin_group) {
        GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
        gtk_widget_set_margin_start(sep, 4);
        gtk_widget_set_margin_end(sep, 4);
        gtk_box_append(GTK_BOX(s_toolbar), sep);
        s_plugin_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(s_plugin_group, "npp-plugin-toolbar-group");
        gtk_box_append(GTK_BOX(s_toolbar), s_plugin_group);
    }

    GtkWidget *btn = gtk_button_new();
    GdkPixbuf *pb = icon_path ? gdk_pixbuf_new_from_file(icon_path, NULL)
                              : NULL;
    if (pb) {
        if (!gdk_pixbuf_get_has_alpha(pb)) {
            GdkPixbuf *a = gdk_pixbuf_add_alpha(pb, FALSE, 0, 0, 0);
            g_object_unref(pb);
            pb = a;
        }
        if (g_prefs.toolbar_color_plugins)
            toolbar_colorize_pixbuf(pb);
        GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
        g_object_unref(pb);
        GtkWidget *img = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
        g_object_unref(tex);
        gtk_image_set_pixel_size(GTK_IMAGE(img), icon_px());
        gtk_button_set_child(GTK_BUTTON(btn), img);
    } else {
        gtk_button_set_icon_name(GTK_BUTTON(btn),
                                 "application-x-addon-symbolic");
    }
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    if (tooltip) gtk_widget_set_tooltip_text(btn, tooltip);
    g_object_set_data(G_OBJECT(btn), "plugin-cmd-id",
                      GINT_TO_POINTER(cmd_id));
    g_signal_connect(btn, "clicked",
                     G_CALLBACK(on_plugin_toolbar_clicked), NULL);
    gtk_box_append(GTK_BOX(s_plugin_group), btn);
}

void toolbar_sync_panels(void)
{
    sync_toggle(s_tgl_doclist,    G_CALLBACK(on_tgl_doclist),    doclist_is_visible());
    sync_toggle(s_tgl_docmap,     G_CALLBACK(on_tgl_docmap),     docmap_is_visible());
    sync_toggle(s_tgl_workspace,  G_CALLBACK(on_tgl_workspace),  workspace_is_visible());
    sync_toggle(s_tgl_funclist,   G_CALLBACK(on_tgl_funclist),   funclist_is_visible());

    NppDoc *doc = editor_current_doc();
    sync_toggle(s_tgl_monitoring, G_CALLBACK(on_tgl_monitoring),
                doc ? doc->monitoring : FALSE);
}

void toolbar_sync_toggles(GtkWidget *sci)
{
    if (!sci) return;

    /* Monitoring: driven by NppDoc flag, not Scintilla */
    NppDoc *doc = editor_current_doc();
    sync_toggle(s_tgl_monitoring, G_CALLBACK(on_tgl_monitoring),
                doc ? doc->monitoring : FALSE);

    /* Block signals while syncing to avoid feedback loop */
    if (s_tgl_wrap) {
        g_signal_handlers_block_matched(s_tgl_wrap, G_SIGNAL_MATCH_FUNC,
                                        0, 0, NULL, G_CALLBACK(on_wrap), NULL);
        gboolean wrap = (scintilla_send_message(SCINTILLA(sci), SCI_GETWRAPMODE, 0, 0) != SC_WRAP_NONE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_tgl_wrap), wrap);
        g_signal_handlers_unblock_matched(s_tgl_wrap, G_SIGNAL_MATCH_FUNC,
                                          0, 0, NULL, G_CALLBACK(on_wrap), NULL);
    }
    if (s_tgl_allchars) {
        g_signal_handlers_block_matched(s_tgl_allchars, G_SIGNAL_MATCH_FUNC,
            0, 0, NULL, G_CALLBACK(on_allchars_button_toggled), NULL);
        gboolean ws = (scintilla_send_message(SCINTILLA(sci),
            SCI_GETVIEWWS, 0, 0) != SC_WS_INVISIBLE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_tgl_allchars), ws);
        g_signal_handlers_unblock_matched(s_tgl_allchars, G_SIGNAL_MATCH_FUNC,
            0, 0, NULL, G_CALLBACK(on_allchars_button_toggled), NULL);
    }
    if (s_tgl_indent) {
        g_signal_handlers_block_matched(s_tgl_indent, G_SIGNAL_MATCH_FUNC,
                                        0, 0, NULL, G_CALLBACK(on_indent), NULL);
        gboolean ig = (scintilla_send_message(SCINTILLA(sci), SCI_GETINDENTATIONGUIDES, 0, 0) != SC_IV_NONE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_tgl_indent), ig);
        g_signal_handlers_unblock_matched(s_tgl_indent, G_SIGNAL_MATCH_FUNC,
                                          0, 0, NULL, G_CALLBACK(on_indent), NULL);
    }
}

/* Update the icon-scale multiplier. Called from main.c at startup with
 * g_prefs.toolbar_icon_scale (an index into the macOS pickScales array). */
void toolbar_apply_icon_scale(int idx) {
    static const double pickScales[] = { 0.50, 0.75, 0.90, 1.00, 1.25, 1.50 };
    if (idx < 0 || idx > 5) idx = 3;
    s_icon_scale = pickScales[idx];
}

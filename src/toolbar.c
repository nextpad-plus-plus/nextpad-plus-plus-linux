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
#include "paths.h"
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

/* GAP-91 — capsule overflow menus carry real ICONS (macOS parity:
 * each NSMenuItem gets a 16 px copy of the button's toolbar icon).
 * GTK4's GtkPopoverMenu silently drops g_menu_item_set_icon on
 * vertical rows (documented project gotcha), so the ▾ menu is a plain
 * GtkPopover of icon+label rows instead of a menu MODEL. A flat
 * MenuButton-owned popover is grab-safe — the Wayland xdg-popup trap
 * only bites hand-parented nested popovers. */

static void on_overflow_row_action(GtkButton *b, gpointer action_name)
{
    GApplication *app = g_application_get_default();
    if (app)
        g_action_group_activate_action(G_ACTION_GROUP(app),
                                       (const char *)action_name, NULL);
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b),
                                             GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void on_overflow_row_plugin(GtkButton *b, gpointer cmd_id)
{
    GApplication *app = g_application_get_default();
    if (app)
        g_action_group_activate_action(G_ACTION_GROUP(app), "plugin-cmd",
            g_variant_new_int32(GPOINTER_TO_INT(cmd_id)));
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b),
                                             GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

/* An empty overflow popover (vertical row box inside). */
static GtkWidget *overflow_popover_new(void)
{
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "npp-overflow-menu");
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    return pop;
}

/* One icon+label row. `icon` may be NULL (label indents consistently
 * via a 16 px placeholder). Returns the row button. */
static GtkWidget *overflow_popover_add(GtkWidget *pop, GtkWidget *icon,
                                       const char *label, GCallback cb,
                                       gpointer data)
{
    GtkWidget *box = gtk_popover_get_child(GTK_POPOVER(pop));
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    if (!icon) {
        icon = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    } else if (GTK_IS_IMAGE(icon)) {
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    }
    gtk_box_append(GTK_BOX(h), icon);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(h), l);
    gtk_button_set_child(GTK_BUTTON(btn), h);
    if (cb) g_signal_connect(btn, "clicked", cb, data);
    gtk_box_append(GTK_BOX(box), btn);
    return btn;
}

/* One capsule: appends it to `tb`, returns the icon row to fill. When
 * `overflow` (a GtkPopover) is non-NULL the label becomes a ▾ menu
 * button owning it. */
static GtkWidget *capsule_begin(GtkWidget *tb, const char *label,
                                GtkWidget *overflow)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(v, "npp-capsule");
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(v), row);

    if (overflow) {
        GtkWidget *mb = gtk_menu_button_new();
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), overflow);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), label);
        gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(mb), TRUE);
        gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
        gtk_widget_add_css_class(mb, "npp-capsule-label");
        gtk_widget_set_halign(mb, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(v), mb);
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
/* GAP-70 — Tahoe capsule groups, table-driven so
 * toolbarButtonsTahoeConf.xml can hide/reorder them. */
/* ── GAP-90 — button-level Tahoe registry ──────────────────────────── */
/* Every capsule button in one table: `name` is the stable English id
 * used by the Preferences ▸ Tahoe editor and the conf file; `action`
 * powers overflow-menu placement (and promoted default-overflow items);
 * `cb` non-NULL marks a native primary builder (exact pre-GAP-90
 * widget: same callback, same static capture). Placement comes from
 * toolbarButtonsTahoeConf.xml; absent block = these defaults. */
typedef struct {
    const char *group;
    const char *name;       /* conf/UI key + overflow label            */
    const char *icon;       /* toolbar icon id (load_icon)             */
    const char *tooltip;    /* primary-placement tooltip               */
    const char *action;     /* app.<action> for overflow / promoted    */
    GCallback   cb;         /* native primary builder callback         */
    gboolean    toggle;
    gboolean    def_overflow;
    gboolean    needs_win;  /* cb user-data = parent window (Find)     */
    GtkWidget **capture;    /* static slot to assign, or NULL          */
} TahoeItem;

static const TahoeItem kTahoeItems[] = {
    { "File", "New",       "new",     "New (Ctrl+N)",     "new",       G_CALLBACK(on_new),   0,0,0, NULL },
    { "File", "Open…",     "open",    "Open… (Ctrl+O)",   "open",      G_CALLBACK(on_open),  0,0,0, NULL },
    { "File", "Save",      "save",    "Save (Ctrl+S)",    "save",      G_CALLBACK(on_save),  0,0,0, &s_btn_save },
    { "File", "Print…",    "print",   "Print… (Ctrl+P)",  "print",     G_CALLBACK(on_print), 0,0,0, NULL },
    { "File", "Save All",  "saveall", "Save All",         "save-all",  NULL, 0,1,0, NULL },
    { "File", "Close",     "close",   "Close",            "close",     NULL, 0,1,0, NULL },
    { "File", "Close All", "closeall","Close All",        "close-all", NULL, 0,1,0, NULL },

    { "Edit", "Copy",  "copy",  "Copy (Ctrl+C)",        "copy",  G_CALLBACK(on_copy),  0,0,0, NULL },
    { "Edit", "Paste", "paste", "Paste (Ctrl+V)",       "paste", G_CALLBACK(on_paste), 0,0,0, NULL },
    { "Edit", "Undo",  "undo",  "Undo (Ctrl+Z)",        "undo",  G_CALLBACK(on_undo),  0,0,0, &s_btn_undo },
    { "Edit", "Redo",  "redo",  "Redo (Ctrl+Shift+Z)",  "redo",  G_CALLBACK(on_redo),  0,0,0, &s_btn_redo },
    { "Edit", "Cut",   "cut",   "Cut",                  "cut",   NULL, 0,1,0, NULL },

    { "Find", "Find…",    "find",    "Find… (Ctrl+F)", "find",    G_CALLBACK(on_find), 0,0,1, NULL },
    { "Find", "Replace…", "findrep", "Replace…",       "replace", NULL, 0,1,0, NULL },

    { "Zoom", "Zoom In",  "zoomIn",  "Zoom In",  "zoom-in",  G_CALLBACK(on_zoom_in),  0,0,0, NULL },
    { "Zoom", "Zoom Out", "zoomOut", "Zoom Out", "zoom-out", G_CALLBACK(on_zoom_out), 0,0,0, NULL },

    { "View", "Word Wrap",           "wrap",        "Toggle Word Wrap",    "word-wrap",         G_CALLBACK(on_wrap),   1,0,0, &s_tgl_wrap },
    { "View", "Indent Guide",        "indentGuide", "Toggle Indent Guide", "show-indent-guide", G_CALLBACK(on_indent), 1,0,0, &s_tgl_indent },
    { "View", "Show All Characters", "allChars",    "Show All Characters", "show-all-chars",    NULL, 0,1,0, NULL },

    { "Sync", "Synchronise Vertical Scrolling",   "syncV", "Synchronise Vertical Scrolling",   "sync-scroll-v", G_CALLBACK(on_syncv), 1,0,0, NULL },
    { "Sync", "Synchronise Horizontal Scrolling", "syncH", "Synchronise Horizontal Scrolling", "sync-scroll-h", NULL, 0,1,0, NULL },

    { "Panels", "Document List",         "docList",     "Document List",       "toggle-doclist",     G_CALLBACK(on_tgl_doclist),   1,0,0, &s_tgl_doclist },
    { "Panels", "Folder as Workspace",   "fileBrowser", "Folder as Workspace", "toggle-workspace",   G_CALLBACK(on_tgl_workspace), 1,0,0, &s_tgl_workspace },
    { "Panels", "Function List",         "funcList",    "Function List",       "toggle-funclist",    G_CALLBACK(on_tgl_funclist),  1,0,0, &s_tgl_funclist },
    { "Panels", "Define Your Language…", "udl",         "Define Your Language…","udl-define",        NULL, 0,1,0, NULL },
    { "Panels", "Document Map",          "docMap",      "Document Map",        "toggle-docmap",      NULL, 0,1,0, NULL },
    { "Panels", "Character Panel",       "charpanel",   "Character Panel",     "toggle-charpanel",   NULL, 0,1,0, NULL },
    { "Panels", "Clipboard History",     "cliphistory", "Clipboard History",   "toggle-cliphistory", NULL, 0,1,0, NULL },
    { "Panels", "Project Panels",        "project",     "Project Panels",      "toggle-project",     NULL, 0,1,0, NULL },

    { "Monitor", "File Monitoring", "monitoring", "File Monitoring (tail -f)", "toggle-monitoring", G_CALLBACK(on_tgl_monitoring), 1,0,0, &s_tgl_monitoring },

    { "Macro", "Start Recording", "startrecord", "Start Recording (Ctrl+Shift+R)", "macro-start",   G_CALLBACK(on_macro_start), 0,0,0, &s_btn_startrecord },
    { "Macro", "Stop Recording",  "stoprecord",  "Stop Recording",                 "macro-stop",    G_CALLBACK(on_macro_stop),  0,0,0, &s_btn_stoprecord },
    { "Macro", "Playback",        "playrecord",  "Playback",                       "macro-play",    NULL, 0,1,0, NULL },
    { "Macro", "Run a Macro Multiple Times…", "playrecord", "Run a Macro Multiple Times…", "macro-play-n", NULL, 0,1,0, NULL },
    { "Macro", "Save Current Recorded Macro…", "saverecord", "Save Current Recorded Macro…", "macro-save-as", NULL, 0,1,0, NULL },
};

static const TahoeItem *tahoe_item_find(const char *group, const char *name)
{
    for (size_t i = 0; i < G_N_ELEMENTS(kTahoeItems); i++)
        if (strcmp(kTahoeItems[i].group, group) == 0 &&
            strcmp(kTahoeItems[i].name, name) == 0)
            return &kTahoeItems[i];
    return NULL;
}

/* Promoted default-overflow items become plain buttons that activate
 * the app action — same dispatch as their menu item. */
static void on_tahoe_promoted(GtkWidget *b, gpointer action_name)
{
    (void)b;
    GApplication *app = g_application_get_default();
    if (app)
        g_action_group_activate_action(G_ACTION_GROUP(app),
                                       (const char *)action_name, NULL);
}

/* Forward decls — the conf block I/O lives with the plugins machinery
 * further down (shared implementation, GAP-89). */
static gboolean tahoeconf_group_load(const char *group, gboolean *customized,
                                     char ***primary, char ***overflow,
                                     char ***hidden);

/* Build one built-in capsule from its conf split (defaults when no
 * block exists). Exact pre-GAP-90 widgets for native entries: same
 * make_btn/make_toggle call, same callback, same static capture. */
static void cap_build_group(GtkWidget *tb, GtkWidget *parent_window,
                            const char *group)
{
    gboolean customized = FALSE;
    char **primary = NULL, **overflow = NULL, **hidden = NULL;
    if (!tahoeconf_group_load(group, &customized, &primary, &overflow,
                              &hidden)) {
        /* Defaults from the registry. */
        GPtrArray *prim = g_ptr_array_new();
        GPtrArray *over = g_ptr_array_new();
        for (size_t i = 0; i < G_N_ELEMENTS(kTahoeItems); i++) {
            if (strcmp(kTahoeItems[i].group, group) != 0) continue;
            g_ptr_array_add(kTahoeItems[i].def_overflow ? over : prim,
                            g_strdup(kTahoeItems[i].name));
        }
        g_ptr_array_add(prim, NULL);
        g_ptr_array_add(over, NULL);
        primary  = (char **)g_ptr_array_free(prim, FALSE);
        overflow = (char **)g_ptr_array_free(over, FALSE);
    }

    GtkWidget *menu = NULL;
    int n_over = 0;
    for (int i = 0; overflow && overflow[i]; i++) {
        const TahoeItem *it = tahoe_item_find(group, overflow[i]);
        if (!it) continue;
        if (!menu) menu = overflow_popover_new();
        /* GAP-91 — the same toolbar icon at menu size (macOS 16 px). */
        overflow_popover_add(menu, load_icon(it->icon), it->name,
                             G_CALLBACK(on_overflow_row_action),
                             (gpointer)it->action);
        n_over++;
    }

    int n_prim = 0;
    GtkWidget *row = NULL;
    for (int i = 0; primary && primary[i]; i++) {
        const TahoeItem *it = tahoe_item_find(group, primary[i]);
        if (!it) continue;
        if (!row) row = capsule_begin(tb, group, menu);
        GtkWidget *w;
        if (it->cb) {
            w = (it->toggle ? make_toggle : make_btn)(
                    it->icon, it->tooltip, it->cb,
                    it->needs_win ? parent_window : NULL);
        } else {
            w = make_btn(it->icon, it->name, G_CALLBACK(on_tahoe_promoted),
                         (gpointer)it->action);
        }
        if (it->capture) *it->capture = w;
        gtk_box_append(GTK_BOX(row), w);
        n_prim++;
    }
    /* Overflow-only group: the capsule is just the ▾ label. */
    if (!row && n_over > 0)
        row = capsule_begin(tb, group, menu);
    else if (!row && menu)
        g_object_ref_sink(menu), g_object_unref(menu);
    if (g_getenv("NPP_TB_DUMP"))
        g_message("capsule %s: primary=%d overflow=%d", group, n_prim,
                  n_over);
    g_strfreev(primary);
    g_strfreev(overflow);
    g_strfreev(hidden);
}

#define DEFINE_CAP(fn, id) \
    static void fn(GtkWidget *tb, GtkWidget *parent_window) \
    { cap_build_group(tb, parent_window, id); }
DEFINE_CAP(cap_file,    "File")
DEFINE_CAP(cap_edit,    "Edit")
DEFINE_CAP(cap_find,    "Find")
DEFINE_CAP(cap_zoom,    "Zoom")
DEFINE_CAP(cap_view,    "View")
DEFINE_CAP(cap_sync,    "Sync")
DEFINE_CAP(cap_panels,  "Panels")
DEFINE_CAP(cap_monitor, "Monitor")
DEFINE_CAP(cap_macro,   "Macro")

typedef void (*CapsuleBuildFn)(GtkWidget *, GtkWidget *);
/* GAP-89 — ordered slot for the Plugins capsule: an empty anchor box at
 * the configured position; plugin icon registrations (which arrive
 * after toolbar build, at NPPN_TBMODIFICATION) populate it via
 * rebuild_plugins_capsule(). Tentative defs — the full machinery and
 * these statics' home live after the plugin-button block below. */
static GtkWidget *s_plugins_slot;
static gboolean   s_plugins_cap_hidden;

static void cap_plugins_slot(GtkWidget *tb, GtkWidget *parent_window)
{
    (void)parent_window;
    if (!s_plugins_slot) {
        s_plugins_slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_append(GTK_BOX(tb), s_plugins_slot);
    }
}

static const struct { const char *id; CapsuleBuildFn build; } kCapsules[] = {
    { "File", cap_file },
    { "Edit", cap_edit },
    { "Find", cap_find },
    { "Zoom", cap_zoom },
    { "View", cap_view },
    { "Sync", cap_sync },
    { "Panels", cap_panels },
    { "Monitor", cap_monitor },
    { "Macro", cap_macro },
    { "Plugins", cap_plugins_slot }
};

/* Load group order/visibility from toolbarButtonsTahoeConf.xml in the
 * settings dir; materialize the default layout on first use. Format:
 *   <TahoeToolbar><Group id="File" visible="yes"/>…</TahoeToolbar> */
static int tahoeconf_load(const char *out_ids[16])
{
    gchar *path = npp_user_file(NULL, "toolbarButtonsTahoeConf.xml");
    gchar *data = NULL;
    int n = 0;

    if (!g_file_get_contents(path, &data, NULL, NULL)) {
        GString *xml = g_string_new(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<TahoeToolbar>\n");
        for (size_t i = 0; i < G_N_ELEMENTS(kCapsules); i++)
            g_string_append_printf(xml,
                "    <Group id=\"%s\" visible=\"yes\" />\n",
                kCapsules[i].id);
        g_string_append(xml, "</TahoeToolbar>\n");
        g_file_set_contents(path, xml->str, (gssize)xml->len, NULL);
        g_string_free(xml, TRUE);
        g_free(path);
        for (size_t i = 0; i < G_N_ELEMENTS(kCapsules); i++)
            out_ids[n++] = kCapsules[i].id;
        return n;
    }
    g_free(path);

    /* Tiny forgiving parse: Group elements in file order. */
    for (char *p = data; (p = strstr(p, "<Group")) != NULL; p++) {
        char *idp = strstr(p, "id=\"");
        char *vis = strstr(p, "visible=\"");
        char *close = strchr(p, '>');
        if (!idp || !close || idp > close) continue;
        idp += 4;
        char *q = strchr(idp, '"');
        if (!q) continue;
        gboolean shown = !(vis && vis < close &&
                           g_str_has_prefix(vis + 9, "no"));
        if (!shown) {
            /* GAP-89 — remember a hidden Plugins group so the plugin-
             * registration fallback doesn't resurrect the capsule. */
            if ((size_t)(q - idp) == 7 && strncmp(idp, "Plugins", 7) == 0)
                s_plugins_cap_hidden = TRUE;
            continue;
        }
        for (size_t i = 0; i < G_N_ELEMENTS(kCapsules) && n < 16; i++)
            if ((size_t)(q - idp) == strlen(kCapsules[i].id) &&
                strncmp(idp, kCapsules[i].id, (size_t)(q - idp)) == 0)
                out_ids[n++] = kCapsules[i].id;
    }
    g_free(data);
    if (n == 0)   /* unparsable file → defaults */
        for (size_t i = 0; i < G_N_ELEMENTS(kCapsules); i++)
            out_ids[n++] = kCapsules[i].id;
    return n;
}

static void build_modern_toolbar(GtkWidget *tb, GtkWidget *parent_window)
{
    const char *ids[16];
    int n = tahoeconf_load(ids);
    for (int i = 0; i < n; i++)
        for (size_t k = 0; k < G_N_ELEMENTS(kCapsules); k++)
            if (strcmp(ids[i], kCapsules[k].id) == 0)
                kCapsules[k].build(tb, parent_window);
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

/* One plugin icon button (shared by Classic group + Tahoe capsule). */
static GtkWidget *make_plugin_icon_button(const char *icon_path, int cmd_id,
                                          const char *tooltip)
{
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
    return btn;
}

/* ── GAP-89 — Tahoe "Plugins" capsule with config-driven split ──────── */
/* macOS parity (MainWindowController.mm makeTahoePluginGroupToolbarItem
 * + _reconcileTahoePluginsIfNeeded): every plugin-registered icon lives
 * in ONE "Plugins" capsule; the primary/overflow/hidden split is keyed
 * by COMMAND NAME in the Plugins group of toolbarButtonsTahoeConf.xml.
 * While the group is un-customized, the curated default split is
 * re-derived on every registration (plugins arrive one-by-one with no
 * "all done" callback); once the user takes over (customized="yes"),
 * their arrangement is preserved and new commands only APPEND to
 * overflow. Overflow items ride the ▾ label menu through
 * app.plugin-cmd(i) — same dispatch (and macro recording) as the menu. */

typedef struct { char *name; char *icon_path; int cmd_id; } PluginTbItem;
static GPtrArray *s_plugin_items;      /* PluginTbItem*, registration order */
static GtkWidget *s_plugins_slot;      /* Modern: capsule anchor in tb */
static GtkWidget *s_plugins_capsule;   /* current capsule card, or NULL */
static gboolean   s_plugins_cap_hidden;/* conf: <Group id="Plugins" visible="no"> */

/* Curated default primary commands (same list as macOS). */
static const char *const kPluginPrimaryTips[] = {
    "Compare", "Clear Active Compare",
    "Spell Check Document Automatically",
    "Show Beads panel", "Toggle Markdown Panel",
};

static gboolean strv_has(char **v, const char *s)
{
    for (int i = 0; v && v[i]; i++)
        if (strcmp(v[i], s) == 0) return TRUE;
    return FALSE;
}

/* Read the Plugins group block from toolbarButtonsTahoeConf.xml.
 * Returns TRUE when a block exists; lists are newly-allocated GStrvs. */
static gboolean tahoeconf_group_load(const char *group,
                                     gboolean *customized,
                                     char ***primary, char ***overflow,
                                     char ***hidden)
{
    *customized = FALSE;
    *primary = *overflow = *hidden = NULL;
    gchar *path = npp_user_file(NULL, "toolbarButtonsTahoeConf.xml");
    gchar *data = NULL;
    gboolean ok = g_file_get_contents(path, &data, NULL, NULL);
    g_free(path);
    if (!ok) return FALSE;

    char needle[64];
    g_snprintf(needle, sizeof needle, "<Group id=\"%s\"", group);
    char *g = strstr(data, needle);
    if (!g) { g_free(data); return FALSE; }
    /* A self-closing row ("<Group id=... />") has no nested block. */
    {
        char *close = strchr(g, '>');
        if (close && close > g && *(close - 1) == '/') {
            g_free(data);
            return FALSE;
        }
    }
    char *end = strstr(g, "</Group>");
    if (!end) end = data + strlen(data);
    char *hdr_close = strchr(g, '>');
    if (hdr_close && hdr_close < end) {
        char save = *hdr_close; *hdr_close = '\0';
        *customized = strstr(g, "customized=\"yes\"") != NULL;
        *hdr_close = save;
    }

    static const char *const sect[] = { "Primary", "Overflow", "Hidden" };
    char ***out[] = { primary, overflow, hidden };
    for (int k = 0; k < 3; k++) {
        char open_tag[24], close_tag[24];
        g_snprintf(open_tag, sizeof open_tag, "<%s>", sect[k]);
        g_snprintf(close_tag, sizeof close_tag, "</%s>", sect[k]);
        char *sp = g_strstr_len(g, end - g, open_tag);
        if (!sp) continue;
        char *se = g_strstr_len(sp, end - sp, close_tag);
        if (!se) continue;
        GPtrArray *names = g_ptr_array_new();
        for (char *it = sp; (it = g_strstr_len(it, se - it, "<Item")) != NULL; it++) {
            char *np = g_strstr_len(it, se - it, "name=\"");
            if (!np) break;
            np += 6;
            char *nq = strchr(np, '"');
            if (!nq || nq > se) break;
            char *dec = g_strndup(np, (gsize)(nq - np));
            /* Un-escape the entities the writer produces. */
            static const struct { const char *e; const char *c; } ents[] = {
                { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" },
                { "&quot;", "\"" }, { "&apos;", "'" },
            };
            for (size_t ei = 0; ei < G_N_ELEMENTS(ents); ei++) {
                char *hit;
                while ((hit = strstr(dec, ents[ei].e)) != NULL) {
                    size_t el = strlen(ents[ei].e);
                    hit[0] = ents[ei].c[0];
                    memmove(hit + 1, hit + el, strlen(hit + el) + 1);
                }
            }
            g_ptr_array_add(names, dec);
            it = nq;
        }
        g_ptr_array_add(names, NULL);
        *out[k] = (char **)g_ptr_array_free(names, FALSE);
    }
    g_free(data);
    return TRUE;
}

/* Write the Plugins block back (replace existing block or the
 * self-closing "<Group id=\"Plugins\" .../>" row, else append before
 * </TahoeToolbar>). Preserves the rest of the file byte-for-byte. */
static void tahoeconf_group_save(const char *group, gboolean customized,
                                 char **primary, char **overflow,
                                 char **hidden)
{
    gchar *path = npp_user_file(NULL, "toolbarButtonsTahoeConf.xml");
    gchar *data = NULL;
    if (!g_file_get_contents(path, &data, NULL, NULL))
        data = g_strdup("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
                        "<TahoeToolbar>\n</TahoeToolbar>\n");

    /* Preserve the row's existing visible attribute (default yes;
     * the Plugins group also honours its hidden flag). */
    gboolean visible = TRUE;
    if (strcmp(group, "Plugins") == 0 && s_plugins_cap_hidden)
        visible = FALSE;
    {
        char needle[64];
        g_snprintf(needle, sizeof needle, "<Group id=\"%s\"", group);
        char *g0 = strstr(data, needle);
        char *close = g0 ? strchr(g0, '>') : NULL;
        if (g0 && close) {
            char save = *close; *close = '\0';
            if (strstr(g0, "visible=\"no\"")) visible = FALSE;
            *close = save;
        }
    }

    GString *blk = g_string_new(NULL);
    g_string_append_printf(blk,
        "    <Group id=\"%s\" visible=\"%s\" customized=\"%s\">\n",
        group, visible ? "yes" : "no", customized ? "yes" : "no");
    static const char *const sect[] = { "Primary", "Overflow", "Hidden" };
    char **lists[] = { primary, overflow, hidden };
    for (int k = 0; k < 3; k++) {
        g_string_append_printf(blk, "        <%s>\n", sect[k]);
        for (int i = 0; lists[k] && lists[k][i]; i++) {
            gchar *esc = g_markup_escape_text(lists[k][i], -1);
            g_string_append_printf(blk,
                "            <Item name=\"%s\" />\n", esc);
            g_free(esc);
        }
        g_string_append_printf(blk, "        </%s>\n", sect[k]);
    }
    g_string_append(blk, "    </Group>\n");

    GString *out = g_string_new(NULL);
    char needle2[64];
    g_snprintf(needle2, sizeof needle2, "<Group id=\"%s\"", group);
    char *g = strstr(data, needle2);
    if (g) {
        char *tail;
        char *blk_end = strstr(g, "</Group>");
        char *self_close = strchr(g, '>');
        if (blk_end && (!self_close || blk_end < self_close ||
                        *(self_close - 1) != '/'))
            tail = blk_end + strlen("</Group>");
        else
            tail = self_close ? self_close + 1 : g;
        /* swallow one trailing newline so the block doesn't drift */
        if (*tail == '\n') tail++;
        g_string_append_len(out, data, g - data);
        /* strip leading indentation we are about to re-add */
        while (out->len && (out->str[out->len - 1] == ' ' ||
                            out->str[out->len - 1] == '\t'))
            g_string_truncate(out, out->len - 1);
        g_string_append(out, blk->str);
        g_string_append(out, tail);
    } else {
        char *close = strstr(data, "</TahoeToolbar>");
        if (close) {
            g_string_append_len(out, data, close - data);
            g_string_append(out, blk->str);
            g_string_append(out, close);
        } else {
            g_string_append(out, data);
            g_string_append(out, blk->str);
        }
    }
    g_file_set_contents(path, out->str, (gssize)out->len, NULL);
    g_string_free(out, TRUE);
    g_string_free(blk, TRUE);
    g_free(data);
    g_free(path);
}

/* Curated default split over the CURRENT item set (macOS
 * _tahoeDefaultPluginsGroupDict): curated names go primary; if none
 * match, first three registered go primary, the rest overflow. */
static void plugins_default_split(char ***primary_out, char ***overflow_out)
{
    GPtrArray *prim = g_ptr_array_new();
    GPtrArray *over = g_ptr_array_new();
    for (guint i = 0; s_plugin_items && i < s_plugin_items->len; i++) {
        PluginTbItem *it = g_ptr_array_index(s_plugin_items, i);
        gboolean is_primary = FALSE;
        for (size_t k = 0; k < G_N_ELEMENTS(kPluginPrimaryTips); k++)
            if (g_ascii_strcasecmp(it->name, kPluginPrimaryTips[k]) == 0) {
                is_primary = TRUE;
                break;
            }
        g_ptr_array_add(is_primary ? prim : over, g_strdup(it->name));
    }
    if (prim->len == 0 && over->len > 0) {
        guint n = MIN(3u, over->len);
        for (guint i = 0; i < n; i++)
            g_ptr_array_add(prim, g_ptr_array_index(over, i));
        g_ptr_array_remove_range(over, 0, n);
    }
    g_ptr_array_add(prim, NULL);
    g_ptr_array_add(over, NULL);
    *primary_out  = (char **)g_ptr_array_free(prim, FALSE);
    *overflow_out = (char **)g_ptr_array_free(over, FALSE);
}

static PluginTbItem *plugin_item_by_name(const char *name)
{
    for (guint i = 0; s_plugin_items && i < s_plugin_items->len; i++) {
        PluginTbItem *it = g_ptr_array_index(s_plugin_items, i);
        if (strcmp(it->name, name) == 0) return it;
    }
    return NULL;
}

/* Rebuild the Plugins capsule inside its slot from the (reconciled)
 * config split. Called on every plugin icon registration, like macOS
 * _rebuildTahoePluginsGroup. */
static void rebuild_plugins_capsule(void)
{
    if (!s_plugins_slot) return;
    if (s_plugins_capsule) {
        gtk_box_remove(GTK_BOX(s_plugins_slot), s_plugins_capsule);
        s_plugins_capsule = NULL;
    }
    if (!s_plugin_items || s_plugin_items->len == 0) return;

    /* Reconcile the config split with the live item set. */
    gboolean customized = FALSE;
    char **primary = NULL, **overflow = NULL, **hidden = NULL;
    gboolean have = tahoeconf_group_load("Plugins", &customized, &primary,
                                         &overflow, &hidden);
    gboolean changed = !have;
    if (!customized) {
        /* Un-customized: re-derive the curated default over the current
         * set (registration is one-by-one; freezing an early partial
         * split would be wrong). */
        char **np = NULL, **no = NULL;
        plugins_default_split(&np, &no);
        if (!have ||
            g_strv_length(primary  ? primary  : (char *[]){NULL}) !=
                g_strv_length(np) ||
            g_strv_length(overflow ? overflow : (char *[]){NULL}) !=
                g_strv_length(no)) changed = TRUE;
        else {
            for (int i = 0; np[i] && !changed; i++)
                if (!primary[i] || strcmp(np[i], primary[i]) != 0)
                    changed = TRUE;
            for (int i = 0; no[i] && !changed; i++)
                if (!overflow[i] || strcmp(no[i], overflow[i]) != 0)
                    changed = TRUE;
        }
        g_strfreev(primary);
        g_strfreev(overflow);
        primary = np;
        overflow = no;
    } else {
        /* Customized: append genuinely new commands to overflow only. */
        GPtrArray *over = g_ptr_array_new();
        for (int i = 0; overflow && overflow[i]; i++)
            g_ptr_array_add(over, g_strdup(overflow[i]));
        for (guint i = 0; i < s_plugin_items->len; i++) {
            PluginTbItem *it = g_ptr_array_index(s_plugin_items, i);
            if (strv_has(primary, it->name) || strv_has(overflow, it->name) ||
                strv_has(hidden, it->name)) continue;
            g_ptr_array_add(over, g_strdup(it->name));
            changed = TRUE;
        }
        g_ptr_array_add(over, NULL);
        g_strfreev(overflow);
        overflow = (char **)g_ptr_array_free(over, FALSE);
    }
    if (changed)
        tahoeconf_group_save("Plugins", customized, primary, overflow,
                             hidden);

    /* Build the capsule: primary buttons + ▾ overflow via the label. */
    int n_prim = 0, n_over = 0;
    GtkWidget *menu = NULL;
    for (int i = 0; overflow && overflow[i]; i++) {
        PluginTbItem *it = plugin_item_by_name(overflow[i]);
        if (!it) continue;
        if (!menu) menu = overflow_popover_new();
        /* GAP-91 — the plugin's own PNG at menu size (macOS mi.image). */
        GtkWidget *img = NULL;
        GdkPixbuf *pb = it->icon_path[0]
            ? gdk_pixbuf_new_from_file(it->icon_path, NULL) : NULL;
        if (pb) {
            if (g_prefs.toolbar_color_plugins)
                toolbar_colorize_pixbuf(pb);
            GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
            g_object_unref(pb);
            img = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
            g_object_unref(tex);
        }
        overflow_popover_add(menu, img, it->name,
                             G_CALLBACK(on_overflow_row_plugin),
                             GINT_TO_POINTER(it->cmd_id));
        n_over++;
    }
    GtkWidget *row = capsule_begin(s_plugins_slot, "Plugins", menu);
    for (int i = 0; primary && primary[i]; i++) {
        PluginTbItem *it = plugin_item_by_name(primary[i]);
        if (!it) continue;
        gtk_box_append(GTK_BOX(row),
                       make_plugin_icon_button(it->icon_path, it->cmd_id,
                                               it->name));
        n_prim++;
    }
    s_plugins_capsule = gtk_widget_get_last_child(s_plugins_slot);
    if (n_prim == 0 && n_over == 0) {
        gtk_box_remove(GTK_BOX(s_plugins_slot), s_plugins_capsule);
        s_plugins_capsule = NULL;
    }
    if (g_getenv("NPP_TB_DUMP"))
        g_message("plugins-capsule: primary=%d overflow=%d hidden=%d",
                  n_prim, n_over,
                  hidden ? (int)g_strv_length(hidden) : 0);
    g_strfreev(primary);
    g_strfreev(overflow);
    g_strfreev(hidden);
}

void toolbar_add_plugin_button(const char *icon_path, int cmd_id,
                               const char *tooltip)
{
    if (!s_toolbar) return;

    if (g_prefs.appearance_style == 1) {
        /* GAP-89 — Tahoe: record the item, then rebuild the Plugins
         * capsule from the config split (macOS behaviour). */
        if (s_plugins_cap_hidden) return;   /* <Group visible="no"> */
        if (!s_plugins_slot) {
            /* Conf predates the Plugins group entry — trail the bar. */
            s_plugins_slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_box_append(GTK_BOX(s_toolbar), s_plugins_slot);
        }
        if (!s_plugin_items)
            s_plugin_items = g_ptr_array_new();
        char fallback[32];
        if (!tooltip || !*tooltip) {
            g_snprintf(fallback, sizeof fallback, "Plugin command %d",
                       cmd_id);
            tooltip = fallback;
        }
        for (guint i = 0; i < s_plugin_items->len; i++) {
            PluginTbItem *e = g_ptr_array_index(s_plugin_items, i);
            if (e->cmd_id == cmd_id) return;      /* duplicate */
        }
        PluginTbItem *it = g_new0(PluginTbItem, 1);
        it->name      = g_strdup(tooltip);
        it->icon_path = g_strdup(icon_path ? icon_path : "");
        it->cmd_id    = cmd_id;
        g_ptr_array_add(s_plugin_items, it);
        rebuild_plugins_capsule();
        return;
    }

    /* Classic: one trailing group after a divider (unchanged). */
    if (!s_plugin_group) {
        GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
        gtk_widget_set_margin_start(sep, 4);
        gtk_widget_set_margin_end(sep, 4);
        gtk_box_append(GTK_BOX(s_toolbar), sep);
        s_plugin_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(s_plugin_group,
                                 "npp-plugin-toolbar-group");
        gtk_box_append(GTK_BOX(s_toolbar), s_plugin_group);
    }
    gtk_box_append(GTK_BOX(s_plugin_group),
                   make_plugin_icon_button(icon_path, cmd_id, tooltip));
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

/* ── GAP-90 — Preferences ▸ Tahoe: toolbar-group editor API ─────────── */

typedef struct { char *group; char *name; int place; } TbEditRow;
static GPtrArray *s_tb_edit;   /* TbEditRow*, snapshot for the dialog */

static int placement_in_lists(const char *name, char **prim, char **over,
                              char **hid, int fallback)
{
    if (strv_has(prim, name)) return NPP_TB_PLACE_PRIMARY;
    if (strv_has(over, name)) return NPP_TB_PLACE_OVERFLOW;
    if (strv_has(hid,  name)) return NPP_TB_PLACE_HIDDEN;
    return fallback;
}

int toolbar_tahoe_item_count(void)
{
    if (s_tb_edit) {
        for (guint i = 0; i < s_tb_edit->len; i++) {
            TbEditRow *r = g_ptr_array_index(s_tb_edit, i);
            g_free(r->group); g_free(r->name); g_free(r);
        }
        g_ptr_array_free(s_tb_edit, TRUE);
    }
    s_tb_edit = g_ptr_array_new();

    /* Built-ins: registry order, conf block (if any) decides placement. */
    const char *cur_group = NULL;
    gboolean customized = FALSE;
    char **prim = NULL, **over = NULL, **hid = NULL;
    gboolean have = FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(kTahoeItems); i++) {
        const TahoeItem *it = &kTahoeItems[i];
        if (!cur_group || strcmp(cur_group, it->group) != 0) {
            g_strfreev(prim); g_strfreev(over); g_strfreev(hid);
            prim = over = hid = NULL;
            have = tahoeconf_group_load(it->group, &customized, &prim,
                                        &over, &hid);
            cur_group = it->group;
        }
        int def = it->def_overflow ? NPP_TB_PLACE_OVERFLOW
                                   : NPP_TB_PLACE_PRIMARY;
        TbEditRow *r = g_new0(TbEditRow, 1);
        r->group = g_strdup(it->group);
        r->name  = g_strdup(it->name);
        r->place = have ? placement_in_lists(it->name, prim, over, hid, def)
                        : def;
        g_ptr_array_add(s_tb_edit, r);
    }
    g_strfreev(prim); g_strfreev(over); g_strfreev(hid);
    prim = over = hid = NULL;

    /* Plugins: live registrations ∪ conf lists. */
    gboolean phave = tahoeconf_group_load("Plugins", &customized, &prim,
                                          &over, &hid);
    for (guint i = 0; s_plugin_items && i < s_plugin_items->len; i++) {
        PluginTbItem *it = g_ptr_array_index(s_plugin_items, i);
        TbEditRow *r = g_new0(TbEditRow, 1);
        r->group = g_strdup("Plugins");
        r->name  = g_strdup(it->name);
        r->place = phave ? placement_in_lists(it->name, prim, over, hid,
                                              NPP_TB_PLACE_OVERFLOW)
                         : NPP_TB_PLACE_OVERFLOW;
        g_ptr_array_add(s_tb_edit, r);
    }
    char **lists[3] = { prim, over, hid };
    for (int k = 0; k < 3; k++)
        for (int i = 0; lists[k] && lists[k][i]; i++) {
            gboolean seen = FALSE;
            for (guint j = 0; j < s_tb_edit->len && !seen; j++) {
                TbEditRow *r = g_ptr_array_index(s_tb_edit, j);
                if (strcmp(r->group, "Plugins") == 0 &&
                    strcmp(r->name, lists[k][i]) == 0) seen = TRUE;
            }
            if (seen) continue;
            TbEditRow *r = g_new0(TbEditRow, 1);
            r->group = g_strdup("Plugins");
            r->name  = g_strdup(lists[k][i]);
            r->place = k;
            g_ptr_array_add(s_tb_edit, r);
        }
    g_strfreev(prim); g_strfreev(over); g_strfreev(hid);
    return (int)s_tb_edit->len;
}

void toolbar_tahoe_item_get(int i, const char **group, const char **name,
                            int *placement)
{
    if (group) *group = NULL;
    if (name)  *name  = NULL;
    if (placement) *placement = 0;
    if (!s_tb_edit || i < 0 || (guint)i >= s_tb_edit->len) return;
    TbEditRow *r = g_ptr_array_index(s_tb_edit, i);
    if (group) *group = r->group;
    if (name)  *name  = r->name;
    if (placement) *placement = r->place;
}

void toolbar_tahoe_set_placement(const char *group, const char *name,
                                 int placement)
{
    if (!group || !name) return;
    gboolean customized = FALSE;
    char **prim = NULL, **over = NULL, **hid = NULL;
    if (!tahoeconf_group_load(group, &customized, &prim, &over, &hid)) {
        /* Materialize the group's current effective split first. */
        GPtrArray *p0 = g_ptr_array_new(), *o0 = g_ptr_array_new();
        if (strcmp(group, "Plugins") == 0) {
            for (guint i = 0; s_plugin_items && i < s_plugin_items->len; i++) {
                PluginTbItem *it = g_ptr_array_index(s_plugin_items, i);
                g_ptr_array_add(o0, g_strdup(it->name));
            }
        } else {
            for (size_t i = 0; i < G_N_ELEMENTS(kTahoeItems); i++) {
                if (strcmp(kTahoeItems[i].group, group) != 0) continue;
                g_ptr_array_add(kTahoeItems[i].def_overflow ? o0 : p0,
                                g_strdup(kTahoeItems[i].name));
            }
        }
        g_ptr_array_add(p0, NULL);
        g_ptr_array_add(o0, NULL);
        prim = (char **)g_ptr_array_free(p0, FALSE);
        over = (char **)g_ptr_array_free(o0, FALSE);
        hid  = NULL;
    }

    /* Remove from all three, append to the target list. */
    char **lists[3] = { prim, over, hid };
    GPtrArray *nl[3];
    for (int k = 0; k < 3; k++) {
        nl[k] = g_ptr_array_new();
        for (int i = 0; lists[k] && lists[k][i]; i++)
            if (strcmp(lists[k][i], name) != 0)
                g_ptr_array_add(nl[k], g_strdup(lists[k][i]));
    }
    if (placement >= 0 && placement <= 2)
        g_ptr_array_add(nl[placement], g_strdup(name));
    char *fin[3];
    for (int k = 0; k < 3; k++) {
        g_ptr_array_add(nl[k], NULL);
        fin[k] = (char *)g_ptr_array_free(nl[k], FALSE);
    }
    tahoeconf_group_save(group, TRUE, (char **)fin[0], (char **)fin[1],
                         (char **)fin[2]);
    g_strfreev((char **)fin[0]);
    g_strfreev((char **)fin[1]);
    g_strfreev((char **)fin[2]);
    g_strfreev(prim); g_strfreev(over); g_strfreev(hid);

    /* The Plugins capsule can re-split live; built-ins on restart. */
    if (strcmp(group, "Plugins") == 0 && g_prefs.appearance_style == 1)
        rebuild_plugins_capsule();
}

void toolbar_tahoe_reset(void)
{
    gchar *path = npp_user_file(NULL, "toolbarButtonsTahoeConf.xml");
    gchar *data = NULL;
    if (!g_file_get_contents(path, &data, NULL, NULL)) {
        g_free(path);
        return;
    }
    /* Replace every nested Group block with a flat visible row. */
    GString *out = g_string_new(NULL);
    char *p = data;
    while (*p) {
        char *g = strstr(p, "<Group id=\"");
        if (!g) { g_string_append(out, p); break; }
        char *idp = g + 11;
        char *idq = strchr(idp, '"');
        char *close = strchr(g, '>');
        if (!idq || !close) { g_string_append(out, p); break; }
        g_string_append_len(out, p, g - p);
        gchar *id = g_strndup(idp, (gsize)(idq - idp));
        g_string_append_printf(out, "<Group id=\"%s\" visible=\"yes\" />",
                               id);
        g_free(id);
        if (*(close - 1) == '/') {
            p = close + 1;
        } else {
            char *be = strstr(close, "</Group>");
            p = be ? be + strlen("</Group>") : close + 1;
        }
    }
    g_file_set_contents(path, out->str, (gssize)out->len, NULL);
    g_string_free(out, TRUE);
    g_free(data);
    g_free(path);
    if (g_prefs.appearance_style == 1)
        rebuild_plugins_capsule();
}

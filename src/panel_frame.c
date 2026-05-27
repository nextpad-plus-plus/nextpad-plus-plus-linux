/*
 * panel_frame.c — uniform side-panel chrome wrapper.
 *
 * GTK3 port of src/PanelFrame.mm. Builds a vertical GtkBox:
 *
 *   ┌────────────────────────────────────────────┐
 *   │ Title              [pop_out]   [ × ]       │  25-px title bar
 *   ├────────────────────────────────────────────┤  1-px separator
 *   │                                            │
 *   │              content widget                │  fills the rest
 *   │                                            │
 *   └────────────────────────────────────────────┘
 *
 * Theme: a per-frame GtkCssProvider paints the title bar with a tab-bar
 * style background (hardcoded #f0f0f0 light / #2F2F2F dark — we currently
 * have no central NppThemeManager equivalent, so each panel reads the
 * GtkSettings "gtk-application-prefer-dark-theme" hint).
 *
 * Pop-out: the [pop_out] button calls floating_toggle(name). Pop-out is
 * exclusively floating.c's responsibility; we just dispatch. The button's
 * icon swaps between pop_out / pop_in based on floating_is_floating(name).
 *
 * Show / hide: panel modules historically called gtk_widget_show/hide on
 * their own content widget. To keep those call-sites working without
 * touching every module, the frame mirrors the content's visibility via
 * "show" / "hide" signal handlers — show the content, the wrapper shows;
 * hide the content, the wrapper hides. Net effect: workspace_set_visible
 * etc. continue to work transparently after wrapping.
 */
#include "panel_frame.h"
#include "gtk_compat.h"
#include "floating.h"

#include <string.h>
#include <gdk/gdkkeysyms.h>

/* ─────────────────────────────────────────────────────────────────────── */
/* Per-frame state                                                        */
/* ─────────────────────────────────────────────────────────────────────── */

typedef struct {
    char       name[64];
    GtkWidget *title_bar;     /* GtkBox horizontal */
    GtkWidget *title_label;
    GtkWidget *pop_button;
    GtkWidget *pop_image;     /* GtkImage inside pop_button */
    GtkWidget *close_button;
    GtkWidget *separator;
    GtkWidget *content;
    void     (*on_close)(GtkWidget *frame, gpointer user);
    gpointer   on_close_user;
    int             zoom;       /* per-panel zoom level, steps from 0     */
    GtkCssProvider *zoom_css;   /* font-size scaler, scoped to content    */
} PanelFrameState;

#define PF_STATE_KEY "nextpad-panel-frame-state"

static void pf_state_free(gpointer p) {
    PanelFrameState *st = p;
    if (st->zoom_css) g_object_unref(st->zoom_css);
    g_free(st);
}

static PanelFrameState *pf_state(GtkWidget *frame) {
    return (PanelFrameState *)g_object_get_data(G_OBJECT(frame), PF_STATE_KEY);
}

/* ─────────────────────────────────────────────────────────────────────── */
/* Theme                                                                  */
/* ─────────────────────────────────────────────────────────────────────── */

/* Hardcoded tab-bar colours from PanelFrame.mm comment block. We avoid a
 * single global provider and instead scope CSS per-frame so future panels
 * can override per-instance without leaking into the global style cascade. */
static const char *kFrameCssLight =
    ".nextpad-panel-frame-titlebar { "
    "  background-color: #f0f0f0; "   /* matches the main toolbar background */
    "  border: none; "
    "  min-height: 25px; "
    "}\n"
    ".nextpad-panel-frame-title { "
    "  font-size: 11pt; "
    "  margin-left: 6px; "
    "  color: @theme_fg_color; "
    "}\n"
    ".nextpad-panel-frame-button { "
    "  padding: 0px; "
    "  margin: 0px; "
    "  min-width: 13px; "
    "  min-height: 13px; "
    "  border: none; "
    "  border-radius: 0px; "
    "  background: transparent; "
    "}\n"
    ".nextpad-panel-frame-button:hover { "
    "  background-color: #E5F3FF; "
    "}\n"
    ".nextpad-panel-frame-button:active { "
    "  background-color: #CCE8FF; "
    "}\n"
    /* Close X button: a permanent 1px square grey border (macOS
     * _PFCloseButton uses colorWithWhite:0.75), turning toolbar-blue on
     * hover/press. Declared after -button so its border wins. */
    ".nextpad-panel-frame-close { "
    "  border: 1px solid #bfbfbf; "
    "}\n"
    ".nextpad-panel-frame-close:hover, .nextpad-panel-frame-close:active { "
    "  border-color: #d0eaff; "
    "}\n"
    /* Hairline dividing the panel title bar from the toolbar/content. */
    ".nextpad-panel-frame-separator { "
    "  background-color: #cccccc; "
    "  min-height: 1px; "
    "}\n";

static const char *kFrameCssDark =
    ".nextpad-panel-frame-titlebar { "
    "  background-color: #2F2F2F; "
    "  border: none; "
    "  min-height: 25px; "
    "}\n"
    ".nextpad-panel-frame-title { "
    "  font-size: 11pt; "
    "  margin-left: 6px; "
    "  color: @theme_fg_color; "
    "}\n"
    ".nextpad-panel-frame-button { "
    "  padding: 0px; "
    "  margin: 0px; "
    "  min-width: 13px; "
    "  min-height: 13px; "
    "  border: none; "
    "  border-radius: 0px; "
    "  background: transparent; "
    "}\n"
    ".nextpad-panel-frame-button:hover { "
    "  background-color: rgba(255,255,255,0.10); "
    "}\n"
    ".nextpad-panel-frame-button:active { "
    "  background-color: rgba(255,255,255,0.18); "
    "}\n"
    /* Close X button: permanent 1px square border (dark-mode grey). */
    ".nextpad-panel-frame-close { "
    "  border: 1px solid #555555; "
    "}\n"
    ".nextpad-panel-frame-close:hover, .nextpad-panel-frame-close:active { "
    "  border-color: #6f8fb0; "
    "}\n"
    /* Hairline dividing the panel title bar from the toolbar/content. */
    ".nextpad-panel-frame-separator { "
    "  background-color: #444444; "
    "  min-height: 1px; "
    "}\n";

static gboolean pf_is_dark(void) {
    gboolean dark = FALSE;
    GtkSettings *s = gtk_settings_get_default();
    if (s) g_object_get(s, "gtk-application-prefer-dark-theme", &dark, NULL);
    return dark;
}

/* Install the per-frame CSS provider on every styled descendant. We give
 * the provider APPLICATION priority so it overrides theme defaults but
 * stays below USER. */
static void pf_apply_css(GtkWidget *target) {
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, pf_is_dark() ? kFrameCssDark : kFrameCssLight,
                                    -1);
    gtk_style_context_add_provider(gtk_widget_get_style_context(target),
                                   GTK_STYLE_PROVIDER(p),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

/* ─────────────────────────────────────────────────────────────────────── */
/* Pop-out / close button helpers                                         */
/* ─────────────────────────────────────────────────────────────────────── */

/* Try to load the project-shipped pop_out / pop_in PNGs. Falls back to
 * stock icons if the asset is missing (e.g. when running from a stripped
 * install). The pop button is 16×16 with a 12px icon — matching the
 * macOS PanelFrame _PFPopButton (16×16 button, ~13px icon). */
#ifndef RESOURCES_DIR
#  define RESOURCES_DIR "resources"
#endif

/* Pop-out icon: rendered 16×16 (macOS spec). The pop_out/pop_in PNGs are
 * bilevel 48px; 16 is exactly 48/3, a clean integer downscale (no
 * fractional-sample blur). */
#define PF_POP_ICON_PX 16
/* Pop-out button box size (matches the macOS _PFPopButton). */
#define PF_POP_BTN_PX  16
/* Margin around the pop-out button — macOS renders it with a 4pt inset. */
#define PF_POP_BTN_MARGIN 4

static GtkWidget *pf_make_pop_image(gboolean popped) {
    const char *theme_subdir = pf_is_dark() ? "dark" : "standard";
    const char *icon_name    = popped ? "pop_in" : "pop_out";
    char path[1024];
    g_snprintf(path, sizeof(path),
               "%s/icons/%s/panels/toolbar/%s.png",
               RESOURCES_DIR, theme_subdir, icon_name);

    /* Match the GTK3 port exactly — its pop icon renders sharp. Scale the
     * source to the FINAL pixel size up front with
     * gdk_pixbuf_new_from_file_at_size, then gtk_image_new_from_pixbuf.
     * The image is created already at the target size, so it is blitted
     * 1:1 with no draw-time GSK rescale (the soft step our texture +
     * set_pixel_size approach was doing). */
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(
            path, PF_POP_ICON_PX, PF_POP_ICON_PX, NULL);
        if (pb) {
            GtkWidget *img = gtk_image_new_from_pixbuf(pb);
            g_object_unref(pb);
            return img;
        }
    }

    /* Fallback to stock icons. */
    GtkWidget *img = gtk_image_new_from_icon_name(
        popped ? "go-down" : "go-up");
    gtk_image_set_pixel_size(GTK_IMAGE(img), PF_POP_ICON_PX);
    return img;
}

static void pf_refresh_pop_icon(PanelFrameState *st) {
    gboolean popped = floating_is_floating(st->name);
    GtkWidget *new_img = pf_make_pop_image(popped);
    /* Replace the current image inside the button. */
    if (st->pop_image)
        gtk_container_remove(GTK_CONTAINER(st->pop_button), st->pop_image);
    st->pop_image = new_img;
    gtk_container_add(GTK_CONTAINER(st->pop_button), st->pop_image);
    gtk_widget_show(st->pop_image);
    gtk_widget_set_tooltip_text(st->pop_button,
                                popped ? "Dock back" : "Detach");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* Signal handlers                                                        */
/* ─────────────────────────────────────────────────────────────────────── */

static void on_close_clicked(GtkButton *btn, gpointer ud) {
    (void)btn;
    GtkWidget *frame = GTK_WIDGET(ud);
    PanelFrameState *st = pf_state(frame);
    if (!st) return;
    g_message("panel_frame: close clicked for '%s'", st->name);
    if (st->on_close)
        st->on_close(frame, st->on_close_user);
    else if (st->content)
        /* Hide the CONTENT, not the frame: this drives the content→frame
         * visibility mirror (so the frame and the side-host counter both
         * update) and keeps each module's xxx_is_visible() — which probes
         * the content widget — consistent with what the user sees. */
        gtk_widget_hide(st->content);
    else
        gtk_widget_hide(frame);
}

static void on_pop_clicked(GtkButton *btn, gpointer ud) {
    (void)btn;
    GtkWidget *frame = GTK_WIDGET(ud);
    PanelFrameState *st = pf_state(frame);
    if (!st) return;
    floating_toggle(st->name);
    pf_refresh_pop_icon(st);
}

/* Mirror the content widget's visibility onto the wrapper so existing
 * panel modules that call gtk_widget_show/hide on their own content
 * widget continue to work after we've wrapped them. */
static void on_content_show(GtkWidget *content, gpointer ud) {
    (void)content;
    gtk_widget_show(GTK_WIDGET(ud));
}

static void on_content_hide(GtkWidget *content, gpointer ud) {
    (void)content;
    gtk_widget_hide(GTK_WIDGET(ud));
}

/* ─────────────────────────────────────────────────────────────────────── */
/* Panel content zoom — Ctrl +/- / Ctrl 0                                 */
/*                                                                         */
/* Every panel routes through panel_frame_new(), so wiring the zoom here    */
/* gives ALL panels the shortcut for free. Each panel keeps its OWN zoom    */
/* level (macOS behaviour): the CSS provider is scoped to that panel's      */
/* content style context, so font-size scales just that panel — its         */
/* labels and tree/list cell text. Ctrl +/-/0 act on the focused panel.    */
/* ─────────────────────────────────────────────────────────────────────── */

static void pf_apply_zoom(PanelFrameState *st)
{
    if (st->zoom < -4) st->zoom = -4;   /* clamp: 60% … 200% */
    if (st->zoom > 10) st->zoom = 10;
    char css[128];
    g_snprintf(css, sizeof(css),
               ".npp-panel-content { font-size: %d%%; }\n",
               100 + st->zoom * 10);
    gtk_css_provider_load_from_data(st->zoom_css, css, -1);
}

/* Ctrl + / Ctrl - / Ctrl 0 — added per frame; acts on that frame only. */
static gboolean pf_on_key(GtkEventControllerKey *ctl, guint keyval,
                          guint keycode, GdkModifierType state, gpointer u)
{
    (void)keycode; (void)u;
    if (!(state & GDK_CONTROL_MASK)) return FALSE;
    GtkWidget *frame = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctl));
    PanelFrameState *st = pf_state(frame);
    if (!st || !st->zoom_css) return FALSE;
    switch (keyval) {
    case GDK_KEY_plus:  case GDK_KEY_equal:  case GDK_KEY_KP_Add:
        st->zoom++;   pf_apply_zoom(st); return TRUE;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
        st->zoom--;   pf_apply_zoom(st); return TRUE;
    case GDK_KEY_0:     case GDK_KEY_KP_0:
        st->zoom = 0; pf_apply_zoom(st); return TRUE;
    }
    return FALSE;
}

/* ─────────────────────────────────────────────────────────────────────── */
/* Construction                                                           */
/* ─────────────────────────────────────────────────────────────────────── */

/* Neutral selection-highlight CSS — installed once at display level so
 * every panel that goes through panel_frame_new() gets it. Targets the
 * row-selection CSS nodes of the four list-style widgets actually used
 * by side panels: GtkListView (modern; the doc-list panel + future
 * panels), GtkColumnView's inner listview, GtkTreeView (deprecated but
 * still used by funclist / workspace / project / charpanel / gitpanel),
 * and GtkListBox (cliphistory). Search Results is the deliberate
 * exception — its content widget is a Scintilla view, not a list, and
 * Scintilla draws its own selection via SCI_STYLESETBACK, so no CSS
 * rule here applies to it. */
static void install_panel_selection_css_once(void)
{
    static gboolean done = FALSE;
    if (done) return;
    done = TRUE;
    const char *css =
        /* GtkListView / GtkColumnView */
        ".npp-panel-content listview > row:selected,"
        ".npp-panel-content listview > row:selected cell,"
        /* GtkTreeView (gtk_tree_view_new_with_model). The selection
         * paints on the .view child node when a row is :selected. */
        ".npp-panel-content treeview.view:selected,"
        ".npp-panel-content treeview > cell:selected,"
        /* GtkListBox */
        ".npp-panel-content list > row:selected {"
        "  background-color: #dcdcdc;"
        "  color: inherit;"
        "}";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

GtkWidget *panel_frame_new(const char *name,
                           const char *title,
                           GtkWidget   *content,
                           void (*on_close)(GtkWidget *frame, gpointer user),
                           gpointer     user)
{
    g_return_val_if_fail(content != NULL, NULL);
    install_panel_selection_css_once();

    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    PanelFrameState *st = g_new0(PanelFrameState, 1);
    g_strlcpy(st->name, name ? name : "", sizeof(st->name));
    st->content        = content;
    st->on_close       = on_close;
    st->on_close_user  = user;
    g_object_set_data_full(G_OBJECT(frame), PF_STATE_KEY, st, pf_state_free);

    /* ── Title bar ── */
    st->title_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(st->title_bar, -1, 25);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(st->title_bar),
        "nextpad-panel-frame-titlebar");
    pf_apply_css(st->title_bar);

    /* Left-aligned title label. */
    st->title_label = gtk_label_new(title ? title : "");
    gtk_label_set_xalign(GTK_LABEL(st->title_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(st->title_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(st->title_label, TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(st->title_label),
        "nextpad-panel-frame-title");
    pf_apply_css(st->title_label);

    /* Pop-out button — 16×16 with a 4px margin on every side (macOS spec). */
    st->pop_button = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(st->pop_button), FALSE);
    gtk_widget_set_focus_on_click(st->pop_button, FALSE);
    gtk_widget_set_size_request(st->pop_button, PF_POP_BTN_PX, PF_POP_BTN_PX);
    gtk_widget_set_valign(st->pop_button, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(st->pop_button,    PF_POP_BTN_MARGIN);
    gtk_widget_set_margin_bottom(st->pop_button, PF_POP_BTN_MARGIN);
    gtk_widget_set_margin_start(st->pop_button,  PF_POP_BTN_MARGIN);
    gtk_widget_set_margin_end(st->pop_button,    PF_POP_BTN_MARGIN);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(st->pop_button),
        "nextpad-panel-frame-button");
    pf_apply_css(st->pop_button);
    st->pop_image = pf_make_pop_image(FALSE);
    gtk_container_add(GTK_CONTAINER(st->pop_button), st->pop_image);
    gtk_widget_set_tooltip_text(st->pop_button, "Detach");
    g_signal_connect(st->pop_button, "clicked",
                     G_CALLBACK(on_pop_clicked), frame);

    /* Close X button — 30% larger than macOS's 13×13
     * (PanelFrame.mm:302-304) so it's easier to hit on Linux. */
    st->close_button = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(st->close_button), FALSE);
    gtk_widget_set_focus_on_click(st->close_button, FALSE);
    gtk_widget_set_size_request(st->close_button, 17, 17);
    gtk_widget_set_valign(st->close_button, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(st->close_button, 4);
    gtk_widget_set_margin_start(st->close_button, 2);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(st->close_button),
        "nextpad-panel-frame-button");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(st->close_button),
        "nextpad-panel-frame-close");
    pf_apply_css(st->close_button);
    {
        GtkWidget *x_img = gtk_image_new_from_icon_name(
            "window-close-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(x_img), 14);
        gtk_container_add(GTK_CONTAINER(st->close_button), x_img);
    }
    gtk_widget_set_tooltip_text(st->close_button, "Close panel");
    g_signal_connect(st->close_button, "clicked",
                     G_CALLBACK(on_close_clicked), frame);

    npp_box_pack(GTK_BOX(st->title_bar), st->title_label, TRUE, 0);
    npp_box_pack(GTK_BOX(st->title_bar), st->pop_button, FALSE, 0);
    npp_box_pack(GTK_BOX(st->title_bar), st->close_button, FALSE, 0);

    /* ── Separator ── */
    st->separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(st->separator),
        "nextpad-panel-frame-separator");
    pf_apply_css(st->separator);

    /* ── Stack: title bar + separator + content. ── */
    npp_box_pack(GTK_BOX(frame), st->title_bar, FALSE, 0);
    npp_box_pack(GTK_BOX(frame), st->separator, FALSE, 0);
    npp_box_pack(GTK_BOX(frame), content, TRUE, 0);

    /* Mirror content visibility onto the wrapper — see header comment.
     * Panels begin hidden in main.c; the show/hide handlers will keep
     * the wrapper in lock-step with the content widget. */
    g_signal_connect(content, "show", G_CALLBACK(on_content_show), frame);
    g_signal_connect(content, "hide", G_CALLBACK(on_content_hide), frame);

    /* Per-panel content zoom (Ctrl +/-/0): a CSS provider scoped to THIS
     * panel's content, so each panel keeps an independent zoom level. */
    st->zoom     = 0;
    st->zoom_css = gtk_css_provider_new();
    gtk_widget_add_css_class(content, "npp-panel-content");
    gtk_style_context_add_provider(gtk_widget_get_style_context(content),
                                   GTK_STYLE_PROVIDER(st->zoom_css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    {
        GtkEventController *kc = gtk_event_controller_key_new();
        g_signal_connect(kc, "key-pressed", G_CALLBACK(pf_on_key), NULL);
        gtk_widget_add_controller(frame, kc);
    }

    /* Sync the frame to the content's CURRENT visibility. Several panel
     * modules hide their content inside their own _init() (e.g. docmap,
     * searchresults). Because GTK4 widgets default to visible, without this
     * the wrapper frame would stay visible while its content is hidden —
     * then revealing any one panel (which shows the shared side-host) would
     * expose every such stale frame at once. */
    gtk_widget_set_visible(frame, gtk_widget_get_visible(content));

    return frame;
}

void panel_frame_set_title(GtkWidget *frame, const char *title) {
    g_return_if_fail(GTK_IS_WIDGET(frame));
    PanelFrameState *st = pf_state(frame);
    if (!st) return;
    gtk_label_set_text(GTK_LABEL(st->title_label), title ? title : "");
}

void panel_frame_set_chrome_visible(GtkWidget *frame, gboolean visible) {
    g_return_if_fail(GTK_IS_WIDGET(frame));
    PanelFrameState *st = pf_state(frame);
    if (!st) return;
    gtk_widget_set_visible(st->title_bar, visible);
    gtk_widget_set_visible(st->separator, visible);
}

void panel_frame_set_detachable(GtkWidget *frame, gboolean detachable) {
    g_return_if_fail(GTK_IS_WIDGET(frame));
    PanelFrameState *st = pf_state(frame);
    if (!st || !st->pop_button) return;
    gtk_widget_set_visible(st->pop_button, detachable);
}

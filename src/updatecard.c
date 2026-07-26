/*
 * updatecard.c — GAP-107, port of the macOS UpdateNotificationView.
 *
 * Metrics are macOS's, verbatim (UpdateNotificationView.mm:6-14). Two
 * deliberate divergences, both forced by the toolkit:
 *
 *  - macOS floats the card on an NSVisualEffectView popover material;
 *    GTK4 has no equivalent blur, so the body is a solid themed surface
 *    with the same radius, border and shadow.
 *  - macOS animates via NSAnimationContext. GTK4 CSS transitions do not
 *    apply to gtk_widget_set_opacity (a widget property, not a CSS one),
 *    so the fade + rise runs off the frame clock instead — same 0.28s
 *    ease-out in, 0.18s ease-in out, same 10px/8px travel.
 */
#include "updatecard.h"
#include "i18n.h"
#include "gtk_compat.h"

#ifndef RESOURCES_DIR
#  define RESOURCES_DIR "resources"
#endif

/* Card metrics — macOS UpdateNotificationView.mm:6-14. */
#define CARD_WIDTH    340
#define CARD_MARGIN    16   /* gap from the editor area's edges */
#define CARD_PAD       16   /* inner padding                    */
#define ICON_SIZE      32
#define ICON_GAP       12
#define BTN_HEIGHT     24
#define BTN_MIN_CLOSE  72
#define BTN_MIN_PRIM   92
/* "You're up to date" is an acknowledgement, not a decision — it leaves
 * on its own rather than demanding the click a modal would have. */
#define UPTODATE_AUTO_DISMISS_MS 4500
#define FADE_IN_US   (280 * 1000)
#define FADE_OUT_US  (180 * 1000)
#define SLIDE_IN_PX   10
#define SLIDE_OUT_PX   8
/* Text column = card - 2*pad - icon - gap = 260px. Expressed in
 * chars because that is the only natural-width cap GTK labels
 * offer; tuned so the widest string still lands on a 340px card. */
#define TEXT_MAX_CHARS 32

typedef struct {
    GtkWidget *card;
    GtkWidget *never_check;      /* UPDATE_AVAILABLE only */
    NppUpdateCardStyle style;
    void (*on_dismiss)(gboolean, gpointer);
    void (*on_primary)(gpointer);
    gpointer user_data;
    guint    auto_src;
    /* Frame-clock animation. */
    guint    tick_id;
    gint64   anim_start;
    gint64   anim_len;
    gboolean anim_out;
    gboolean dismissing;
} Card;

static Card *s_card;

static void css_once(void)
{
    static gboolean done;
    if (done) return;
    done = TRUE;
    static const char *css =
        ".npp-update-card {"
        "  background-color: @theme_bg_color;"
        "  border-radius: 12px;"
        "  border: 1px solid alpha(currentColor, 0.10);"
        "  box-shadow: 0 2px 12px alpha(black, 0.18);"
        "}"
        ".npp-update-card .npp-card-title { font-weight: bold; }"
        ".npp-update-card .npp-card-body  { opacity: 0.65; }"
        ".npp-update-card button.npp-card-primary {"
        "  background-image: none;"
        "  background-color: @theme_selected_bg_color;"
        "  color: @theme_selected_fg_color;"
        "}";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    g_object_unref(p);
}

/* The app icon, pre-scaled so it blits 1:1 (same technique panel_frame
 * uses for its chrome icons). */
static GtkWidget *app_icon_image(int px)
{
    static const char *candidates[] = {
        RESOURCES_DIR "/icons/hicolor/64x64/apps/nextpad-plus-plus.png",
        RESOURCES_DIR "/icons/hicolor/48x48/apps/nextpad-plus-plus.png",
        RESOURCES_DIR "/icons/standard/about/logo100px.png",
    };
    for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
        if (!g_file_test(candidates[i], G_FILE_TEST_EXISTS)) continue;
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(candidates[i],
                                                         px, px, NULL);
        if (pb) {
            GtkWidget *img = gtk_image_new_from_pixbuf(pb);
            g_object_unref(pb);
            return img;
        }
    }
    GtkWidget *img = gtk_image_new_from_icon_name("text-editor");
    gtk_image_set_pixel_size(GTK_IMAGE(img), px);
    return img;
}

static void card_detach(Card *c)
{
    GtkWidget *parent = gtk_widget_get_parent(c->card);
    if (GTK_IS_OVERLAY(parent))
        gtk_overlay_remove_overlay(GTK_OVERLAY(parent), c->card);
    else if (parent)
        gtk_widget_unparent(c->card);
}

static void card_free(Card *c)
{
    if (c->auto_src) g_source_remove(c->auto_src);
    if (c->tick_id)  gtk_widget_remove_tick_callback(c->card, c->tick_id);
    g_free(c);
}

static void finish_dismiss(Card *c)
{
    gboolean never = c->never_check
        ? npp_toggle_get_active(c->never_check) : FALSE;
    void (*cb)(gboolean, gpointer) = c->on_dismiss;
    gpointer ud = c->user_data;

    card_detach(c);
    if (s_card == c) s_card = NULL;
    card_free(c);

    if (cb) cb(never, ud);
}

/* ease-out cubic in, ease-in cubic out — the CAMediaTimingFunction pair
 * macOS uses for present/dismiss. */
static double ease(double t, gboolean out)
{
    if (out) return t * t * t;
    double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

static gboolean tick(GtkWidget *w, GdkFrameClock *clock, gpointer u)
{
    (void)w;
    Card *c = u;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    if (c->anim_start == 0) c->anim_start = now;
    double t = (double)(now - c->anim_start) / (double)c->anim_len;
    if (t > 1.0) t = 1.0;
    double e = ease(t, c->anim_out);

    if (c->anim_out) {
        gtk_widget_set_opacity(c->card, 1.0 - e);
        gtk_widget_set_margin_bottom(c->card,
            CARD_MARGIN - (int)(e * SLIDE_OUT_PX));
    } else {
        gtk_widget_set_opacity(c->card, e);
        gtk_widget_set_margin_bottom(c->card,
            CARD_MARGIN - (int)((1.0 - e) * SLIDE_IN_PX));
    }

    if (t < 1.0) return G_SOURCE_CONTINUE;

    c->tick_id = 0;
    if (c->anim_out) finish_dismiss(c);
    return G_SOURCE_REMOVE;
}

static void animate(Card *c, gboolean out)
{
    if (c->tick_id) { gtk_widget_remove_tick_callback(c->card, c->tick_id); c->tick_id = 0; }
    c->anim_out   = out;
    c->anim_start = 0;
    c->anim_len   = out ? FADE_OUT_US : FADE_IN_US;
    c->tick_id = gtk_widget_add_tick_callback(c->card, tick, c, NULL);
}

static void dismiss(Card *c)
{
    if (c->dismissing) return;
    c->dismissing = TRUE;
    if (c->auto_src) { g_source_remove(c->auto_src); c->auto_src = 0; }
    animate(c, TRUE);
}

static void on_close_clicked(GtkButton *b, gpointer u)
{
    (void)b;
    dismiss((Card *)u);
}

static void on_primary_clicked(GtkButton *b, gpointer u)
{
    (void)b;
    Card *c = u;
    if (c->on_primary) c->on_primary(c->user_data);
    dismiss(c);
}

static gboolean auto_dismiss_cb(gpointer u)
{
    Card *c = u;
    c->auto_src = 0;
    dismiss(c);
    return G_SOURCE_REMOVE;
}

void updatecard_hide(void)
{
    if (!s_card) return;
    Card *c = s_card;
    s_card = NULL;
    c->on_dismiss = NULL;          /* silent teardown */
    c->dismissing = TRUE;
    card_detach(c);
    card_free(c);
}

gboolean updatecard_is_visible(void) { return s_card != NULL; }

void updatecard_show(GtkWidget *host,
                     NppUpdateCardStyle style,
                     const char *title,
                     const char *message,
                     void (*on_dismiss)(gboolean, gpointer),
                     void (*on_primary)(gpointer),
                     gpointer user_data)
{
    g_return_if_fail(GTK_IS_OVERLAY(host));
    css_once();
    updatecard_hide();            /* macOS MWC:11450 — one card at a time */

    Card *c = g_new0(Card, 1);
    c->style = style;
    c->on_dismiss = on_dismiss;
    c->on_primary = on_primary;
    c->user_data = user_data;

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    c->card = card;
    gtk_widget_add_css_class(card, "npp-update-card");
    gtk_widget_set_size_request(card, CARD_WIDTH, -1);
    gtk_widget_set_halign(card, GTK_ALIGN_END);
    gtk_widget_set_valign(card, GTK_ALIGN_END);
    gtk_widget_set_margin_end(card, CARD_MARGIN);
    /* Never overflow a narrow window (macOS leading >= 8). */
    gtk_widget_set_margin_start(card, 8);

    /* ── Content: icon | (title / message) ─────────────────────────── */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, ICON_GAP);
    gtk_widget_set_margin_start(row, CARD_PAD);
    gtk_widget_set_margin_end(row, CARD_PAD);
    gtk_widget_set_margin_top(row, CARD_PAD);
    npp_box_pack(GTK_BOX(card), row, FALSE, 0);

    GtkWidget *icon = app_icon_image(ICON_SIZE);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_widget_set_size_request(icon, ICON_SIZE, ICON_SIZE);
    npp_box_pack(GTK_BOX(row), icon, FALSE, 0);

    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    npp_box_pack(GTK_BOX(row), texts, TRUE, 0);

    GtkWidget *tl = gtk_label_new(title ? title : "");
    gtk_widget_add_css_class(tl, "npp-card-title");
    gtk_label_set_xalign(GTK_LABEL(tl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(tl), PANGO_ELLIPSIZE_END);
    /* max-width-chars caps the NATURAL width request. Without it a long
     * title/message widens the whole card (GTK size requests are minima
     * — there is no max-width), and macOS pins the card at 340. */
    gtk_label_set_max_width_chars(GTK_LABEL(tl), TEXT_MAX_CHARS);
    npp_box_pack(GTK_BOX(texts), tl, FALSE, 0);

    /* Failure messages (rate limit, proxy) run long: the card grows
     * downward to fit rather than truncating the reason. */
    GtkWidget *ml = gtk_label_new(message ? message : "");
    gtk_widget_add_css_class(ml, "npp-card-body");
    gtk_label_set_xalign(GTK_LABEL(ml), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(ml), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(ml), PANGO_WRAP_WORD_CHAR);
    /* macOS: preferredMaxLayoutWidth = card - 2*pad - icon - gap. */
    gtk_label_set_max_width_chars(GTK_LABEL(ml), TEXT_MAX_CHARS);
    gtk_label_set_width_chars(GTK_LABEL(ml), TEXT_MAX_CHARS);
    npp_box_pack(GTK_BOX(texts), ml, FALSE, 0);

    /* ── "Never remind me again" (update-available only) ───────────── */
    if (style == NPP_CARD_UPDATE_AVAILABLE) {
        c->never_check = gtk_check_button_new_with_label(
            i18n_translate("Never remind me again"));
        gtk_widget_set_margin_start(c->never_check,
                                    CARD_PAD + ICON_SIZE + ICON_GAP);
        gtk_widget_set_margin_end(c->never_check, CARD_PAD);
        gtk_widget_set_margin_top(c->never_check, 12);
        npp_box_pack(GTK_BOX(card), c->never_check, FALSE, 0);
    }

    /* ── Button row: [Close] [Primary], trailing-aligned ───────────── */
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    gtk_widget_set_margin_start(btns, CARD_PAD);
    gtk_widget_set_margin_end(btns, CARD_PAD);
    gtk_widget_set_margin_top(btns, 12);
    gtk_widget_set_margin_bottom(btns, CARD_PAD);
    npp_box_pack(GTK_BOX(card), btns, FALSE, 0);

    GtkWidget *close = gtk_button_new_with_label(i18n_translate("Close"));
    gtk_widget_set_size_request(close, BTN_MIN_CLOSE, BTN_HEIGHT);
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), c);
    npp_box_pack(GTK_BOX(btns), close, FALSE, 0);

    /* Primary action: Download (available) or Try Again (failure).
     * "Up to date" has nothing to act on, so it gets Close alone.
     * Accent-filled, but deliberately NOT the default button — the
     * editor owns Return. */
    const char *primary_title = NULL;
    if (style == NPP_CARD_UPDATE_AVAILABLE)  primary_title = i18n_translate("Download");
    else if (style == NPP_CARD_FAILED)       primary_title = i18n_translate("Try Again");
    if (primary_title) {
        GtkWidget *prim = gtk_button_new_with_label(primary_title);
        gtk_widget_add_css_class(prim, "npp-card-primary");
        gtk_widget_set_size_request(prim, BTN_MIN_PRIM, BTN_HEIGHT);
        g_signal_connect(prim, "clicked", G_CALLBACK(on_primary_clicked), c);
        npp_box_pack(GTK_BOX(btns), prim, FALSE, 0);
    }

    /* Start 10px low and transparent, then rise into place. */
    gtk_widget_set_opacity(card, 0.0);
    gtk_widget_set_margin_bottom(card, CARD_MARGIN - SLIDE_IN_PX);
    gtk_overlay_add_overlay(GTK_OVERLAY(host), card);

    s_card = c;
    animate(c, FALSE);
    if (style == NPP_CARD_UP_TO_DATE)
        c->auto_src = g_timeout_add(UPTODATE_AUTO_DISMISS_MS,
                                    auto_dismiss_cb, c);
}

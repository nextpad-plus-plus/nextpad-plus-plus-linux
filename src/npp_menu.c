/*
 * npp_menu.c — GTK4 popup / context-menu compatibility layer.
 * See npp_menu.h for the rationale and API.
 */
#include "npp_menu.h"
#include "gtk_compat.h"

struct NppMenu {
    GtkWidget *popover;       /* this menu's GtkPopover */
    GtkWidget *box;           /* vertical GtkBox of rows */
    GtkWidget *root_popover;  /* topmost popover — popped down on any item */
    NppMenu   *root;          /* the top-level menu (root->root == root) */
    GSList    *subs;          /* root only: every child NppMenu, for cleanup */
    /* Hover-to-open state — per menu level, so nested submenus work too. */
    NppMenu   *open_sub;      /* the child submenu currently popped up */
    NppMenu   *pending_sub;   /* the one waiting for the hover timer to fire */
    guint      hover_timer;   /* g_timeout id, 0 when idle */
};

static NppMenu *alloc_menu(void)
{
    NppMenu *m = g_new0(NppMenu, 1);
    m->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(m->box, "npp-popup-menu");
    return m;
}

static GtkWidget *make_popover(GtkWidget *child)
{
    GtkWidget *p = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(p), FALSE);
    gtk_popover_set_child(GTK_POPOVER(p), child);
    return p;
}

/* Install the right-click menu CSS once. Two jobs:
 *   - Match the GtkPopoverMenu modelbutton spacing/padding the App
 *     menubar uses, so right-click menus feel identical.
 *   - Clamp min-width/height on the GtkMenuButton's internal arrow
 *     placeholder image to 0 — even after gtk_menu_button_set_child(),
 *     GTK4 still measures that hidden GtkImage and sometimes passes a
 *     negative for_size, producing the 'GtkImage width 0 height -9'
 *     warning every time a submenu animates open. The CSS makes the
 *     measure floor at 0 so for_size can't go negative. */
static void ensure_npp_menu_css(void) {
    static gboolean installed = FALSE;
    if (installed) return;
    installed = TRUE;
    /* - Adwaita modelbutton metrics (min-height: 26 / padding: 3 11)
     *   so right-click rows match the App menubar's rows.
     * - :hover/:focus/:active mirror Adwaita's modelbutton state
     *   highlighting (subtle alpha tint over the row).
     * - .npp-submenu-arrow renders the pan-end-symbolic icon as a CSS
     *   background — no GtkImage widget, so no measure assertion. */
    const char *css =
        ".npp-popup-menu { padding: 0; }"
        ".npp-popup-menu > button {"
        "  padding: 3px 11px;"
        "  min-height: 26px;"
        "  border-radius: 0;"
        "}"
        ".npp-popup-menu > button:hover,"
        ".npp-popup-menu > button:focus {"
        "  background: alpha(currentColor, 0.08);"
        "}"
        ".npp-popup-menu > button:active {"
        "  background: alpha(currentColor, 0.14);"
        "}"
        ".npp-submenu-arrow {"
        "  background-image: -gtk-icontheme(\"pan-end-symbolic\");"
        "  background-repeat: no-repeat;"
        "  background-position: center;"
        "  background-size: contain;"
        "  -gtk-icon-style: symbolic;"
        "  opacity: 0.6;"
        "}";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

NppMenu *npp_menu_new(void)
{
    ensure_npp_menu_css();
    NppMenu *m = alloc_menu();
    m->popover = make_popover(m->box);
    m->root_popover = m->popover;
    m->root = m;
    return m;
}

/* ---- Hover-to-open submenu plumbing ----------------------------------
 * Each row gets a GtkEventControllerMotion. On enter:
 *   - cancel any pending hover timer on the parent menu;
 *   - if a different submenu is currently open, popdown it;
 *   - if this row is itself a submenu trigger, schedule a delayed popup.
 * Standard menu behaviour, matching the GtkPopoverMenu menubar with
 * GTK_POPOVER_MENU_NESTED. Timer runs at 200 ms — same delay GTK uses
 * for modelbutton submenu open. */
typedef struct { NppMenu *parent; NppMenu *sub; } HoverInfo;

static gboolean hover_open_timer(gpointer data) {
    NppMenu *m = data;
    m->hover_timer = 0;
    NppMenu *pending = m->pending_sub;
    m->pending_sub = NULL;
    if (!pending) return G_SOURCE_REMOVE;
    gtk_popover_popup(GTK_POPOVER(pending->popover));
    m->open_sub = pending;
    return G_SOURCE_REMOVE;
}

static void cancel_hover_timer(NppMenu *m) {
    if (m->hover_timer) {
        g_source_remove(m->hover_timer);
        m->hover_timer = 0;
    }
    m->pending_sub = NULL;
}

static void on_row_enter(GtkEventControllerMotion *ctl, double x, double y,
                         gpointer ud)
{
    (void)ctl; (void)x; (void)y;
    HoverInfo *info = ud;
    NppMenu *m   = info->parent;
    NppMenu *sub = info->sub;

    /* Re-entering the same submenu trigger that's already open — keep it. */
    if (sub && m->open_sub == sub) {
        cancel_hover_timer(m);
        return;
    }

    cancel_hover_timer(m);

    /* Close any other open submenu when moving to a different row. */
    if (m->open_sub && m->open_sub != sub) {
        gtk_popover_popdown(GTK_POPOVER(m->open_sub->popover));
        m->open_sub = NULL;
    }

    if (sub) {
        m->pending_sub = sub;
        m->hover_timer = g_timeout_add(200, hover_open_timer, m);
    }
}

static void attach_hover(GtkWidget *row, NppMenu *parent, NppMenu *sub) {
    HoverInfo *info = g_new0(HoverInfo, 1);
    info->parent = parent;
    info->sub    = sub;
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_object_set_data_full(G_OBJECT(motion), "hover-info", info, g_free);
    g_signal_connect(motion, "enter", G_CALLBACK(on_row_enter), info);
    gtk_widget_add_controller(row, motion);
}

GtkWidget *npp_menu_box(NppMenu *m) { return m->box; }

GtkWidget *npp_menu_add(NppMenu *m, const char *label,
                        GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    GtkWidget *lbl = gtk_button_get_child(GTK_BUTTON(b));
    if (lbl) gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    if (cb) g_signal_connect(b, "clicked", cb, data);
    g_signal_connect_swapped(b, "clicked",
                             G_CALLBACK(gtk_popover_popdown), m->root_popover);
    /* Hovering a regular row closes any open submenu in this menu. */
    attach_hover(b, m, NULL);
    gtk_box_append(GTK_BOX(m->box), b);
    return b;
}

GtkWidget *npp_menu_add_check(NppMenu *m, const char *label, gboolean active,
                              GCallback cb, gpointer data)
{
    GtkWidget *c = gtk_check_button_new_with_label(label);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c), active);
    if (cb) g_signal_connect(c, "toggled", cb, data);
    gtk_box_append(GTK_BOX(m->box), c);
    return c;
}

void npp_menu_add_separator(NppMenu *m)
{
    gtk_box_append(GTK_BOX(m->box),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
}

NppMenu *npp_menu_add_submenu(NppMenu *m, const char *label)
{
    NppMenu *sub = alloc_menu();
    sub->popover      = make_popover(sub->box);
    sub->root_popover = m->root_popover;
    sub->root         = m->root;
    m->root->subs     = g_slist_prepend(m->root->subs, sub);

    /* Only the root popover gets an autohide grab. If the submenu also
     * grabs (default for GtkPopover), an outside click is consumed by
     * its grab — the submenu closes but the root stays up ("sticky
     * menu — ESC works but clicking outside doesn't"). With autohide
     * off, the root's grab cleanly catches the outside click; the
     * cascade in on_closed unparents this submenu transitively. */
    gtk_popover_set_autohide(GTK_POPOVER(sub->popover), FALSE);

    /* Plain GtkButton — not GtkMenuButton — because GtkMenuButton
     * instantiates an internal GtkImage for its arrow placeholder in
     * init() and keeps a reference to it even after
     * gtk_menu_button_set_child() unparents it. The popover-animation
     * measure pass on submenu open still hits that lingering GtkImage
     * with a negative for_size and trips the
     * 'gtk_widget_measure: for_size >= -1' assertion. A plain GtkButton
     * has no such placeholder.
     *
     * Custom child layout: [label hexpand][pan-end-symbolic via CSS
     * background] — visually identical to the menubar's modelbutton
     * submenu rows, with no GtkImage anywhere in the tree. */
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *lab  = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lab), 0.0);
    gtk_widget_set_hexpand(lab, TRUE);
    GtkWidget *arr  = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(arr, 16, 16);
    gtk_widget_set_valign(arr, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(arr, "npp-submenu-arrow");
    gtk_box_append(GTK_BOX(hbox), lab);
    gtk_box_append(GTK_BOX(hbox), arr);
    gtk_button_set_child(GTK_BUTTON(btn), hbox);

    /* Parent the submenu popover to the button (same lifecycle the old
     * GtkMenuButton path arranged via set_popover). Position to the
     * right so submenus cascade like macOS NSMenu. */
    gtk_widget_set_parent(sub->popover, btn);
    gtk_popover_set_position(GTK_POPOVER(sub->popover), GTK_POS_RIGHT);

    /* Click opens the submenu. The submenu's items popdown the whole
     * root popover via npp_menu_add's connect_swapped, so the parent
     * menu closes too once an item is chosen. */
    g_signal_connect_swapped(btn, "clicked",
                             G_CALLBACK(gtk_popover_popup), sub->popover);

    /* Hover-to-open: enter schedules the popup after the 200 ms delay,
     * matching the GtkPopoverMenu menubar's nested-submenu behaviour. */
    attach_hover(btn, m, sub);

    gtk_box_append(GTK_BOX(m->box), btn);
    return sub;
}

/* Synchronous teardown out of "closed". The original deferred-via-idle
 * version caused two bugs the user kept hitting:
 *
 *   (a) "menu sometimes doesn't open when I right-click again" — the
 *       previous popover is still parented to the Scintilla widget
 *       while the next right-click tries to map a new one. On Wayland
 *       the xdg-popup grab refuses or the popup never realizes.
 *   (b) Teardown running on idle could chase the user's next click and
 *       race with the gesture handler.
 *
 * Running unparent inside the "closed" emission is safe: closed fires
 * AFTER the popdown animation completes, and GTK isn't iterating
 * children at that point.
 *
 * Each submenu's popover is manually parented to its GtkButton row
 * (plain GtkButton — nothing else owns the parent link), so we MUST
 * unparent every submenu popover before letting the root tear down,
 * otherwise the cascade-free of the row buttons logs
 *     Finalizing GtkButton, but it still has children left: GtkPopover
 * The NppMenu structs (sub-menu metadata only) are freed last. */
static void on_closed(GtkPopover *p, gpointer data)
{
    (void)p;
    NppMenu *root = data;
    cancel_hover_timer(root);
    for (GSList *l = root->subs; l; l = l->next) {
        NppMenu *sub = l->data;
        cancel_hover_timer(sub);
        if (sub->popover && gtk_widget_get_parent(sub->popover))
            gtk_widget_unparent(sub->popover);
    }
    gtk_widget_unparent(root->popover);
    g_slist_free_full(root->subs, g_free);
    g_free(root);
}

void npp_menu_popup_at(NppMenu *m, GtkWidget *anchor, double x, double y)
{
    gtk_widget_set_parent(m->popover, anchor);
    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(m->popover), &r);
    g_signal_connect(m->popover, "closed", G_CALLBACK(on_closed), m);
    gtk_popover_popup(GTK_POPOVER(m->popover));
}

void npp_menu_popup_at_widget(NppMenu *m, GtkWidget *anchor)
{
    gtk_widget_set_parent(m->popover, anchor);
    g_signal_connect(m->popover, "closed", G_CALLBACK(on_closed), m);
    gtk_popover_popup(GTK_POPOVER(m->popover));
}

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

    gtk_box_append(GTK_BOX(m->box), btn);
    return sub;
}

/* Teardown is deferred out of the "closed" emission. Each submenu's
 * popover is manually parented to its GtkButton row (we use a plain
 * GtkButton, not GtkMenuButton, so nothing else owns that parent
 * link). We MUST unparent every submenu popover before letting the
 * root tear down: when gtk_widget_unparent(root->popover) cascades and
 * frees the row buttons, GTK4 finalizes each button — and if a button
 * still has the submenu GtkPopover as a child, GTK logs
 *     Finalizing GtkButton, but it still has children left: GtkPopover
 * Then the NppMenu structs (sub-menu metadata only) get freed. */
static gboolean teardown(gpointer data)
{
    NppMenu *root = data;
    for (GSList *l = root->subs; l; l = l->next) {
        NppMenu *sub = l->data;
        if (sub->popover && gtk_widget_get_parent(sub->popover))
            gtk_widget_unparent(sub->popover);
    }
    gtk_widget_unparent(root->popover);
    g_slist_free_full(root->subs, g_free);
    g_free(root);
    return G_SOURCE_REMOVE;
}

static void on_closed(GtkPopover *p, gpointer data)
{
    (void)p;
    g_idle_add(teardown, data);
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

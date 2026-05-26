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
     * - .npp-submenu-arrow renders the pan-end-symbolic icon as a CSS
     *   background on a plain GtkBox — avoids the GtkImage measure
     *   assertion that animating popovers trigger when measuring the
     *   icon widget.
     * - menubutton's internal placeholder image gets min-size: 0 as a
     *   belt-and-braces clamp (it still exists even after set_child). */
    const char *css =
        ".npp-popup-menu { padding: 0; }"
        ".npp-popup-menu > button,"
        ".npp-popup-menu > menubutton,"
        ".npp-popup-menu > menubutton > button {"
        "  padding: 3px 11px;"
        "  min-height: 26px;"
        "}"
        ".npp-popup-menu > menubutton { padding: 0; }"
        ".npp-popup-menu menubutton image {"
        "  min-width: 0;"
        "  min-height: 0;"
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

    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
    /* Custom child: [label hexpand][pan-end-symbolic via CSS background].
     * Rendering the arrow with a CSS background-image on a plain GtkBox
     * (instead of a GtkImage) keeps the icon visually identical to the
     * menubar's modelbutton submenu arrow while eliminating the GtkImage
     * measure assertion ("for_size >= -1 failed" / "width 0 height -9")
     * that fires when a submenu animates open — that assertion lives in
     * gtk_widget_measure() on GtkImage and cannot be suppressed from CSS
     * once the widget exists. set_direction(RIGHT) positions the popover
     * side-by-side (macOS cascade). */
    gtk_menu_button_set_direction(GTK_MENU_BUTTON(mb), GTK_ARROW_RIGHT);
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
    gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), hbox);
    /* The menu button takes ownership of the submenu popover, so it is
     * freed transitively when the root popover tree is torn down. */
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), sub->popover);
    gtk_box_append(GTK_BOX(m->box), mb);
    return sub;
}

/* Teardown is deferred out of the "closed" emission. Unparenting the root
 * popover drops its last reference and frees the whole widget tree (box,
 * rows, submenu buttons and their popovers). Then the NppMenu structs go. */
static gboolean teardown(gpointer data)
{
    NppMenu *root = data;
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

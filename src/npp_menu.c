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

NppMenu *npp_menu_new(void)
{
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
    gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), label);
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
    /* Side-pointing arrow indicator (macOS style) — RIGHT both points the
     * indicator glyph rightwards and makes the submenu open to the side
     * of the parent row instead of below it. always-show-arrow is needed
     * for the arrow to appear alongside the text label. */
    gtk_menu_button_set_direction(GTK_MENU_BUTTON(mb), GTK_ARROW_RIGHT);
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(mb), TRUE);
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

/* GTK4 requires popovers with autohide=TRUE to be parented to a top-
 * level GtkWindow — otherwise the xdg-popup grab fails on Wayland and
 * the popover dismisses itself on the very click that opened it
 * ("Tried to map a grabbing popup with a non-top most parent"). Walk
 * anchor → root, translate the click coordinates into window space,
 * parent there. */
static GtkWidget *toplevel_anchor(GtkWidget *anchor, double *x, double *y)
{
    GtkRoot *root = gtk_widget_get_root(anchor);
    if (!root) return anchor;
    GtkWidget *toplevel = GTK_WIDGET(root);
    if (toplevel == anchor) return anchor;
    graphene_point_t in  = GRAPHENE_POINT_INIT((float)*x, (float)*y);
    graphene_point_t out = GRAPHENE_POINT_INIT(0, 0);
    if (gtk_widget_compute_point(anchor, toplevel, &in, &out)) {
        *x = out.x;
        *y = out.y;
        return toplevel;
    }
    return anchor;
}

void npp_menu_popup_at(NppMenu *m, GtkWidget *anchor, double x, double y)
{
    anchor = toplevel_anchor(anchor, &x, &y);
    gtk_widget_set_parent(m->popover, anchor);
    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(m->popover), &r);
    g_signal_connect(m->popover, "closed", G_CALLBACK(on_closed), m);
    gtk_popover_popup(GTK_POPOVER(m->popover));
}

void npp_menu_popup_at_widget(NppMenu *m, GtkWidget *anchor)
{
    double zero_x = 0, zero_y = 0;
    GtkWidget *parent = toplevel_anchor(anchor, &zero_x, &zero_y);
    gtk_widget_set_parent(m->popover, parent);
    /* When we walked up to the window, point at the anchor's bounds in
     * window space so the popover still appears next to the original
     * widget instead of at the window origin. */
    if (parent != anchor) {
        graphene_rect_t bounds;
        if (gtk_widget_compute_bounds(anchor, parent, &bounds)) {
            GdkRectangle r = { (int)bounds.origin.x, (int)bounds.origin.y,
                               (int)bounds.size.width, (int)bounds.size.height };
            gtk_popover_set_pointing_to(GTK_POPOVER(m->popover), &r);
        }
    }
    g_signal_connect(m->popover, "closed", G_CALLBACK(on_closed), m);
    gtk_popover_popup(GTK_POPOVER(m->popover));
}

/*
 * floating.c — G21 floating side panel registry + reparenting.
 *
 * Each entry remembers the original parent (typically a GtkPaned) and
 * the slot (pack1 / pack2 / which position in a Box). Pop-out detaches
 * the widget from its parent, packs it into a fresh GtkWindow. Close
 * of that window reparents back.
 */
#include "floating.h"
#include "gtk_compat.h"
#include <string.h>

typedef enum {
    SLOT_NONE,
    SLOT_PANED_1,
    SLOT_PANED_2,
    SLOT_BOX_START,
    SLOT_BOX_END,
    SLOT_CONTAINER
} SlotKind;

typedef struct {
    char        name[64];
    GtkWidget  *widget;
    GtkWidget  *parent;        /* original parent — GtkPaned, GtkBox, or container */
    SlotKind    slot;
    gboolean    resize;        /* GtkPaned pack resize hint */
    gboolean    shrink;        /* GtkPaned pack shrink hint */
    GtkWidget  *float_window;  /* current floating window, or NULL */
    int         float_w, float_h; /* persisted geometry */
    gboolean    pinned;        /* GAP-72 — floats above the main window */
} FloatingEntry;

#define MAX_FLOATS 48   /* built-ins + plugin panels (GAP-81) */
static FloatingEntry s_entries[MAX_FLOATS];
static int           s_n = 0;
/* GAP-83 — panelstate.c re-saves open+popped state on every pop-out /
 * dock-back (these transitions emit no show/hide; macOS b98a360). */
static void (*s_layout_hook)(void);

static FloatingEntry *find_entry(const char *name) {
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_entries[i].name, name) == 0)
            return &s_entries[i];
    return NULL;
}

/* Inspect the parent of `widget` and classify which slot it occupies. */
static void capture_slot(FloatingEntry *e) {
    GtkWidget *parent = gtk_widget_get_parent(e->widget);
    e->parent = parent;
    e->slot = SLOT_NONE;
    if (!parent) return;
    if (GTK_IS_PANED(parent)) {
        GtkWidget *c1 = gtk_paned_get_start_child(GTK_PANED(parent));
        if (c1 == e->widget) {
            e->slot = SLOT_PANED_1;
            e->resize = gtk_paned_get_resize_start_child(GTK_PANED(parent));
            e->shrink = gtk_paned_get_shrink_start_child(GTK_PANED(parent));
        } else {
            e->slot = SLOT_PANED_2;
            e->resize = gtk_paned_get_resize_end_child(GTK_PANED(parent));
            e->shrink = gtk_paned_get_shrink_end_child(GTK_PANED(parent));
        }
    } else if (GTK_IS_BOX(parent)) {
        /* Assume start-packed (pack-type defaults to start). */
        e->slot = SLOT_BOX_START;
    } else if (GTK_IS_CONTAINER(parent)) {
        e->slot = SLOT_CONTAINER;
    }
}

void floating_register(const char *name, GtkWidget *widget) {
    if (s_n >= MAX_FLOATS || !widget) return;
    FloatingEntry *e = &s_entries[s_n++];
    memset(e, 0, sizeof(*e));
    g_strlcpy(e->name, name, sizeof(e->name));
    e->widget = widget;
    capture_slot(e);
    /* Default geometry for popout. */
    e->float_w = 400;
    e->float_h = 600;
    e->pinned  = TRUE;   /* panels float above the editor by default */
}

/* GAP-72 — pin toggle: transient-for-main on, free toplevel off. */
static void on_pin_toggled(GtkToggleButton *b, gpointer win)
{
    FloatingEntry *e = g_object_get_data(G_OBJECT(b), "float-entry");
    GtkRoot *main_root = g_object_get_data(G_OBJECT(b), "float-main");
    gboolean pinned = npp_toggle_get_active(GTK_WIDGET(b));
    if (e) e->pinned = pinned;
    gtk_window_set_transient_for(GTK_WINDOW(win),
        pinned && GTK_IS_WINDOW(main_root) ? GTK_WINDOW(main_root) : NULL);
}

static gboolean on_float_window_delete(GtkWindow *w, gpointer ud) {
    FloatingEntry *e = (FloatingEntry *)ud;
    if (!e) return FALSE;
    /* Persist geometry before closing. */
    gtk_window_get_size(w, &e->float_w, &e->float_h);
    /* Dock back. */
    floating_dockback(e->name);
    return TRUE;  /* we destroyed the window via dockback */
}

/* TRUE if `box` still has at least one visible child. */
static gboolean box_has_visible_child(GtkWidget *box) {
    for (GtkWidget *c = gtk_widget_get_first_child(box); c;
         c = gtk_widget_get_next_sibling(c))
        if (gtk_widget_get_visible(c)) return TRUE;
    return FALSE;
}

void floating_popout(const char *name) {
    FloatingEntry *e = find_entry(name); if (!e || e->float_window) return;
    if (!e->widget || !e->parent) return;

    /* Hold a ref so the widget survives reparenting. */
    g_object_ref(e->widget);
    gtk_container_remove(GTK_CONTAINER(e->parent), e->widget);

    /* If the dock host (a box) is now empty, collapse it so the editor
     * reclaims the space the detached panel occupied. */
    if (GTK_IS_BOX(e->parent) && !box_has_visible_child(e->parent))
        gtk_widget_set_visible(e->parent, FALSE);

    /* Root of the docked parent = the main window; grab it BEFORE the
     * removal below unparents everything. */
    GtkRoot *main_root = gtk_widget_get_root(e->parent);

    GtkWidget *win = gtk_window_new();
    char title[128];
    g_snprintf(title, sizeof(title), "%s — Nextpad++ Panel", e->name);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), e->float_w, e->float_h);
    g_signal_connect(win, "close-request",
                     G_CALLBACK(on_float_window_delete), e);

    /* GAP-72 — pin toggle in the header bar (macOS FloatingPanelWindow):
     * pinned keeps the panel above the main window. GTK4 has no window
     * levels, so pin = transient-for-main; unpin = free-floating. */
    {
        GtkWidget *hb  = gtk_header_bar_new();
        GtkWidget *pin = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(pin), "view-pin-symbolic");
        gtk_button_set_has_frame(GTK_BUTTON(pin), FALSE);
        gtk_widget_set_tooltip_text(pin, "Keep above the main window");
        npp_toggle_set_active(pin, e->pinned);
        g_object_set_data(G_OBJECT(pin), "float-entry", e);
        g_object_set_data(G_OBJECT(pin), "float-main",  main_root);
        g_signal_connect(pin, "toggled", G_CALLBACK(on_pin_toggled), win);
        gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), pin);
        gtk_window_set_titlebar(GTK_WINDOW(win), hb);
    }
    if (e->pinned && GTK_IS_WINDOW(main_root))
        gtk_window_set_transient_for(GTK_WINDOW(win),
                                     GTK_WINDOW(main_root));

    gtk_container_add(GTK_CONTAINER(win), e->widget);
    g_object_unref(e->widget);

    gtk_widget_show_all(win);
    e->float_window = win;
    if (s_layout_hook) s_layout_hook();
}

void floating_dockback(const char *name) {
    FloatingEntry *e = find_entry(name); if (!e || !e->float_window) return;
    if (!e->widget || !e->parent) return;

    g_object_ref(e->widget);
    gtk_container_remove(GTK_CONTAINER(e->float_window), e->widget);

    /* Re-show the dock host before re-inserting (popout may have hidden
     * it once empty). */
    if (e->parent) gtk_widget_set_visible(e->parent, TRUE);

    switch (e->slot) {
    case SLOT_PANED_1:
        gtk_paned_pack1(GTK_PANED(e->parent), e->widget, e->resize, e->shrink);
        break;
    case SLOT_PANED_2:
        gtk_paned_pack2(GTK_PANED(e->parent), e->widget, e->resize, e->shrink);
        break;
    case SLOT_BOX_START:
        npp_box_pack(GTK_BOX(e->parent), e->widget, TRUE, 0);
        break;
    case SLOT_BOX_END:
        npp_box_pack_end(GTK_BOX(e->parent), e->widget, TRUE, 0);
        break;
    case SLOT_CONTAINER:
    case SLOT_NONE:
        gtk_container_add(GTK_CONTAINER(e->parent), e->widget);
        break;
    }
    g_object_unref(e->widget);

    gtk_widget_destroy(e->float_window);
    e->float_window = NULL;
    gtk_widget_show(e->widget);
    if (s_layout_hook) s_layout_hook();
}

void floating_toggle(const char *name) {
    FloatingEntry *e = find_entry(name); if (!e) return;
    if (e->float_window) floating_dockback(name);
    else                  floating_popout(name);
}

gboolean floating_is_floating(const char *name) {
    FloatingEntry *e = find_entry(name);
    return e && e->float_window != NULL;
}

/* ── GAP-83 — persistence support (panelstate.c) ─────────────────────── */

void floating_set_state(const char *name, int w, int h, gboolean pinned) {
    FloatingEntry *e = find_entry(name); if (!e) return;
    if (w >= 100 && h >= 100) { e->float_w = w; e->float_h = h; }
    e->pinned = pinned;
}

gboolean floating_get_state(const char *name, int *w, int *h,
                            gboolean *pinned) {
    FloatingEntry *e = find_entry(name); if (!e) return FALSE;
    if (w)      *w      = e->float_w;
    if (h)      *h      = e->float_h;
    if (pinned) *pinned = e->pinned;
    return TRUE;
}

void floating_capture_geometry(void) {
    for (int i = 0; i < s_n; i++) {
        FloatingEntry *e = &s_entries[i];
        if (e->float_window)
            gtk_window_get_size(GTK_WINDOW(e->float_window),
                                &e->float_w, &e->float_h);
    }
}

void floating_set_layout_hook(void (*hook)(void)) {
    s_layout_hook = hook;
}

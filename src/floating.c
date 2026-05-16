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
} FloatingEntry;

#define MAX_FLOATS 16
static FloatingEntry s_entries[MAX_FLOATS];
static int           s_n = 0;

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

void floating_popout(const char *name) {
    FloatingEntry *e = find_entry(name); if (!e || e->float_window) return;
    if (!e->widget || !e->parent) return;

    /* Hold a ref so the widget survives reparenting. */
    g_object_ref(e->widget);
    gtk_container_remove(GTK_CONTAINER(e->parent), e->widget);

    GtkWidget *win = gtk_window_new();
    char title[128];
    g_snprintf(title, sizeof(title), "%s — Nextpad++ Panel", e->name);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), e->float_w, e->float_h);
    g_signal_connect(win, "close-request",
                     G_CALLBACK(on_float_window_delete), e);

    gtk_container_add(GTK_CONTAINER(win), e->widget);
    g_object_unref(e->widget);

    gtk_widget_show_all(win);
    e->float_window = win;
}

void floating_dockback(const char *name) {
    FloatingEntry *e = find_entry(name); if (!e || !e->float_window) return;
    if (!e->widget || !e->parent) return;

    g_object_ref(e->widget);
    gtk_container_remove(GTK_CONTAINER(e->float_window), e->widget);

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

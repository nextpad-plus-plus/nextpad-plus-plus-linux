/*
 * npp_menu.c — GtkPopoverMenu-backed context menu.
 *
 * See npp_menu.h for the why. This file is the runtime: each NppMenu
 * owns a GMenu (model) plus, for the root menu only, a transient
 * GSimpleActionGroup that backs every callback-style item. Sections in
 * the GMenu render as separator-delimited groups. Sub-menus are nested
 * GMenuModels; the popover widget is built once, lazily, when the menu
 * is popped up.
 */

#include "npp_menu.h"
#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/* Per-NppMenu state                                                   */
/* ------------------------------------------------------------------ */
struct NppMenu {
    /* This menu's model (a GMenu that contains 1+ sections). Submenus
     * point to their own GMenu via g_menu_append_submenu in the parent. */
    GMenu              *model;
    /* Current open section — separator starts a new one. */
    GMenu              *cur_section;

    /* Pointer back to root NppMenu (root->root == root). The root owns
     * the action group, the next-id counter, and the eventual popover. */
    NppMenu            *root;

    /* --- root-only fields below --- */
    GSimpleActionGroup *actions;     /* "rcm" namespace for callback items */
    int                 next_id;     /* generates unique action names      */
    GSList             *subs;        /* every sub NppMenu, for free()      */
    GtkWidget          *popover;     /* GtkPopoverMenu, NULL until popup  */
};

/* ------------------------------------------------------------------ */
/* Callback adapter — bridges GtkButton-style cb to GSimpleAction      */
/* "activate" so existing callers don't have to change their callback  */
/* signatures.                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    GCallback cb;
    gpointer  data;
} CbClosure;

static void cb_closure_free(gpointer ud, GClosure *closure) {
    (void)closure;
    g_free(ud);
}

static void action_activated_thunk(GSimpleAction *a, GVariant *param,
                                   gpointer ud)
{
    (void)a; (void)param;
    CbClosure *c = ud;
    typedef void (*BtnFn)(GtkButton *, gpointer);
    BtnFn fn = (BtnFn) c->cb;
    fn(NULL, c->data);
}

/* For check items — handler must update state then run the user cb.   */
static void check_change_state(GSimpleAction *a, GVariant *value,
                               gpointer ud)
{
    g_simple_action_set_state(a, value);
    CbClosure *c = ud;
    typedef void (*CheckFn)(GtkCheckButton *, gpointer);
    CheckFn fn = (CheckFn) c->cb;
    fn(NULL, c->data);
}

/* ------------------------------------------------------------------ */
/* alloc / new                                                          */
/* ------------------------------------------------------------------ */
static NppMenu *alloc_menu(void) {
    NppMenu *m = g_new0(NppMenu, 1);
    m->model       = g_menu_new();
    m->cur_section = g_menu_new();
    g_menu_append_section(m->model, NULL, G_MENU_MODEL(m->cur_section));
    return m;
}

NppMenu *npp_menu_new(void) {
    NppMenu *m = alloc_menu();
    m->root    = m;
    m->actions = g_simple_action_group_new();
    return m;
}

/* ------------------------------------------------------------------ */
/* Item add — three flavours                                            */
/* ------------------------------------------------------------------ */

/* Register a new transient action under the root's "rcm" group; return
 * the GSimpleAction (owned by the group). Generates a name like "i17". */
static GSimpleAction *register_transient_action(NppMenu *m,
                                                gboolean stateful,
                                                gboolean initial_state,
                                                gboolean enabled,
                                                GCallback cb,
                                                gpointer data,
                                                gchar **out_full_action)
{
    int id = m->root->next_id++;
    gchar name[32];
    g_snprintf(name, sizeof(name), "i%d", id);

    GSimpleAction *a;
    if (stateful) {
        a = g_simple_action_new_stateful(name, NULL,
                                         g_variant_new_boolean(initial_state));
        if (cb) {
            CbClosure *c = g_new(CbClosure, 1);
            c->cb = cb; c->data = data;
            g_signal_connect_data(a, "change-state",
                                  G_CALLBACK(check_change_state),
                                  c, cb_closure_free, 0);
        }
    } else {
        a = g_simple_action_new(name, NULL);
        if (cb) {
            CbClosure *c = g_new(CbClosure, 1);
            c->cb = cb; c->data = data;
            g_signal_connect_data(a, "activate",
                                  G_CALLBACK(action_activated_thunk),
                                  c, cb_closure_free, 0);
        }
    }
    g_simple_action_set_enabled(a, enabled);
    g_action_map_add_action(G_ACTION_MAP(m->root->actions), G_ACTION(a));
    /* Drop our ref — action group keeps one. */
    g_object_unref(a);

    if (out_full_action)
        *out_full_action = g_strdup_printf("rcm.%s", name);
    return a;
}

gpointer npp_menu_add(NppMenu *m, const char *label,
                      GCallback cb, gpointer data)
{
    gchar *full = NULL;
    /* When cb is NULL we still register an action — but disabled, so the
     * item renders as a non-clickable header (used by spell's
     * "Suggestions for …" row). */
    GSimpleAction *a = register_transient_action(m, FALSE, FALSE,
                                                 cb != NULL,
                                                 cb, data, &full);
    GMenuItem *it = g_menu_item_new(label, full);
    g_menu_append_item(m->cur_section, it);
    g_object_unref(it);
    g_free(full);
    return a;  /* opaque handle for set_sensitive */
}

gpointer npp_menu_add_check(NppMenu *m, const char *label, gboolean active,
                            GCallback cb, gpointer data)
{
    gchar *full = NULL;
    GSimpleAction *a = register_transient_action(m, TRUE, active,
                                                 TRUE,
                                                 cb, data, &full);
    GMenuItem *it = g_menu_item_new(label, full);
    g_menu_append_item(m->cur_section, it);
    g_object_unref(it);
    g_free(full);
    return a;
}

void npp_menu_add_action(NppMenu *m, const char *label,
                         const char *action_full_name)
{
    GMenuItem *it = g_menu_item_new(label, action_full_name);
    g_menu_append_item(m->cur_section, it);
    g_object_unref(it);
}

void npp_menu_add_action_target(NppMenu *m, const char *label,
                                const char *action_full_name, GVariant *target)
{
    GMenuItem *it = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target_value(it, action_full_name, target);
    /* g_menu_item_set_action_and_target_value sinks the floating ref
     * and copies as needed; defensively unref if caller passed a fresh
     * non-floating variant. */
    g_menu_append_item(m->cur_section, it);
    g_object_unref(it);
}

gpointer npp_menu_add_markup(NppMenu *m, const char *markup_label,
                             GCallback cb, gpointer data)
{
    gchar *full = NULL;
    GSimpleAction *a = register_transient_action(m, FALSE, FALSE,
                                                 cb != NULL,
                                                 cb, data, &full);
    GMenuItem *it = g_menu_item_new(markup_label, full);
    g_menu_item_set_attribute(it, "use-markup", "s", "true");
    g_menu_append_item(m->cur_section, it);
    g_object_unref(it);
    g_free(full);
    return a;
}

void npp_menu_add_action_target_markup(NppMenu *m, const char *markup_label,
                                       const char *action_full_name,
                                       GVariant *target)
{
    GMenuItem *it = g_menu_item_new(markup_label, NULL);
    g_menu_item_set_action_and_target_value(it, action_full_name, target);
    g_menu_item_set_attribute(it, "use-markup", "s", "true");
    g_menu_append_item(m->cur_section, it);
    g_object_unref(it);
}

void npp_menu_item_set_sensitive(gpointer handle, gboolean sensitive)
{
    if (G_IS_SIMPLE_ACTION(handle))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(handle), sensitive);
}

/* ------------------------------------------------------------------ */
/* Separator — implemented as a section break                          */
/* ------------------------------------------------------------------ */
void npp_menu_add_separator(NppMenu *m)
{
    /* If current section is empty, no separator would render — but
     * harmless. Start a new section regardless. */
    GMenu *next = g_menu_new();
    g_menu_append_section(m->model, NULL, G_MENU_MODEL(next));
    g_object_unref(m->cur_section);  /* model holds its own ref */
    m->cur_section = next;
}

/* ------------------------------------------------------------------ */
/* Submenu — child NppMenu sharing the root's action group             */
/* ------------------------------------------------------------------ */
NppMenu *npp_menu_add_submenu(NppMenu *m, const char *label)
{
    NppMenu *sub = alloc_menu();
    sub->root   = m->root;
    /* Don't allocate a separate action group — submenu items live in
     * the root's "rcm" namespace too. */
    g_menu_append_submenu(m->cur_section, label, G_MENU_MODEL(sub->model));
    /* Track for free-on-teardown. */
    m->root->subs = g_slist_prepend(m->root->subs, sub);
    return sub;
}

/* ------------------------------------------------------------------ */
/* Data owner for compat (spell.c spell-ctx storage)                   */
/* ------------------------------------------------------------------ */
GObject *npp_menu_data_owner(NppMenu *m)
{
    return G_OBJECT(m->root->actions);
}

/* ------------------------------------------------------------------ */
/* Pop up / teardown                                                    */
/* ------------------------------------------------------------------ */
static void free_one_sub(gpointer p)
{
    NppMenu *sub = p;
    g_object_unref(sub->cur_section);
    g_object_unref(sub->model);
    g_free(sub);
}

static void teardown_root(NppMenu *root)
{
    /* Detach the popover from its anchor; the popover itself is
     * disposed once we drop our last ref through unparent. */
    if (root->popover) {
        gtk_widget_unparent(root->popover);
        root->popover = NULL;
    }
    g_slist_free_full(root->subs, free_one_sub);
    root->subs = NULL;
    g_object_unref(root->cur_section);
    g_object_unref(root->model);
    g_object_unref(root->actions);
    g_free(root);
}

static gboolean teardown_idle(gpointer ud)
{
    teardown_root((NppMenu *)ud);
    return G_SOURCE_REMOVE;
}

static void on_popover_closed(GtkPopover *p, gpointer ud)
{
    (void)p;
    /* Defer to idle so we don't tear down inside the closed-signal
     * emission — GTK's per-window popover list walks during cleanup
     * would otherwise hit a stale entry. Priority HIGH keeps the
     * teardown ahead of the next event so the next right-click starts
     * from a clean slate. */
    g_idle_add_full(G_PRIORITY_HIGH, teardown_idle, ud, NULL);
}

static GtkWidget *build_popover(NppMenu *root)
{
    GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(root->model));
    gtk_widget_insert_action_group(pop, "rcm", G_ACTION_GROUP(root->actions));
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    g_signal_connect(pop, "closed",
                     G_CALLBACK(on_popover_closed), root);
    return pop;
}

void npp_menu_popup_at(NppMenu *m, GtkWidget *anchor, double x, double y)
{
    NppMenu *root = m->root;
    root->popover = build_popover(root);
    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(root->popover), &r);
    gtk_widget_set_parent(root->popover, anchor);
    gtk_popover_popup(GTK_POPOVER(root->popover));
}

void npp_menu_popup_at_widget(NppMenu *m, GtkWidget *anchor)
{
    NppMenu *root = m->root;
    root->popover = build_popover(root);
    gtk_widget_set_parent(root->popover, anchor);
    gtk_popover_popup(GTK_POPOVER(root->popover));
}

/*
 * ctxmenu.c — XML-driven editor + tab right-click menus, rendered as
 * GtkPopoverMenu from a GMenuModel.
 *
 * Mirrors macOS MainWindowController._buildEditorContextMenuFromXML and
 * NppTabBar._buildTabContextMenuFromXML: same XML attributes, same lookup
 * semantics (top-level menu → optional submenu narrowing → recursive item
 * lookup by label), same FolderName grouping, same id="0" → separator
 * rule, and the same display-string localization through the i18n
 * catalog.
 *
 * Switching to GtkPopoverMenu gives the right-click menus all the real-
 * menu behaviour for free: hover highlight, hover-to-open submenus, the
 * side-arrow disclosure, kbd nav, left-aligned rows, and the same
 * spacing/padding as the GtkPopoverMenuBar menus. Apply Color items use
 * the popover-menu custom-child mechanism so we can draw a coloured
 * square left of each label without the GMenu-icon path that crashes
 * GTK's menu renderer (see reference_gtk4_gotchas).
 */
#include "ctxmenu.h"
#include "gtk_compat.h"
#include "paths.h"
#include "i18n.h"
#include <string.h>

/* ============================== Index ============================== */

/* (top-menu, submenu-or-"", item) → "app.action-name" — submenu-aware so
 * the editor XML's three "Using 1st Style" entries (which live in
 * different parent submenus: Style All Occurrences / Style One / Clear
 * Style) resolve to their correct distinct actions. */
static GHashTable *s_index = NULL;

static gchar *strip_trailing_ellipsis(const char *s) {
    gchar *r = g_strdup(s ? s : "");
    gsize n = strlen(r);
    for (;;) {
        if (n >= 3 && (guchar)r[n-3] == 0xE2 &&
            (guchar)r[n-2] == 0x80 && (guchar)r[n-1] == 0xA6) {
            n -= 3; r[n] = '\0'; continue;          /* U+2026 … */
        }
        if (n >= 1 && (r[n-1] == '.' || r[n-1] == ' ' || r[n-1] == '\t')) {
            n -= 1; r[n] = '\0'; continue;
        }
        break;
    }
    return r;
}

static gchar *norm_part(const char *s) {
    gchar *lo  = g_ascii_strdown(s ? s : "", -1);
    gchar *out = strip_trailing_ellipsis(lo);
    g_free(lo);
    return out;
}

static gchar *key3(const char *top, const char *sub, const char *item) {
    gchar *t = norm_part(top);
    gchar *s = norm_part(sub);
    gchar *i = norm_part(item);
    gchar *r = g_strdup_printf("%s\t%s\t%s", t, s, i);
    g_free(t); g_free(s); g_free(i);
    return r;
}

static void index_item(const char *top, const char *sub,
                       const char *label, const char *action)
{
    gchar *k_sub = key3(top, sub, label);
    g_hash_table_insert(s_index, k_sub, g_strdup(action));

    gchar *k_any = key3(top, "", label);
    if (g_hash_table_contains(s_index, k_any))
        g_free(k_any);
    else
        g_hash_table_insert(s_index, k_any, g_strdup(action));
}

static gchar *strip_mnemonic(const char *s) {
    gchar *r = g_new(gchar, strlen(s) + 1);
    int j = 0;
    for (int k = 0; s[k]; k++) if (s[k] != '_') r[j++] = s[k];
    r[j] = '\0';
    return r;
}

static void walk_scope(GMenuModel *model, const char *top, const char *cur_sub)
{
    int n = g_menu_model_get_n_items(model);
    for (int i = 0; i < n; i++) {
        gchar *label  = NULL;
        gchar *action = NULL;
        g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL,  "s", &label);
        g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);

        if (label && action) {
            gchar *clean = strip_mnemonic(label);
            index_item(top, cur_sub, clean, action);
            g_free(clean);
        }

        GMenuModel *sec = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION);
        if (sec) {
            walk_scope(sec, top, cur_sub);
            g_object_unref(sec);
        }
        GMenuModel *sub = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU);
        if (sub) {
            gchar *sub_label = label ? strip_mnemonic(label) : NULL;
            walk_scope(sub, top, sub_label ? sub_label : "");
            g_free(sub_label);
            g_object_unref(sub);
        }
        g_free(label);
        g_free(action);
    }
}

void ctxmenu_index_from_model(GMenuModel *model) {
    if (s_index) g_hash_table_destroy(s_index);
    s_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!model) return;

    int n = g_menu_model_get_n_items(model);
    for (int i = 0; i < n; i++) {
        gchar *label = NULL;
        g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
        GMenuModel *sub = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU);
        if (label && sub) {
            gchar *clean = strip_mnemonic(label);
            walk_scope(sub, clean, "");
            g_free(clean);
        }
        g_free(label);
        if (sub) g_object_unref(sub);
    }
}

/* Try the (top, sub, item) key first, fall back to (top, "", item). */
static const char *lookup_action(const char *entry, const char *sub,
                                 const char *item)
{
    if (!s_index || !entry || !item) return NULL;
    if (sub && *sub) {
        gchar *ks = key3(entry, sub, item);
        const char *v = g_hash_table_lookup(s_index, ks);
        g_free(ks);
        if (v) return v;
    }
    gchar *ka = key3(entry, "", item);
    const char *v = g_hash_table_lookup(s_index, ka);
    g_free(ka);
    return v;
}

/* ============================== CtxMenu ============================== */

typedef struct { GtkWidget *widget; char *slot; } CtxCustom;
typedef struct { gpointer data; GDestroyNotify free; } CtxDestructor;

struct CtxMenu {
    GMenu          *root;        /* the GMenu being built */
    GMenu          *cur_section; /* current section in `root` */
    GHashTable     *folders;     /* FolderName -> FolderInfo* */
    GPtrArray      *customs;     /* CtxCustom* — built widgets to attach */
    GPtrArray      *destructors; /* CtxDestructor* — caller data to free */
    int             next_slot;
    GtkApplication *app;
};

typedef struct { GMenu *menu; GMenu *cur_section; } FolderInfo;

static void custom_free(gpointer p) {
    CtxCustom *c = p;
    if (c->widget) g_object_ref_sink(c->widget);   /* swallow floating ref */
    g_clear_object(&c->widget);
    g_free(c->slot);
    g_free(c);
}
static void destructor_free(gpointer p) {
    CtxDestructor *d = p;
    if (d->free && d->data) d->free(d->data);
    g_free(d);
}
static void folderinfo_free(gpointer p) {
    FolderInfo *f = p;
    g_clear_object(&f->menu);
    g_clear_object(&f->cur_section);
    g_free(f);
}

static CtxMenu *ctxmenu_new(GtkApplication *app) {
    CtxMenu *m = g_new0(CtxMenu, 1);
    m->root        = g_menu_new();
    m->cur_section = g_menu_new();
    g_menu_append_section(m->root, NULL, G_MENU_MODEL(m->cur_section));
    m->folders     = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, folderinfo_free);
    m->customs     = g_ptr_array_new_with_free_func(custom_free);
    m->destructors = g_ptr_array_new_with_free_func(destructor_free);
    m->app         = app;
    return m;
}

static void ctxmenu_free(CtxMenu *m) {
    if (!m) return;
    g_clear_object(&m->root);
    g_clear_object(&m->cur_section);
    g_hash_table_destroy(m->folders);
    g_ptr_array_free(m->customs, TRUE);
    g_ptr_array_free(m->destructors, TRUE);
    g_free(m);
}

GMenu *ctxmenu_root(CtxMenu *m) { return m ? m->root : NULL; }

const char *ctxmenu_register_custom(CtxMenu *m, GtkWidget *floating) {
    if (!m || !floating) return NULL;
    CtxCustom *c = g_new0(CtxCustom, 1);
    /* Ref-sink so we hold one strong ref through the floating period. */
    c->widget = g_object_ref_sink(floating);
    c->slot   = g_strdup_printf("npp-cust-%d", ++m->next_slot);
    g_ptr_array_add(m->customs, c);
    return c->slot;
}

void ctxmenu_attach_data(CtxMenu *m, gpointer data, GDestroyNotify free) {
    if (!m || !data) { if (free) free(data); return; }
    CtxDestructor *d = g_new0(CtxDestructor, 1);
    d->data = data;
    d->free = free;
    g_ptr_array_add(m->destructors, d);
}

/* ============================== Section management ============================== */

static FolderInfo *get_folder(CtxMenu *m, const char *name) {
    FolderInfo *f = g_hash_table_lookup(m->folders, name);
    if (f) return f;
    f = g_new0(FolderInfo, 1);
    f->menu        = g_menu_new();
    f->cur_section = g_menu_new();
    g_menu_append_section(f->menu, NULL, G_MENU_MODEL(f->cur_section));
    /* Folder submenu is appended to the *current* root section so the
     * folder lives in the right place in the menu order. The submenu
     * label goes through the i18n catalog. */
    g_menu_append_submenu(m->cur_section, i18n_translate(name),
                          G_MENU_MODEL(f->menu));
    g_hash_table_insert(m->folders, g_strdup(name), f);
    return f;
}

/* Start a fresh section in either the root menu or a folder. */
static void new_section_in(CtxMenu *m, FolderInfo *folder) {
    GMenu *parent;
    if (folder) {
        g_clear_object(&folder->cur_section);
        folder->cur_section = g_menu_new();
        parent = folder->menu;
        g_menu_append_section(parent, NULL, G_MENU_MODEL(folder->cur_section));
    } else {
        g_clear_object(&m->cur_section);
        m->cur_section = g_menu_new();
        parent = m->root;
        g_menu_append_section(parent, NULL, G_MENU_MODEL(m->cur_section));
    }
}

/* ============================== Color swatches ============================== */

static const char *kColorClass[6] = {
    "npp-color-0",  /* Remove (transparent / dashed) */
    "npp-color-1", "npp-color-2", "npp-color-3", "npp-color-4", "npp-color-5"
};

/* Install the swatch CSS once. RGB matches macOS MenuBuilder.mm tab
 * palette (Yellow / Green / Blue / Orange / Pink). */
static void ensure_color_css(void) {
    static gboolean installed = FALSE;
    if (installed) return;
    installed = TRUE;
    const char *css =
        ".npp-color-0 { background: transparent;"
        "  border: 1px dashed alpha(@theme_fg_color,0.4); }"
        ".npp-color-1 { background: #FCE386; border: 1px solid alpha(@theme_fg_color,0.2); }"
        ".npp-color-2 { background: #A9F08C; border: 1px solid alpha(@theme_fg_color,0.2); }"
        ".npp-color-3 { background: #7AC9F5; border: 1px solid alpha(@theme_fg_color,0.2); }"
        ".npp-color-4 { background: #F5B67A; border: 1px solid alpha(@theme_fg_color,0.2); }"
        ".npp-color-5 { background: #F08CF0; border: 1px solid alpha(@theme_fg_color,0.2); }"
        /* Custom row in a popover menu: match the model-button geometry. */
        ".npp-ctx-row { padding: 4px 12px; min-height: 24px; }";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

/* The click handler dismisses the enclosing popover and activates
 * tab-set-color with the swatch slot (0..5). */
static void on_color_swatch_clicked(GtkButton *btn, gpointer ud) {
    GtkApplication *app = (GtkApplication *)ud;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "color-slot"));
    if (app)
        g_action_group_activate_action(G_ACTION_GROUP(app), "tab-set-color",
                                       g_variant_new_int32(slot));
    GtkWidget *p = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (p) gtk_popover_popdown(GTK_POPOVER(p));
}

/* Build the floating widget that becomes the custom child for an
 * Apply Color / Remove Color row: [colour square][label]. */
static GtkWidget *build_color_swatch_widget(GtkApplication *app, int slot,
                                            const char *label_text)
{
    ensure_color_css();
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "model");      /* inherit menu-row styling */
    gtk_widget_add_css_class(btn, "npp-ctx-row");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *sw   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(sw, 12, 12);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(sw, kColorClass[slot < 0 || slot > 5 ? 0 : slot]);

    GtkWidget *lab = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(lab), 0.0);
    gtk_widget_set_hexpand(lab, TRUE);

    gtk_box_append(GTK_BOX(hbox), sw);
    gtk_box_append(GTK_BOX(hbox), lab);
    gtk_button_set_child(GTK_BUTTON(btn), hbox);

    g_object_set_data(G_OBJECT(btn), "color-slot", GINT_TO_POINTER(slot));
    g_signal_connect(btn, "clicked",
                     G_CALLBACK(on_color_swatch_clicked), app);
    return btn;
}

/* ============================== XML parsing ============================== */

typedef struct {
    char *folder_name;
    char *menu_entry;
    char *menu_item;
    char *menu_sub_menu;
    char *plugin_entry;
    char *plugin_item;
    char *macro_name;
    char *builtin;
    char *display_name;
    int   separator;
} CtxItem;

typedef struct {
    const char *want_root;
    gboolean    in_root;
    GArray     *items;
} CtxParseState;

static void ctx_xml_start(GMarkupParseContext *ctx, const gchar *el,
                          const gchar **names, const gchar **vals,
                          gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    CtxParseState *st = ud;
    if (strcmp(el, st->want_root) == 0) { st->in_root = TRUE; return; }
    if (!st->in_root || strcmp(el, "Item") != 0) return;

    CtxItem it = { 0 };
    for (int i = 0; names[i]; i++) {
        if      (!strcmp(names[i], "FolderName"))            it.folder_name   = g_strdup(vals[i]);
        else if (!strcmp(names[i], "MenuEntryName"))         it.menu_entry    = g_strdup(vals[i]);
        else if (!strcmp(names[i], "MenuItemName"))          it.menu_item     = g_strdup(vals[i]);
        else if (!strcmp(names[i], "MenuSubMenuName"))       it.menu_sub_menu = g_strdup(vals[i]);
        else if (!strcmp(names[i], "PluginEntryName"))       it.plugin_entry  = g_strdup(vals[i]);
        else if (!strcmp(names[i], "PluginCommandItemName")) it.plugin_item   = g_strdup(vals[i]);
        else if (!strcmp(names[i], "MacroEntryName"))        it.macro_name    = g_strdup(vals[i]);
        else if (!strcmp(names[i], "BuiltIn"))               it.builtin       = g_strdup(vals[i]);
        else if (!strcmp(names[i], "ItemNameAs"))            it.display_name  = g_strdup(vals[i]);
        else if (!strcmp(names[i], "id") && !strcmp(vals[i], "0"))
            it.separator = 1;
    }
    g_array_append_val(st->items, it);
}

static void ctx_xml_end(GMarkupParseContext *ctx, const gchar *el,
                        gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    CtxParseState *st = ud;
    if (strcmp(el, st->want_root) == 0) st->in_root = FALSE;
}

static GMarkupParser s_ctxparser = { ctx_xml_start, ctx_xml_end, NULL, NULL, NULL };

static GArray *parse_ctxmenu(const char *path, const char *want_root) {
    if (!path) return NULL;
    gchar *xml = NULL;
    if (!g_file_get_contents(path, &xml, NULL, NULL)) return NULL;

    CtxParseState st = { .want_root = want_root, .in_root = FALSE,
                         .items = g_array_new(FALSE, FALSE, sizeof(CtxItem)) };
    GMarkupParseContext *gctx = g_markup_parse_context_new(&s_ctxparser, 0, &st, NULL);
    g_markup_parse_context_parse(gctx, xml, -1, NULL);
    g_markup_parse_context_end_parse(gctx, NULL);
    g_markup_parse_context_free(gctx);
    g_free(xml);
    return st.items;
}

static void free_items(GArray *a) {
    if (!a) return;
    for (guint i = 0; i < a->len; i++) {
        CtxItem *it = &g_array_index(a, CtxItem, i);
        g_free(it->folder_name);
        g_free(it->menu_entry);
        g_free(it->menu_item);
        g_free(it->menu_sub_menu);
        g_free(it->plugin_entry);
        g_free(it->plugin_item);
        g_free(it->macro_name);
        g_free(it->builtin);
        g_free(it->display_name);
    }
    g_array_free(a, TRUE);
}

/* ============================== Build ============================== */

static int tab_color_slot(const char *item) {
    if (!item) return -1;
    if (!g_ascii_strcasecmp(item, "Remove Color")) return 0;
    if (!g_ascii_strncasecmp(item, "Apply Color ", 12) &&
        item[12] >= '1' && item[12] <= '5')
        return item[12] - '0';
    return -1;
}

/* Translate a display string through the i18n catalog — exactly how
 * macOS funnels FolderName / ItemNameAs / item titles through NppLocalizer
 * before adding them to the menu. */
static const char *xlate(const char *s) {
    return (s && *s) ? i18n_translate(s) : (s ? s : "");
}

/* Build a GMenuItem for an action with no target. */
static GMenuItem *plain_item(const char *label, const char *action) {
    GMenuItem *it = g_menu_item_new(label, NULL);
    g_menu_item_set_attribute(it, "action", "s", action);
    return it;
}

/* Append parsed items into a CtxMenu's GMenu. */
static void populate(CtxMenu *m, GArray *items) {
    if (!items) return;

    for (guint i = 0; i < items->len; i++) {
        CtxItem *it = &g_array_index(items, CtxItem, i);

        if (it->separator) {
            FolderInfo *folder = it->folder_name && *it->folder_name
                ? g_hash_table_lookup(m->folders, it->folder_name) : NULL;
            new_section_in(m, folder);
            continue;
        }

        /* Determine action + display label + (optional) color slot. */
        const char *action     = NULL;
        const char *label_eng  = NULL;
        int         color_slot = -1;

        if (it->builtin && !g_ascii_strcasecmp(it->builtin, "PinTab")) {
            action    = "app.tab-pin-toggle";
            label_eng = it->display_name ? it->display_name : "Pin Tab";
        }
        else if ((color_slot = tab_color_slot(it->menu_item)) >= 0) {
            /* tab-set-color is parametric — built below as a custom child. */
            label_eng = it->display_name ? it->display_name : it->menu_item;
        }
        else if (it->menu_entry && it->menu_item) {
            action    = lookup_action(it->menu_entry, it->menu_sub_menu,
                                      it->menu_item);
            label_eng = it->display_name ? it->display_name : it->menu_item;
        }
        else if (it->plugin_entry || it->macro_name) {
            /* Plugin / named-macro commands aren't registered as GActions
             * on Linux yet — silently skip, matching macOS behaviour
             * when the plugin isn't loaded. */
            continue;
        }

        if (!label_eng) continue;
        if (color_slot < 0 && !action) continue;

        /* Where does this item land? */
        FolderInfo *folder = NULL;
        GMenu *target;
        if (it->folder_name && *it->folder_name) {
            folder = get_folder(m, it->folder_name);
            target = folder->cur_section;
        } else {
            target = m->cur_section;
        }

        if (color_slot >= 0) {
            /* Custom-child row: colour swatch + label; click activates
             * app.tab-set-color(slot). */
            GtkWidget *w = build_color_swatch_widget(m->app, color_slot,
                                                    xlate(label_eng));
            const char *slot = ctxmenu_register_custom(m, w);
            GMenuItem *gi = g_menu_item_new(NULL, NULL);
            g_menu_item_set_attribute(gi, "custom", "s", slot);
            g_menu_append_item(target, gi);
            g_object_unref(gi);
        } else {
            GMenuItem *gi = plain_item(xlate(label_eng), action);
            g_menu_append_item(target, gi);
            g_object_unref(gi);
        }
    }
}

/* ============================== Popup ============================== */

typedef struct {
    GtkWidget *popover;
    CtxMenu   *menu;
} PopupCtx;

/* Run on idle out of the popover's "closed" signal — unparenting from
 * inside the emission deadlocks the popdown animation. */
static gboolean popup_teardown(gpointer ud) {
    PopupCtx *p = ud;
    if (p->popover) gtk_widget_unparent(p->popover);
    ctxmenu_free(p->menu);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void on_popover_closed(GtkPopover *pop, gpointer ud) {
    (void)pop;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, popup_teardown, ud, NULL);
}

void ctxmenu_popup_at(CtxMenu *m, GtkWidget *anchor, double x, double y) {
    if (!m || !anchor) { ctxmenu_free(m); return; }
    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(m->root));
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    /* Hover-to-open submenus (the macOS NSMenu behaviour). */
    gtk_popover_menu_set_flags(GTK_POPOVER_MENU(popover),
                               GTK_POPOVER_MENU_NESTED);
    /* Attach registered custom children. */
    for (guint i = 0; i < m->customs->len; i++) {
        CtxCustom *c = m->customs->pdata[i];
        gtk_popover_menu_add_child(GTK_POPOVER_MENU(popover),
                                   c->widget, c->slot);
    }
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_widget_set_parent(popover, anchor);
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

    PopupCtx *p = g_new0(PopupCtx, 1);
    p->popover = popover;
    p->menu    = m;
    g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), p);

    gtk_popover_popup(GTK_POPOVER(popover));
}

/* ============================== Public builders ============================== */

CtxMenu *ctxmenu_build_scintilla(GtkApplication *app) {
    CtxMenu *m = ctxmenu_new(app);
    gchar *user = npp_user_file(NULL, "contextMenu.xml");
    GArray *items = parse_ctxmenu(user, "ScintillaContextMenu");
    g_free(user);
    if (!items || items->len == 0) {
        if (items) free_items(items);
        gchar *bundle = npp_bundle_file(NULL, "contextMenu.xml");
        items = parse_ctxmenu(bundle, "ScintillaContextMenu");
        g_free(bundle);
    }
    populate(m, items);
    free_items(items);
    return m;
}

CtxMenu *ctxmenu_build_tab(GtkApplication *app) {
    CtxMenu *m = ctxmenu_new(app);
    /* macOS parity: tabContextMenu_example.xml is an inactive template —
     * read only tabContextMenu.xml (user must rename to opt in), falling
     * back to the bundled default. */
    gchar *user = npp_user_file(NULL, "tabContextMenu.xml");
    GArray *items = NULL;
    if (g_file_test(user, G_FILE_TEST_EXISTS))
        items = parse_ctxmenu(user, "TabContextMenu");
    g_free(user);
    if (!items || items->len == 0) {
        if (items) free_items(items);
        gchar *bundle = npp_bundle_file(NULL, "tabContextMenu.xml");
        items = parse_ctxmenu(bundle, "TabContextMenu");
        g_free(bundle);
    }
    populate(m, items);
    free_items(items);
    return m;
}

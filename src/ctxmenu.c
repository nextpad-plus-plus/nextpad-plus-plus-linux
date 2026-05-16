/*
 * ctxmenu.c — XML-driven context menus matching macOS port.
 */
#include "ctxmenu.h"
#include "gtk_compat.h"
#include "paths.h"
#include <string.h>

/* (top-menu-name, item-label) → "app.action-name". Case-insensitive on
 * lookup; entries are stored already lower-cased. */
static GHashTable *s_index = NULL;

static gchar *lc_key(const char *top, const char *item) {
    gchar *t = g_ascii_strdown(top  ? top  : "", -1);
    gchar *i = g_ascii_strdown(item ? item : "", -1);
    gchar *out = g_strdup_printf("%s\t%s", t, i);
    g_free(t); g_free(i);
    return out;
}

static void walk_model(GMenuModel *model, const char *top_name);

static void walk_section(GMenuModel *section, const char *top_name) {
    int n = g_menu_model_get_n_items(section);
    for (int i = 0; i < n; i++) {
        gchar *label = NULL, *action = NULL;
        g_menu_model_get_item_attribute(section, i, G_MENU_ATTRIBUTE_LABEL,  "s", &label);
        g_menu_model_get_item_attribute(section, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);

        if (label && action) {
            /* Strip the "_X" accelerator marker if present. */
            gchar *clean = g_new(gchar, strlen(label) + 1);
            int j = 0;
            for (int k = 0; label[k]; k++)
                if (label[k] != '_') clean[j++] = label[k];
            clean[j] = '\0';
            gchar *key = lc_key(top_name, clean);
            g_hash_table_insert(s_index, key, g_strdup(action));
            g_free(clean);
        }

        /* Nested section / submenu? */
        GMenuModel *sub = NULL;
        sub = g_menu_model_get_item_link(section, i, G_MENU_LINK_SECTION);
        if (sub) {
            walk_section(sub, top_name);
            g_object_unref(sub);
        }
        sub = g_menu_model_get_item_link(section, i, G_MENU_LINK_SUBMENU);
        if (sub) {
            /* Items inside a submenu keep the same top-level entry name
             * — macOS XML doesn't require MenuSubMenuName for most lookups. */
            walk_section(sub, top_name);
            g_object_unref(sub);
        }

        g_free(label);
        g_free(action);
    }
}

static void walk_model(GMenuModel *model, const char *top_name) {
    walk_section(model, top_name);
}

void ctxmenu_index_from_model(GMenuModel *model) {
    if (s_index) g_hash_table_destroy(s_index);
    s_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!model) return;

    /* Top level: iterate menus, each has a label (e.g. "_File"). */
    int n = g_menu_model_get_n_items(model);
    for (int i = 0; i < n; i++) {
        gchar *label = NULL;
        g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
        GMenuModel *sub = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU);
        if (label && sub) {
            /* Strip the "_X" accelerator marker. */
            gchar *clean = g_new(gchar, strlen(label) + 1);
            int j = 0;
            for (int k = 0; label[k]; k++)
                if (label[k] != '_') clean[j++] = label[k];
            clean[j] = '\0';
            walk_model(sub, clean);
            g_free(clean);
        }
        g_free(label);
        if (sub) g_object_unref(sub);
    }
}

static const char *lookup_action(const char *entry, const char *item) {
    if (!s_index || !entry || !item) return NULL;
    gchar *k = lc_key(entry, item);
    const char *v = (const char *)g_hash_table_lookup(s_index, k);
    g_free(k);
    return v;
}

/* ------------------------------------------------------------------ */
/* XML parsing for both context menu schemas                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char *folder_name;     /* may be NULL */
    char *menu_entry;      /* MenuEntryName */
    char *menu_item;       /* MenuItemName */
    char *plugin_entry;    /* PluginEntryName */
    char *plugin_item;     /* PluginCommandItemName */
    char *macro_name;      /* MacroEntryName */
    char *builtin;         /* BuiltIn="…" */
    char *display_name;    /* ItemNameAs override */
    int   separator;       /* id="0" */
} CtxItem;

typedef struct {
    const char *want_root;   /* "ScintillaContextMenu" or "TabContextMenu" */
    gboolean    in_root;
    GArray     *items;       /* CtxItem */
} CtxParseState;

static void ctx_xml_start(GMarkupParseContext *ctx, const gchar *el,
                          const gchar **names, const gchar **vals,
                          gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    CtxParseState *st = ud;
    if (strcmp(el, st->want_root) == 0) {
        st->in_root = TRUE;
        return;
    }
    if (!st->in_root || strcmp(el, "Item") != 0) return;

    CtxItem it = { 0 };
    for (int i = 0; names[i]; i++) {
        if      (!strcmp(names[i], "FolderName"))           it.folder_name = g_strdup(vals[i]);
        else if (!strcmp(names[i], "MenuEntryName"))        it.menu_entry  = g_strdup(vals[i]);
        else if (!strcmp(names[i], "MenuItemName"))         it.menu_item   = g_strdup(vals[i]);
        else if (!strcmp(names[i], "PluginEntryName"))      it.plugin_entry= g_strdup(vals[i]);
        else if (!strcmp(names[i], "PluginCommandItemName"))it.plugin_item = g_strdup(vals[i]);
        else if (!strcmp(names[i], "MacroEntryName"))       it.macro_name  = g_strdup(vals[i]);
        else if (!strcmp(names[i], "BuiltIn"))              it.builtin     = g_strdup(vals[i]);
        else if (!strcmp(names[i], "ItemNameAs"))           it.display_name= g_strdup(vals[i]);
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
        g_free(it->plugin_entry);
        g_free(it->plugin_item);
        g_free(it->macro_name);
        g_free(it->builtin);
        g_free(it->display_name);
    }
    g_array_free(a, TRUE);
}

/* ------------------------------------------------------------------ */
/* Build a GtkMenu from a parsed item list                            */
/* ------------------------------------------------------------------ */

static void on_action_activate(GtkButton *mi, gpointer ud) {
    GtkApplication *app = (GtkApplication *)ud;
    const char *action_name = g_object_get_data(G_OBJECT(mi), "action-name");
    if (!app || !action_name) return;
    g_action_group_activate_action(G_ACTION_GROUP(app), action_name, NULL);
}

extern void editor_tab_toggle_pin(GtkWidget *sci); /* if defined */

static void on_builtin_pintab(GtkButton *mi, gpointer ud) {
    (void)mi;
    GtkApplication *app = (GtkApplication *)ud;
    if (app)
        g_action_group_activate_action(G_ACTION_GROUP(app), "tab-pin", NULL);
}

/* Append the parsed item list onto `menu`. Returns the number of action
 * items added (0 means the XML produced nothing usable). */
static int populate_menu(NppMenu *menu, GArray *items, GtkApplication *app) {
    if (!items) return 0;
    int count = 0;
    /* Track the currently-open folder submenu by name so contiguous items
     * with the same FolderName collapse into one submenu. */
    NppMenu *cur_folder = NULL;
    char     cur_folder_name[128] = "";

    for (guint i = 0; i < items->len; i++) {
        CtxItem *it = &g_array_index(items, CtxItem, i);

        if (it->separator) {
            cur_folder = NULL; cur_folder_name[0] = '\0';
            npp_menu_add_separator(menu);
            continue;
        }

        /* Determine action + display name. */
        const char *action = NULL;
        const char *label  = NULL;

        if (it->builtin && !g_ascii_strcasecmp(it->builtin, "PinTab")) {
            action = "tab-pin";
            label  = it->display_name ? it->display_name : "Pin Tab";
        }
        else if (it->menu_entry && it->menu_item) {
            action = lookup_action(it->menu_entry, it->menu_item);
            label  = it->display_name ? it->display_name : it->menu_item;
        }
        else if (it->plugin_entry && it->plugin_item) {
            /* Plugin commands not wired through GAction yet — skip. */
            continue;
        }
        else if (it->macro_name) {
            /* Macros not exposed as GActions yet — skip. */
            continue;
        }

        if (!action) continue;
        if (!label)  continue;

        gboolean is_pin = it->builtin &&
                          !g_ascii_strcasecmp(it->builtin, "PinTab");
        GCallback cb = is_pin ? G_CALLBACK(on_builtin_pintab)
                              : G_CALLBACK(on_action_activate);

        /* Folder grouping. */
        NppMenu *target = menu;
        if (it->folder_name && *it->folder_name) {
            if (strcmp(cur_folder_name, it->folder_name) != 0) {
                cur_folder = npp_menu_add_submenu(menu, it->folder_name);
                g_strlcpy(cur_folder_name, it->folder_name,
                          sizeof(cur_folder_name));
            }
            target = cur_folder;
        } else {
            cur_folder = NULL; cur_folder_name[0] = '\0';
        }

        GtkWidget *mi = npp_menu_add(target, label, cb, app);
        g_object_set_data_full(G_OBJECT(mi), "action-name",
                               g_strdup(action), g_free);
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

int ctxmenu_append_scintilla(NppMenu *menu, GtkApplication *app) {
    gchar *user = npp_user_file(NULL, "contextMenu.xml");
    GArray *items = parse_ctxmenu(user, "ScintillaContextMenu");
    g_free(user);
    if (!items || items->len == 0) {
        if (items) free_items(items);
        gchar *bundle = npp_bundle_file(NULL, "contextMenu.xml");
        items = parse_ctxmenu(bundle, "ScintillaContextMenu");
        g_free(bundle);
    }
    int n = populate_menu(menu, items, app);
    free_items(items);
    return n;
}

int ctxmenu_append_tab(NppMenu *menu, GtkApplication *app) {
    /* macOS-parity: ~/.nextpad++/tabContextMenu_example.xml is an
     * inactive template — it must NOT be read until the user opts in
     * by renaming it (dropping `_example`). Read tabContextMenu.xml
     * only, falling back to the bundled default if neither exists. */
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
    int n = populate_menu(menu, items, app);
    free_items(items);
    return n;
}

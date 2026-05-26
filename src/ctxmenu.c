/*
 * ctxmenu.c — XML-driven editor + tab context menus.
 *
 * Mirrors macOS MainWindowController.mm:_buildEditorContextMenuFromXML and
 * NppTabBar.mm:_buildTabContextMenuFromXML — same XML attributes, same
 * lookup semantics (top-level menu → optional submenu narrowing → item by
 * label), same FolderName grouping, same id="0" → separator rule, and
 * same display-string localization via the i18n catalog.
 */
#include "ctxmenu.h"
#include "gtk_compat.h"
#include "paths.h"
#include "i18n.h"
#include <string.h>

/* (top-menu, submenu-or-"", item) → "app.action-name", normalised.
 * Submenu-aware so the editor XML's three "Using 1st Style" entries (which
 * live in different parent submenus: Style All Occurrences / Style One /
 * Clear Style) resolve to their correct distinct actions. */
static GHashTable *s_index = NULL;

/* Drop a trailing ellipsis (ASCII "..." or U+2026 "…") and trailing
 * whitespace. The context-menu XMLs spell it "..." while the GTK menu
 * model uses "…"; normalising both ends makes the lookup match. */
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

/* (top, sub, item) → "top<TAB>sub<TAB>item" all lowercased, no ellipsis.
 * `sub` may be NULL/"" for the top-level scope. */
static gchar *key3(const char *top, const char *sub, const char *item) {
    gchar *t = norm_part(top);
    gchar *s = norm_part(sub);
    gchar *i = norm_part(item);
    gchar *r = g_strdup_printf("%s\t%s\t%s", t, s, i);
    g_free(t); g_free(s); g_free(i);
    return r;
}

/* Insert (top, sub, item) → action. Also inserts (top, "", item) for
 * lookups where MenuSubMenuName isn't specified — first-found wins so we
 * don't lose the natural top-down menu order in the face of duplicate
 * item labels across sibling submenus. */
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

/* Strip GTK mnemonic markers ("_") from a model label. */
static gchar *strip_mnemonic(const char *s) {
    gchar *r = g_new(gchar, strlen(s) + 1);
    int j = 0;
    for (int k = 0; s[k]; k++) if (s[k] != '_') r[j++] = s[k];
    r[j] = '\0';
    return r;
}

/* Walk a GMenuModel scope (section or submenu), indexing every (label,
 * action) pair with the supplied `top` and `cur_sub`. Sections inherit
 * `cur_sub` from their parent; descending into a submenu rebinds it. */
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
            /* Re-bind cur_sub to this submenu's label — that gives the
             * (top, sub, item) keys macOS's MenuSubMenuName narrows on. */
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

    /* Top level: each item is a top-level menu with a submenu link. */
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

/* Try the submenu-narrowed key first, fall back to the generic (top, item)
 * key — mirrors macOS's "if subMenuName given, narrow searchIn; else search
 * recursively from the top menu." */
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

/* ------------------------------------------------------------------ */
/* XML parsing                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char *folder_name;
    char *menu_entry;
    char *menu_item;
    char *menu_sub_menu;     /* MenuSubMenuName — narrows lookup */
    char *plugin_entry;
    char *plugin_item;
    char *macro_name;
    char *builtin;
    char *display_name;      /* ItemNameAs override */
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

/* ------------------------------------------------------------------ */
/* Build a GtkMenu from a parsed item list                            */
/* ------------------------------------------------------------------ */

/* Tab-colour items use parameterised "tab-set-color" (0 = clear, 1..5). */
static int tab_color_slot(const char *item) {
    if (!item) return -1;
    if (!g_ascii_strcasecmp(item, "Remove Color")) return 0;
    if (!g_ascii_strncasecmp(item, "Apply Color ", 12) &&
        item[12] >= '1' && item[12] <= '5')
        return item[12] - '0';
    return -1;
}

/* Resolve a display string through the i18n catalog — exactly how macOS
 * runs FolderName / ItemNameAs / item titles through NppLocalizer before
 * adding them to the menu. Returns the input unchanged when no catalog
 * entry matches (safe to apply blindly to plugin/macro/custom labels). */
static const char *xlate(const char *s) {
    return (s && *s) ? i18n_translate(s) : (s ? s : "");
}

/* Resolve the menubar-indexed action name ("app.cut") into the full
 * "app.foo" form GMenuModel wants. lookup_action already returns the
 * prefixed form, so this is just a NULL-safe pass-through. */
static const char *full_action(const char *bare_or_prefixed) {
    return bare_or_prefixed;
}

/* Append the parsed item list onto `menu`. Returns the number of action
 * items added (0 → the XML produced nothing usable). */
static int populate_menu(NppMenu *menu, GArray *items, GtkApplication *app) {
    (void)app;
    if (!items) return 0;
    int count = 0;
    /* Folder submenus keyed by FolderName (original, untranslated) so
     * non-contiguous items with the same FolderName still collapse into
     * one submenu — mirrors macOS's `folders` NSMutableDictionary. */
    GHashTable *folders = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, NULL);

    for (guint i = 0; i < items->len; i++) {
        CtxItem *it = &g_array_index(items, CtxItem, i);

        if (it->separator) {
            /* A separator inside a folder belongs to that folder's
             * submenu (macOS passes it to the folder NSMenu). */
            NppMenu *target = menu;
            if (it->folder_name && *it->folder_name) {
                NppMenu *fld = g_hash_table_lookup(folders, it->folder_name);
                if (fld) target = fld;
            }
            npp_menu_add_separator(target);
            continue;
        }

        const char *action = NULL;
        const char *label  = NULL;
        int color_slot = -1;

        if (it->builtin && !g_ascii_strcasecmp(it->builtin, "PinTab")) {
            action = "app.tab-pin-toggle";
            label  = it->display_name ? it->display_name : "Pin Tab";
        }
        else if ((color_slot = tab_color_slot(it->menu_item)) >= 0) {
            /* Handled below via npp_menu_add_action_target. */
            label  = it->display_name ? it->display_name : it->menu_item;
        }
        else if (it->menu_entry && it->menu_item) {
            action = lookup_action(it->menu_entry, it->menu_sub_menu,
                                   it->menu_item);
            label  = it->display_name ? it->display_name : it->menu_item;
        }
        else if (it->plugin_entry && it->plugin_item) {
            /* macOS approach (MainWindowController.mm:192-213): the
             * Plugins menu is the source of truth; lookup_action finds
             * the (Plugins, PluginEntryName, PluginCommandItemName) key
             * in our GMenuModel index. Silent skip when the plugin
             * isn't bundled, matching macOS. */
            action = lookup_action("Plugins", it->plugin_entry, it->plugin_item);
            label  = it->display_name ? it->display_name : it->plugin_item;
        }
        else if (it->macro_name) {
            /* Named macros not exposed as GActions yet — skip. */
            continue;
        }

        gboolean is_color = (color_slot >= 0);
        if (!is_color && !action) continue;
        if (!label) continue;

        /* Folder grouping — translate the folder title (macOS xlate)
         * the first time we instantiate it so the user sees it in their
         * UI language. */
        NppMenu *target = menu;
        if (it->folder_name && *it->folder_name) {
            NppMenu *fld = g_hash_table_lookup(folders, it->folder_name);
            if (!fld) {
                fld = npp_menu_add_submenu(menu, xlate(it->folder_name));
                g_hash_table_insert(folders, g_strdup(it->folder_name), fld);
            }
            target = fld;
        }

        if (is_color) {
            npp_menu_add_action_target(target, xlate(label),
                                       "app.tab-set-color",
                                       g_variant_new_int32(color_slot));
        } else {
            npp_menu_add_action(target, xlate(label), full_action(action));
        }
        count++;
    }
    g_hash_table_destroy(folders);
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
    /* macOS-parity: tabContextMenu_example.xml is an inactive template —
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
    int n = populate_menu(menu, items, app);
    free_items(items);
    return n;
}

/*
 * Shortcut Mapper — Linux/GTK3 port of macOS ShortcutMapperWindowController.
 *
 *   - 5 tabs: Main menu / Macros / Run commands / Plugin commands /
 *     Scintilla commands.
 *   - Filter entry at top (live, case-insensitive substring match).
 *   - Bottom toolbar: Modify… / Delete / Save / Close.
 *   - Conflict-warning area: red label showing duplicate accelerator
 *     collisions for the currently-selected row.
 *   - Modify dialog: 2×2 modifier checkboxes + key dropdown + live conflict
 *     warning; OK disabled while a conflict is present.
 *   - Persistence: ~/<APP_CONFIG_DIR>/shortcuts.xml (GMarkupParser load,
 *     hand-written write) matching the macOS schema.
 *   - Saving applies new accelerators to the live GtkApplication via
 *     gtk_application_set_accels_for_action() so menu shortcuts update
 *     without restart.
 */

#include "shortcutmap.h"
#include "paths.h"
#include "gtk_compat.h"
#include "branding.h"
#include "macro.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Data model                                                             */
/* ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    TAB_MAIN     = 0,
    TAB_MACROS   = 1,
    TAB_RUN      = 2,
    TAB_PLUGIN   = 3,
    TAB_SCI      = 4,
    TAB_COUNT    = 5
} SmTab;

typedef struct {
    /* Display */
    gchar    *name;        /* "Save" / "Trim Trailing…" */
    gchar    *category;    /* "File" / plugin name / "" */

    /* Shortcut */
    gboolean  has_ctrl, has_alt, has_shift, has_super;
    guint     keycode;     /* 0 = unbound; canonical Windows-VK style */

    /* Lookup keys */
    gchar    *action_id;   /* "app.save" (Main tab)                    */
    gchar    *plugin_name; /* Plugin tab                               */
    gint      command_id;  /* SCI_* id / plugin internal id            */

    /* State */
    gboolean  modified;    /* user changed it during this session      */
} ShortcutRow;

static void shortcut_row_free(gpointer p) {
    ShortcutRow *r = (ShortcutRow *)p;
    if (!r) return;
    g_free(r->name);
    g_free(r->category);
    g_free(r->action_id);
    g_free(r->plugin_name);
    g_free(r);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Key-code helpers (Windows-VK style codes for shortcuts.xml parity)     */
/* ═══════════════════════════════════════════════════════════════════════ */

/* Map a GDK keyval to the Windows-VK-style code used in shortcuts.xml. */
static guint vk_from_gdk(guint kv) {
    if (kv >= GDK_KEY_a && kv <= GDK_KEY_z) return 'A' + (kv - GDK_KEY_a);
    if (kv >= GDK_KEY_A && kv <= GDK_KEY_Z) return kv;
    if (kv >= GDK_KEY_0 && kv <= GDK_KEY_9) return kv;
    if (kv >= GDK_KEY_F1 && kv <= GDK_KEY_F12) return 112 + (kv - GDK_KEY_F1);
    switch (kv) {
        case GDK_KEY_BackSpace: return 8;
        case GDK_KEY_Tab:       return 9;
        case GDK_KEY_Return:    return 13;
        case GDK_KEY_Escape:    return 27;
        case GDK_KEY_space:     return 32;
        case GDK_KEY_Page_Up:   return 33;
        case GDK_KEY_Page_Down: return 34;
        case GDK_KEY_End:       return 35;
        case GDK_KEY_Home:      return 36;
        case GDK_KEY_Left:      return 37;
        case GDK_KEY_Up:        return 38;
        case GDK_KEY_Right:     return 39;
        case GDK_KEY_Down:      return 40;
        case GDK_KEY_Insert:    return 45;
        case GDK_KEY_Delete:    return 46;
        case GDK_KEY_semicolon: return 186;
        case GDK_KEY_equal:     return 187;
        case GDK_KEY_comma:     return 188;
        case GDK_KEY_minus:     return 189;
        case GDK_KEY_period:    return 190;
        case GDK_KEY_slash:     return 191;
        case GDK_KEY_grave:     return 192;
        case GDK_KEY_bracketleft:  return 219;
        case GDK_KEY_backslash:    return 220;
        case GDK_KEY_bracketright: return 221;
        case GDK_KEY_apostrophe:   return 222;
    }
    return 0;
}

/* Public: macro.c pushes accels for bound macros with the same mapping. */
guint shortcut_vk_to_gdk(guint vk);

/* Reverse map: VK → GDK keyval, for building gtk_application_set_accels. */
static guint gdk_from_vk(guint vk) {
    if (vk >= 'A' && vk <= 'Z') return GDK_KEY_a + (vk - 'A');
    if (vk >= '0' && vk <= '9') return vk;
    if (vk >= 112 && vk <= 123)  return GDK_KEY_F1 + (vk - 112);
    switch (vk) {
        case 8:   return GDK_KEY_BackSpace;
        case 9:   return GDK_KEY_Tab;
        case 13:  return GDK_KEY_Return;
        case 27:  return GDK_KEY_Escape;
        case 32:  return GDK_KEY_space;
        case 33:  return GDK_KEY_Page_Up;
        case 34:  return GDK_KEY_Page_Down;
        case 35:  return GDK_KEY_End;
        case 36:  return GDK_KEY_Home;
        case 37:  return GDK_KEY_Left;
        case 38:  return GDK_KEY_Up;
        case 39:  return GDK_KEY_Right;
        case 40:  return GDK_KEY_Down;
        case 45:  return GDK_KEY_Insert;
        case 46:  return GDK_KEY_Delete;
        case 186: return GDK_KEY_semicolon;
        case 187: return GDK_KEY_equal;
        case 188: return GDK_KEY_comma;
        case 189: return GDK_KEY_minus;
        case 190: return GDK_KEY_period;
        case 191: return GDK_KEY_slash;
        case 192: return GDK_KEY_grave;
        case 219: return GDK_KEY_bracketleft;
        case 220: return GDK_KEY_backslash;
        case 221: return GDK_KEY_bracketright;
        case 222: return GDK_KEY_apostrophe;
    }
    return 0;
}

guint shortcut_vk_to_gdk(guint vk) { return gdk_from_vk(vk); }

/* Names exposed in the Modify-dialog key dropdown. The first entry MUST
 * be "None" (= unbound). Order matches the macOS popup. */
static const char *const KEY_POPUP[] = {
    "None",
    "A","B","C","D","E","F","G","H","I","J","K","L","M",
    "N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
    "0","1","2","3","4","5","6","7","8","9",
    "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    "Backspace","Tab","Enter","Escape","Space",
    "Page Up","Page Down","Home","End",
    "Left","Up","Right","Down",
    "Insert","Delete",
    ";","=",",","-",".","[","]","'","`","/",
    NULL
};

static const char *popup_name_for_vk(guint vk) {
    if (vk == 0) return "None";
    if (vk >= 'A' && vk <= 'Z') {
        static char buf[2] = {0,0}; buf[0] = (char)vk; return buf;
    }
    if (vk >= '0' && vk <= '9') {
        static char buf[2] = {0,0}; buf[0] = (char)vk; return buf;
    }
    if (vk >= 112 && vk <= 123) {
        static char buf[4]; g_snprintf(buf, sizeof(buf), "F%u", vk - 111); return buf;
    }
    switch (vk) {
        case 8:   return "Backspace";
        case 9:   return "Tab";
        case 13:  return "Enter";
        case 27:  return "Escape";
        case 32:  return "Space";
        case 33:  return "Page Up";
        case 34:  return "Page Down";
        case 35:  return "End";
        case 36:  return "Home";
        case 37:  return "Left";
        case 38:  return "Up";
        case 39:  return "Right";
        case 40:  return "Down";
        case 45:  return "Insert";
        case 46:  return "Delete";
        case 186: return ";"; case 187: return "=";
        case 188: return ","; case 189: return "-";
        case 190: return "."; case 191: return "/";
        case 192: return "`"; case 219: return "[";
        case 220: return "\\"; case 221: return "]";
        case 222: return "'";
    }
    return "None";
}

static guint vk_from_popup_name(const char *n) {
    if (!n || !*n || g_strcmp0(n, "None") == 0) return 0;
    if (strlen(n) == 1) {
        char c = n[0];
        if (c >= 'a' && c <= 'z') return (guint)(c - 32);
        return (guint)(guchar)c;
    }
    if (n[0] == 'F' && (n[1] >= '0' && n[1] <= '9')) {
        int n2 = atoi(n + 1);
        if (n2 >= 1 && n2 <= 12) return 111 + n2;
    }
    if (!strcmp(n, "Backspace")) return 8;
    if (!strcmp(n, "Tab"))       return 9;
    if (!strcmp(n, "Enter"))     return 13;
    if (!strcmp(n, "Escape"))    return 27;
    if (!strcmp(n, "Space"))     return 32;
    if (!strcmp(n, "Page Up"))   return 33;
    if (!strcmp(n, "Page Down")) return 34;
    if (!strcmp(n, "End"))       return 35;
    if (!strcmp(n, "Home"))      return 36;
    if (!strcmp(n, "Left"))      return 37;
    if (!strcmp(n, "Up"))        return 38;
    if (!strcmp(n, "Right"))     return 39;
    if (!strcmp(n, "Down"))      return 40;
    if (!strcmp(n, "Insert"))    return 45;
    if (!strcmp(n, "Delete"))    return 46;
    return 0;
}

/* Build a "Ctrl+Shift+S"-style label using GtkAccelLabel rules. */
static gchar *shortcut_display(const ShortcutRow *r) {
    if (r->keycode == 0) return g_strdup("");
    guint kv = gdk_from_vk(r->keycode);
    GdkModifierType m = 0;
    if (r->has_ctrl)  m |= GDK_CONTROL_MASK;
    if (r->has_alt)   m |= GDK_ALT_MASK;
    if (r->has_shift) m |= GDK_SHIFT_MASK;
    if (r->has_super) m |= GDK_SUPER_MASK;
    gchar *lbl = gtk_accelerator_get_label(kv, m);
    return lbl ? lbl : g_strdup("");
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Scintilla key-binding defaults (mirrors macOS scintKeyDefs[])          */
/* ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;
    int   sciID;
    gboolean ctrl, alt, shift;
    guint key;
} SciKeyDef;

static const SciKeyDef SCI_DEFS[] = {
    {"SCI_SELECTALL",            2013, TRUE,  FALSE, FALSE, 'A'},
    {"SCI_CLEAR",                2180, FALSE, FALSE, FALSE, 46},
    {"SCI_CLEARALL",             2004, FALSE, FALSE, FALSE,  0},
    {"SCI_UNDO",                 2176, TRUE,  FALSE, FALSE, 'Z'},
    {"SCI_REDO",                 2011, TRUE,  FALSE, TRUE,  'Z'},
    {"SCI_NEWLINE",              2329, FALSE, FALSE, FALSE, 13},
    {"SCI_TAB",                  2327, FALSE, FALSE, FALSE,  9},
    {"SCI_BACKTAB",              2328, FALSE, FALSE, TRUE,   9},
    {"SCI_FORMFEED",             2330, FALSE, FALSE, FALSE,  0},
    {"SCI_ZOOMIN",               2333, TRUE,  FALSE, FALSE, 187},
    {"SCI_ZOOMOUT",              2334, TRUE,  FALSE, FALSE, 189},
    {"SCI_SETZOOM",              2373, TRUE,  FALSE, FALSE, 191},
    {"SCI_SELECTIONDUPLICATE",   2469, TRUE,  FALSE, FALSE, 'D'},
    {"SCI_LINESJOIN",            2288, FALSE, FALSE, FALSE,  0},
    {"SCI_SCROLLCARET",          2169, FALSE, FALSE, FALSE,  0},
    {"SCI_EDITTOGGLEOVERTYPE",   2324, FALSE, FALSE, FALSE, 45},
    {"SCI_MOVECARETINSIDEVIEW",  2401, FALSE, FALSE, FALSE,  0},
    {"SCI_LINEDOWN",             2300, FALSE, FALSE, FALSE, 40},
    {"SCI_LINEDOWNEXTEND",       2301, FALSE, FALSE, TRUE,  40},
    {"SCI_LINESCROLLDOWN",       2342, TRUE,  FALSE, FALSE, 40},
    {"SCI_LINEUP",               2302, FALSE, FALSE, FALSE, 38},
    {"SCI_LINEUPEXTEND",         2303, FALSE, FALSE, TRUE,  38},
    {"SCI_LINESCROLLUP",         2343, TRUE,  FALSE, FALSE, 38},
    {"SCI_PARADOWN",             2413, TRUE,  FALSE, FALSE, 221},
    {"SCI_PARADOWNEXTEND",       2414, TRUE,  FALSE, TRUE,  221},
    {"SCI_PARAUP",               2415, TRUE,  FALSE, FALSE, 219},
    {"SCI_PARAUPEXTEND",         2416, TRUE,  FALSE, TRUE,  219},
    {"SCI_CHARLEFT",             2304, FALSE, FALSE, FALSE, 37},
    {"SCI_CHARLEFTEXTEND",       2305, FALSE, FALSE, TRUE,  37},
    {"SCI_CHARRIGHT",            2306, FALSE, FALSE, FALSE, 39},
    {"SCI_CHARRIGHTEXTEND",      2307, FALSE, FALSE, TRUE,  39},
    {"SCI_WORDLEFT",             2308, TRUE,  FALSE, FALSE, 37},
    {"SCI_WORDLEFTEXTEND",       2309, TRUE,  FALSE, TRUE,  37},
    {"SCI_WORDRIGHT",            2310, TRUE,  FALSE, FALSE, 39},
    {"SCI_WORDRIGHTEXTEND",      2311, TRUE,  FALSE, TRUE,  39},
    {"SCI_WORDPARTLEFT",         2390, TRUE,  FALSE, FALSE, 191},
    {"SCI_WORDPARTLEFTEXTEND",   2391, TRUE,  FALSE, TRUE,  191},
    {"SCI_WORDPARTRIGHT",        2392, TRUE,  FALSE, FALSE, 220},
    {"SCI_WORDPARTRIGHTEXTEND",  2393, TRUE,  FALSE, TRUE,  220},
    {"SCI_HOME",                 2312, FALSE, FALSE, FALSE, 36},
    {"SCI_HOMEEXTEND",           2313, FALSE, FALSE, TRUE,  36},
    {"SCI_VCHOME",               2331, FALSE, FALSE, FALSE,  0},
    {"SCI_VCHOMEEXTEND",         2332, FALSE, FALSE, FALSE,  0},
    {"SCI_LINEEND",              2314, FALSE, FALSE, FALSE, 35},
    {"SCI_LINEENDEXTEND",        2315, FALSE, FALSE, TRUE,  35},
    {"SCI_DOCUMENTSTART",        2316, TRUE,  FALSE, FALSE, 36},
    {"SCI_DOCUMENTSTARTEXTEND",  2317, TRUE,  FALSE, TRUE,  36},
    {"SCI_DOCUMENTEND",          2318, TRUE,  FALSE, FALSE, 35},
    {"SCI_DOCUMENTENDEXTEND",    2319, TRUE,  FALSE, TRUE,  35},
    {"SCI_PAGEUP",               2320, FALSE, FALSE, FALSE, 33},
    {"SCI_PAGEUPEXTEND",         2321, FALSE, FALSE, TRUE,  33},
    {"SCI_PAGEDOWN",             2322, FALSE, FALSE, FALSE, 34},
    {"SCI_PAGEDOWNEXTEND",       2323, FALSE, FALSE, TRUE,  34},
    {"SCI_DELETEBACK",           2326, FALSE, FALSE, FALSE,  8},
    {"SCI_DELWORDLEFT",          2335, TRUE,  FALSE, FALSE,  8},
    {"SCI_DELWORDRIGHT",         2336, TRUE,  FALSE, FALSE, 46},
    {"SCI_DELLINELEFT",          2395, TRUE,  FALSE, TRUE,   8},
    {"SCI_DELLINERIGHT",         2396, TRUE,  FALSE, TRUE,  46},
    {"SCI_LINEDELETE",           2338, TRUE,  FALSE, TRUE,  'L'},
    {"SCI_LINECUT",              2337, TRUE,  FALSE, FALSE, 'L'},
    {"SCI_LINECOPY",             2455, TRUE,  FALSE, TRUE,  'X'},
    {"SCI_LINETRANSPOSE",        2339, TRUE,  FALSE, FALSE, 'T'},
    {"SCI_CUT",                  2177, TRUE,  FALSE, FALSE, 'X'},
    {"SCI_COPY",                 2178, TRUE,  FALSE, FALSE, 'C'},
    {"SCI_PASTE",                2179, TRUE,  FALSE, FALSE, 'V'},
    {"SCI_CANCEL",               2325, FALSE, FALSE, FALSE, 27},
};
static const size_t SCI_DEFS_N = sizeof(SCI_DEFS) / sizeof(SCI_DEFS[0]);

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Module state                                                           */
/* ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    GPtrArray *rows[TAB_COUNT];   /* of ShortcutRow* */

    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *search;
    GtkWidget *conflict_label;
    GtkWidget *btn_modify, *btn_delete, *btn_save, *btn_close;
    GtkWidget *views[TAB_COUNT];  /* GtkTreeView */
    GtkListStore *stores[TAB_COUNT];

    gboolean dirty;
} ShortcutMapper;

static ShortcutMapper *G = NULL;

enum {
    COL_NUM = 0,    /* row number (1-based)              */
    COL_NAME,       /* command name                      */
    COL_SHORTCUT,   /* "Ctrl+S" etc.                     */
    COL_CATEGORY,   /* category / plugin name            */
    COL_ROWPTR,     /* gpointer to ShortcutRow           */
    N_COLS
};

/* ═══════════════════════════════════════════════════════════════════════ */
/*  XML — load (~/<APP_CONFIG_DIR>/shortcuts.xml)                          */
/* ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    GPtrArray  *macros;
    GPtrArray  *runcmds;
    GPtrArray  *plugins;
    GPtrArray  *intcmds;   /* ShortcutRow with action_id only */
    GHashTable *sci_over;  /* int sciID → ShortcutRow* */
    /* Active element parser state for character handler */
    gchar      *current_cmd_name;
    GString    *current_cmd_text;
} XmlCtx;

static gboolean parse_yes(const char *v) {
    return v && (g_ascii_strcasecmp(v, "yes") == 0 ||
                 g_ascii_strcasecmp(v, "true") == 0);
}

static const char *attr_get(const gchar **names, const gchar **vals,
                            const char *key) {
    for (int i = 0; names[i]; i++)
        if (g_ascii_strcasecmp(names[i], key) == 0) return vals[i];
    return NULL;
}

static void fill_mods_from_attrs(ShortcutRow *r,
                                 const gchar **names, const gchar **vals) {
    r->has_ctrl  = parse_yes(attr_get(names, vals, "Ctrl"));
    r->has_alt   = parse_yes(attr_get(names, vals, "Alt"));
    r->has_shift = parse_yes(attr_get(names, vals, "Shift"));
    r->has_super = parse_yes(attr_get(names, vals, "Super")) ||
                   parse_yes(attr_get(names, vals, "Cmd"));
    const char *k = attr_get(names, vals, "Key");
    r->keycode = k ? (guint)atoi(k) : 0;
    /* Backward compat: if no Super/Cmd attribute, treat Ctrl-only as is. */
}

static void xml_start(GMarkupParseContext *ctx, const gchar *name,
                      const gchar **attr_names, const gchar **attr_vals,
                      gpointer ud, GError **err) {
    (void)ctx; (void)err;
    XmlCtx *x = (XmlCtx *)ud;

    if (g_strcmp0(name, "Shortcut") == 0) {
        ShortcutRow *r = g_new0(ShortcutRow, 1);
        const char *id = attr_get(attr_names, attr_vals, "id");
        r->action_id = g_strdup(id ? id : "");
        r->name = g_strdup(r->action_id);
        fill_mods_from_attrs(r, attr_names, attr_vals);
        g_ptr_array_add(x->intcmds, r);
        return;
    }
    if (g_strcmp0(name, "Macro") == 0) {
        ShortcutRow *r = g_new0(ShortcutRow, 1);
        const char *nm = attr_get(attr_names, attr_vals, "name");
        r->name = g_strdup(nm ? nm : "");
        fill_mods_from_attrs(r, attr_names, attr_vals);
        g_ptr_array_add(x->macros, r);
        return;
    }
    if (g_strcmp0(name, "Command") == 0) {
        ShortcutRow *r = g_new0(ShortcutRow, 1);
        const char *nm = attr_get(attr_names, attr_vals, "name");
        r->name = g_strdup(nm ? nm : "");
        fill_mods_from_attrs(r, attr_names, attr_vals);
        g_ptr_array_add(x->runcmds, r);
        g_free(x->current_cmd_name);
        x->current_cmd_name = g_strdup(r->name);
        if (!x->current_cmd_text) x->current_cmd_text = g_string_new(NULL);
        else g_string_truncate(x->current_cmd_text, 0);
        return;
    }
    if (g_strcmp0(name, "PluginCommand") == 0) {
        ShortcutRow *r = g_new0(ShortcutRow, 1);
        const char *mod = attr_get(attr_names, attr_vals, "moduleName");
        const char *iid = attr_get(attr_names, attr_vals, "internalID");
        r->plugin_name = g_strdup(mod ? mod : "");
        r->category    = g_strdup(mod ? mod : "");
        r->command_id  = iid ? atoi(iid) : 0;
        r->name = g_strdup_printf("%s[%d]", r->plugin_name, r->command_id);
        fill_mods_from_attrs(r, attr_names, attr_vals);
        g_ptr_array_add(x->plugins, r);
        return;
    }
    if (g_strcmp0(name, "ScintKey") == 0) {
        const char *sid = attr_get(attr_names, attr_vals, "ScintID");
        if (!sid) return;
        ShortcutRow *r = g_new0(ShortcutRow, 1);
        r->command_id = atoi(sid);
        fill_mods_from_attrs(r, attr_names, attr_vals);
        g_hash_table_insert(x->sci_over,
                            GINT_TO_POINTER(r->command_id), r);
        return;
    }
}

static void xml_text(GMarkupParseContext *ctx, const gchar *text,
                     gsize text_len, gpointer ud, GError **err) {
    (void)ctx; (void)err;
    XmlCtx *x = (XmlCtx *)ud;
    if (x->current_cmd_name && x->current_cmd_text)
        g_string_append_len(x->current_cmd_text, text, (gssize)text_len);
}

static void xml_end(GMarkupParseContext *ctx, const gchar *name,
                    gpointer ud, GError **err) {
    (void)ctx; (void)err;
    XmlCtx *x = (XmlCtx *)ud;
    if (g_strcmp0(name, "Command") == 0) {
        /* Stash command body in the category slot (we don't display it,
         * but we must preserve it when saving). */
        if (x->current_cmd_name && x->current_cmd_text && x->runcmds->len) {
            ShortcutRow *r = (ShortcutRow *)
                g_ptr_array_index(x->runcmds, x->runcmds->len - 1);
            r->category = g_strstrip(g_strdup(x->current_cmd_text->str));
        }
        g_free(x->current_cmd_name); x->current_cmd_name = NULL;
        if (x->current_cmd_text) g_string_truncate(x->current_cmd_text, 0);
    }
}

static gchar *shortcuts_xml_path(void) {
    return npp_user_file(NULL, "shortcuts.xml");
}

static void load_shortcuts_xml(XmlCtx *x) {
    gchar *path = shortcuts_xml_path();
    gchar *xml  = NULL;
    gsize  len  = 0;
    if (!g_file_get_contents(path, &xml, &len, NULL)) {
        g_free(path);
        return;
    }
    GMarkupParser p = { xml_start, xml_end, xml_text, NULL, NULL };
    GMarkupParseContext *ctx = g_markup_parse_context_new(&p, 0, x, NULL);
    g_markup_parse_context_parse(ctx, xml, (gssize)len, NULL);
    g_markup_parse_context_end_parse(ctx, NULL);
    g_markup_parse_context_free(ctx);
    g_free(xml);
    g_free(path);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Loaders for each tab                                                   */
/* ═══════════════════════════════════════════════════════════════════════ */

static gchar *humanize_action(const char *action_name) {
    /* "open-folder-as-workspace" → "Open Folder As Workspace" */
    GString *s = g_string_new(NULL);
    gboolean upper_next = TRUE;
    for (const char *p = action_name; *p; p++) {
        if (*p == '-' || *p == '_') {
            g_string_append_c(s, ' ');
            upper_next = TRUE;
        } else if (upper_next) {
            g_string_append_c(s, g_ascii_toupper(*p));
            upper_next = FALSE;
        } else {
            g_string_append_c(s, *p);
        }
    }
    return g_string_free(s, FALSE);
}

/* Categorize an action name into a coarse main-menu group. */
static const char *category_for_action(const char *a) {
    if (!a) return "Other";
    if (g_str_has_prefix(a, "new") || g_str_has_prefix(a, "open") ||
        g_str_has_prefix(a, "save") || g_str_has_prefix(a, "close") ||
        g_str_has_prefix(a, "print") || g_str_has_prefix(a, "reload") ||
        g_str_has_prefix(a, "rename") || g_str_has_prefix(a, "move-to-trash") ||
        g_str_has_prefix(a, "quit") || g_str_has_prefix(a, "load-session") ||
        g_str_has_prefix(a, "save-session") || g_str_has_prefix(a, "reopen") ||
        g_str_has_prefix(a, "clear-recent"))
        return "File";
    if (g_str_has_prefix(a, "undo") || g_str_has_prefix(a, "redo") ||
        g_str_has_prefix(a, "cut") || g_str_has_prefix(a, "copy") ||
        g_str_has_prefix(a, "paste") || g_str_has_prefix(a, "select") ||
        g_str_has_prefix(a, "duplicate") || g_str_has_prefix(a, "delete-line") ||
        g_str_has_prefix(a, "move-line") || g_str_has_prefix(a, "comment") ||
        g_str_has_prefix(a, "insert") || g_str_has_prefix(a, "toggle-case") ||
        g_str_has_prefix(a, "upper") || g_str_has_prefix(a, "lower") ||
        g_str_has_prefix(a, "trim"))
        return "Edit";
    if (g_str_has_prefix(a, "find") || g_str_has_prefix(a, "replace") ||
        g_str_has_prefix(a, "goto") || g_str_has_prefix(a, "bookmark") ||
        g_str_has_prefix(a, "brace") || g_str_has_prefix(a, "mark"))
        return "Search";
    if (g_str_has_prefix(a, "zoom") || g_str_has_prefix(a, "fullscreen") ||
        g_str_has_prefix(a, "toggle-") || g_str_has_prefix(a, "show-") ||
        g_str_has_prefix(a, "hide-") || g_str_has_prefix(a, "word-wrap") ||
        g_str_has_prefix(a, "split-"))
        return "View";
    if (g_str_has_prefix(a, "encoding") || g_str_has_prefix(a, "to-utf") ||
        g_str_has_prefix(a, "convert"))
        return "Encoding";
    if (g_str_has_prefix(a, "lang-") || g_str_has_prefix(a, "set-lang"))
        return "Language";
    if (g_str_has_prefix(a, "pref") || g_str_has_prefix(a, "settings") ||
        g_str_has_prefix(a, "shortcut") || g_str_has_prefix(a, "style"))
        return "Settings";
    if (g_str_has_prefix(a, "macro") || g_str_has_prefix(a, "run") ||
        g_str_has_prefix(a, "play") || g_str_has_prefix(a, "record"))
        return "Run";
    if (g_str_has_prefix(a, "plugin")) return "Plugins";
    if (g_str_has_prefix(a, "tab-") || g_str_has_prefix(a, "next-tab") ||
        g_str_has_prefix(a, "prev-tab") || g_str_has_prefix(a, "switch-"))
        return "Window";
    if (g_str_has_prefix(a, "about") || g_str_has_prefix(a, "help") ||
        g_str_has_prefix(a, "doc"))
        return "Help";
    return "Other";
}

/* Apply an InternalCommands override (read from XML) on top of a freshly
 * built Main-menu row. */
static void apply_override_to_main(ShortcutRow *r, GPtrArray *overrides) {
    for (guint i = 0; i < overrides->len; i++) {
        ShortcutRow *o = (ShortcutRow *)g_ptr_array_index(overrides, i);
        if (g_strcmp0(o->action_id, r->action_id) == 0) {
            r->has_ctrl  = o->has_ctrl;
            r->has_alt   = o->has_alt;
            r->has_shift = o->has_shift;
            r->has_super = o->has_super;
            r->keycode   = o->keycode;
            r->modified  = TRUE;
            return;
        }
    }
}

static void load_main_menu(ShortcutMapper *m, GPtrArray *overrides) {
    GApplication *app = g_application_get_default();
    if (!app) return;
    if (!G_IS_ACTION_GROUP(app)) return;

    gchar **names = g_action_group_list_actions(G_ACTION_GROUP(app));
    /* Sort alphabetically for stable display order. */
    if (names) {
        guint n = g_strv_length(names);
        for (guint i = 0; i + 1 < n; i++)
            for (guint j = i + 1; j < n; j++)
                if (g_strcmp0(names[i], names[j]) > 0) {
                    gchar *t = names[i]; names[i] = names[j]; names[j] = t;
                }
    }

    for (int i = 0; names && names[i]; i++) {
        /* Skip parametric actions (open-recent etc.) — only no-argument
         * actions appear in the mapper. */
        const GVariantType *pt =
            g_action_group_get_action_parameter_type(G_ACTION_GROUP(app),
                                                     names[i]);
        if (pt) continue;

        ShortcutRow *r = g_new0(ShortcutRow, 1);
        r->action_id = g_strdup_printf("app.%s", names[i]);
        r->name      = humanize_action(names[i]);
        r->category  = g_strdup(category_for_action(names[i]));

        /* Look up the current accelerator for this action. */
        gchar **accels = gtk_application_get_accels_for_action(
            GTK_APPLICATION(app), r->action_id);
        if (accels && accels[0] && *accels[0]) {
            guint kv = 0;
            GdkModifierType mods = 0;
            gtk_accelerator_parse(accels[0], &kv, &mods);
            r->keycode   = vk_from_gdk(kv);
            r->has_ctrl  = (mods & GDK_CONTROL_MASK) != 0;
            r->has_alt   = (mods & GDK_ALT_MASK)    != 0;
            r->has_shift = (mods & GDK_SHIFT_MASK)   != 0;
            r->has_super = (mods & GDK_SUPER_MASK)   != 0;
        }
        if (accels) g_strfreev(accels);

        /* Overlay user XML override if present. */
        apply_override_to_main(r, overrides);

        g_ptr_array_add(m->rows[TAB_MAIN], r);
    }
    if (names) g_strfreev(names);
}

static void load_scintilla(ShortcutMapper *m, GHashTable *sci_over) {
    for (size_t i = 0; i < SCI_DEFS_N; i++) {
        const SciKeyDef *d = &SCI_DEFS[i];
        ShortcutRow *r = g_new0(ShortcutRow, 1);
        r->name       = g_strdup(d->name);
        r->command_id = d->sciID;
        ShortcutRow *o = sci_over ?
            (ShortcutRow *)g_hash_table_lookup(sci_over,
                                               GINT_TO_POINTER(d->sciID))
            : NULL;
        if (o) {
            r->has_ctrl  = o->has_ctrl;
            r->has_alt   = o->has_alt;
            r->has_shift = o->has_shift;
            r->has_super = o->has_super;
            r->keycode   = o->keycode;
            r->modified  = TRUE;
        } else {
            r->has_ctrl  = d->ctrl;
            r->has_alt   = d->alt;
            r->has_shift = d->shift;
            r->keycode   = d->key;
        }
        g_ptr_array_add(m->rows[TAB_SCI], r);
    }
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Conflict detection                                                     */
/* ═══════════════════════════════════════════════════════════════════════ */

static gboolean row_eq_combo(const ShortcutRow *a, const ShortcutRow *b) {
    return a->keycode != 0 && b->keycode != 0 &&
           a->keycode == b->keycode &&
           a->has_ctrl  == b->has_ctrl  &&
           a->has_alt   == b->has_alt   &&
           a->has_shift == b->has_shift &&
           a->has_super == b->has_super;
}

/* Returns a newly-allocated descriptor of conflicts (or NULL if none). */
static gchar *find_conflict(const ShortcutRow *target, const ShortcutRow *exclude) {
    if (!target || target->keycode == 0) return NULL;
    static const char *TAB_NAMES[TAB_COUNT] = {
        "Main menu", "Macros", "Run commands",
        "Plugin commands", "Scintilla commands",
    };
    GString *s = g_string_new(NULL);
    for (int t = 0; t < TAB_COUNT; t++) {
        if (!G || !G->rows[t]) continue;
        for (guint i = 0; i < G->rows[t]->len; i++) {
            ShortcutRow *r = (ShortcutRow *)g_ptr_array_index(G->rows[t], i);
            if (r == target || r == exclude) continue;
            if (!row_eq_combo(target, r)) continue;
            gchar *dl = shortcut_display(r);
            if (s->len) g_string_append(s, "\n");
            g_string_append_printf(s, "%s | %u  %s  (%s)",
                                   TAB_NAMES[t], i + 1,
                                   r->name ? r->name : "?", dl);
            g_free(dl);
        }
    }
    if (s->len == 0) { g_string_free(s, TRUE); return NULL; }
    return g_string_free(s, FALSE);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Store / View / filter                                                  */
/* ═══════════════════════════════════════════════════════════════════════ */

static void store_set_row(GtkListStore *store, GtkTreeIter *it,
                          int rownum, const ShortcutRow *r) {
    gchar *sc = shortcut_display(r);
    gtk_list_store_set(store, it,
        COL_NUM,      rownum,
        COL_NAME,     r->name ? r->name : "",
        COL_SHORTCUT, sc,
        COL_CATEGORY, r->category ? r->category :
                      (r->plugin_name ? r->plugin_name : ""),
        COL_ROWPTR,   (gpointer)r,
        -1);
    g_free(sc);
}

static gboolean filter_matches(const ShortcutRow *r, const char *needle) {
    if (!needle || !*needle) return TRUE;
    gchar *n = g_utf8_casefold(needle, -1);
    gboolean hit = FALSE;
    const char *fields[] = { r->name, r->category, r->plugin_name, r->action_id, NULL };
    for (int i = 0; fields[i] && !hit; i++) {
        gchar *f = g_utf8_casefold(fields[i], -1);
        hit = (strstr(f, n) != NULL);
        g_free(f);
    }
    if (!hit) {
        gchar *sc = shortcut_display(r);
        gchar *f  = g_utf8_casefold(sc, -1);
        hit = (strstr(f, n) != NULL);
        g_free(f);
        g_free(sc);
    }
    g_free(n);
    return hit;
}

static void refresh_tab(SmTab t) {
    GtkListStore *store = G->stores[t];
    if (!store) return;
    gtk_list_store_clear(store);
    const char *needle = gtk_entry_get_text(GTK_ENTRY(G->search));
    GPtrArray *rows = G->rows[t];
    int rownum = 0;
    for (guint i = 0; i < rows->len; i++) {
        ShortcutRow *r = (ShortcutRow *)g_ptr_array_index(rows, i);
        rownum++;
        if (!filter_matches(r, needle)) continue;
        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        store_set_row(store, &it, rownum, r);
    }
}

static void refresh_all_tabs(void) {
    for (int t = 0; t < TAB_COUNT; t++) refresh_tab((SmTab)t);
}

static SmTab current_tab(void) {
    return (SmTab)gtk_notebook_get_current_page(GTK_NOTEBOOK(G->notebook));
}

static ShortcutRow *selected_row(SmTab t) {
    GtkTreeView *tv = GTK_TREE_VIEW(G->views[t]);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    GtkTreeIter it;
    GtkTreeModel *m = NULL;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return NULL;
    gpointer p = NULL;
    gtk_tree_model_get(m, &it, COL_ROWPTR, &p, -1);
    return (ShortcutRow *)p;
}

static void update_conflict_label_for(const ShortcutRow *r) {
    if (!r || r->keycode == 0) {
        gtk_label_set_markup(GTK_LABEL(G->conflict_label),
            "<span size='small'>No shortcut conflicts for this item.</span>");
        return;
    }
    gchar *c = find_conflict(r, NULL);
    if (!c) {
        gtk_label_set_markup(GTK_LABEL(G->conflict_label),
            "<span size='small'>No shortcut conflicts for this item.</span>");
    } else {
        gchar *esc = g_markup_escape_text(c, -1);
        gchar *markup = g_strdup_printf(
            "<span foreground='#c33' size='small'>CONFLICT: %s</span>", esc);
        gtk_label_set_markup(GTK_LABEL(G->conflict_label), markup);
        g_free(markup); g_free(esc); g_free(c);
    }
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Modify dialog                                                          */
/* ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    GtkWidget   *chk_ctrl, *chk_alt, *chk_shift, *chk_super;
    GtkWidget   *combo_key;
    GtkWidget   *conflict;
    GtkWidget   *btn_ok;
    ShortcutRow *row;
} ModifyCtx;

static gint combo_index_for_vk(guint vk) {
    const char *name = popup_name_for_vk(vk);
    for (int i = 0; KEY_POPUP[i]; i++)
        if (strcmp(KEY_POPUP[i], name) == 0) return i;
    return 0;
}

static void modify_recheck(GtkWidget *w, gpointer ud) {
    (void)w;
    ModifyCtx *c = (ModifyCtx *)ud;

    /* Build a probe row from the dialog state */
    ShortcutRow probe = {0};
    probe.has_ctrl  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c->chk_ctrl));
    probe.has_alt   = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c->chk_alt));
    probe.has_shift = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c->chk_shift));
    probe.has_super = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c->chk_super));
    const gchar *kn =
        gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c->combo_key));
    probe.keycode = vk_from_popup_name(kn);

    if (probe.keycode == 0) {
        gtk_label_set_markup(GTK_LABEL(c->conflict), "");
        gtk_widget_set_sensitive(c->btn_ok, TRUE);
        return;
    }
    gchar *cf = find_conflict(&probe, c->row);
    if (!cf) {
        gtk_label_set_markup(GTK_LABEL(c->conflict),
            "<span size='small'>No conflict.</span>");
        gtk_widget_set_sensitive(c->btn_ok, TRUE);
    } else {
        gchar *esc = g_markup_escape_text(cf, -1);
        gchar *m = g_strdup_printf(
            "<span foreground='#c33' size='small'>CONFLICT: %s</span>", esc);
        gtk_label_set_markup(GTK_LABEL(c->conflict), m);
        gtk_widget_set_sensitive(c->btn_ok, FALSE);
        g_free(m); g_free(esc); g_free(cf);
    }
}

static gboolean run_modify_dialog(GtkWindow *parent, ShortcutRow *row) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Shortcut", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK",     GTK_RESPONSE_OK,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 400, 220);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_set_spacing(GTK_BOX(content), 6);
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    /* Header: command name */
    GtkWidget *name_lbl = gtk_label_new(NULL);
    gchar *nm_markup = g_markup_printf_escaped(
        "<b>%s</b>", row->name ? row->name : "");
    gtk_label_set_markup(GTK_LABEL(name_lbl), nm_markup);
    g_free(nm_markup);
    gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
    npp_box_pack(GTK_BOX(content), name_lbl, FALSE, 2);

    /* 2×2 modifier grid */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);

    GtkWidget *chk_ctrl  = gtk_check_button_new_with_label("Ctrl");
    GtkWidget *chk_alt   = gtk_check_button_new_with_label("Alt");
    GtkWidget *chk_shift = gtk_check_button_new_with_label("Shift");
    GtkWidget *chk_super = gtk_check_button_new_with_label("Super");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_ctrl),  row->has_ctrl);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_alt),   row->has_alt);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_shift), row->has_shift);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_super), row->has_super);
    gtk_grid_attach(GTK_GRID(grid), chk_ctrl,  0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), chk_alt,   1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), chk_shift, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), chk_super, 1, 1, 1, 1);
    npp_box_pack(GTK_BOX(content), grid, FALSE, 4);

    /* Key dropdown */
    GtkWidget *key_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    npp_box_pack(GTK_BOX(key_row), gtk_label_new("Key:"), FALSE, 0);
    GtkWidget *combo = gtk_combo_box_text_new();
    for (int i = 0; KEY_POPUP[i]; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), KEY_POPUP[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), combo_index_for_vk(row->keycode));
    npp_box_pack(GTK_BOX(key_row), combo, FALSE, 0);
    npp_box_pack(GTK_BOX(content), key_row, FALSE, 4);

    /* Conflict label (live) */
    GtkWidget *conflict = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(conflict), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(conflict), TRUE);
    npp_box_pack(GTK_BOX(content), conflict, TRUE, 4);

    /* Wire live re-check */
    GtkWidget *btn_ok = gtk_dialog_get_widget_for_response(
        GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    ModifyCtx ctx = {
        .chk_ctrl = chk_ctrl, .chk_alt = chk_alt,
        .chk_shift = chk_shift, .chk_super = chk_super,
        .combo_key = combo, .conflict = conflict,
        .btn_ok = btn_ok, .row = row,
    };
    g_signal_connect(chk_ctrl,  "toggled", G_CALLBACK(modify_recheck), &ctx);
    g_signal_connect(chk_alt,   "toggled", G_CALLBACK(modify_recheck), &ctx);
    g_signal_connect(chk_shift, "toggled", G_CALLBACK(modify_recheck), &ctx);
    g_signal_connect(chk_super, "toggled", G_CALLBACK(modify_recheck), &ctx);
    g_signal_connect(combo,     "changed", G_CALLBACK(modify_recheck), &ctx);

    modify_recheck(NULL, &ctx);
    gtk_widget_show_all(dlg);

    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gboolean ok = (resp == GTK_RESPONSE_OK);
    if (ok) {
        row->has_ctrl  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_ctrl));
        row->has_alt   = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_alt));
        row->has_shift = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_shift));
        row->has_super = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_super));
        const gchar *kn =
            gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
        row->keycode = vk_from_popup_name(kn);
        row->modified = TRUE;
        G->dirty = TRUE;
    }
    gtk_widget_destroy(dlg);
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Save → XML + live accel push                                           */
/* ═══════════════════════════════════════════════════════════════════════ */

static gchar *gtk_accel_string_for_row(const ShortcutRow *r) {
    if (r->keycode == 0) return NULL;
    guint kv = gdk_from_vk(r->keycode);
    GdkModifierType m = 0;
    if (r->has_ctrl)  m |= GDK_CONTROL_MASK;
    if (r->has_alt)   m |= GDK_ALT_MASK;
    if (r->has_shift) m |= GDK_SHIFT_MASK;
    if (r->has_super) m |= GDK_SUPER_MASK;
    return gtk_accelerator_name(kv, m);
}

static void push_live_accels(void) {
    GApplication *gapp = g_application_get_default();
    if (!GTK_IS_APPLICATION(gapp)) return;
    GtkApplication *app = GTK_APPLICATION(gapp);

    GPtrArray *rows = G->rows[TAB_MAIN];
    for (guint i = 0; i < rows->len; i++) {
        ShortcutRow *r = (ShortcutRow *)g_ptr_array_index(rows, i);
        if (!r->action_id || !*r->action_id) continue;
        gchar *acc = gtk_accel_string_for_row(r);
        const gchar *accels[2] = { acc ? acc : NULL, NULL };
        gtk_application_set_accels_for_action(app, r->action_id,
            acc ? accels : (const gchar *[]){ NULL });
        g_free(acc);
    }
}

static void xml_append_shortcut_attrs(GString *out, const ShortcutRow *r) {
    g_string_append_printf(out,
        " Ctrl=\"%s\" Alt=\"%s\" Shift=\"%s\" Super=\"%s\" Key=\"%u\"",
        r->has_ctrl  ? "yes" : "no",
        r->has_alt   ? "yes" : "no",
        r->has_shift ? "yes" : "no",
        r->has_super ? "yes" : "no",
        r->keycode);
}

static void save_shortcuts_xml(void) {
    gchar *dir = npp_user_dir();
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    gchar *path = shortcuts_xml_path();

    GString *out = g_string_new(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
        "<NotepadPlus>\n");

    /* InternalCommands — only entries that were modified (matches macOS) */
    g_string_append(out, "    <InternalCommands>\n");
    for (guint i = 0; i < G->rows[TAB_MAIN]->len; i++) {
        ShortcutRow *r =
            (ShortcutRow *)g_ptr_array_index(G->rows[TAB_MAIN], i);
        if (!r->modified) continue;
        gchar *eid = g_markup_escape_text(r->action_id ? r->action_id : "", -1);
        g_string_append_printf(out, "        <Shortcut id=\"%s\"", eid);
        xml_append_shortcut_attrs(out, r);
        g_string_append(out, " />\n");
        g_free(eid);
    }
    g_string_append(out, "    </InternalCommands>\n");

    /* Macros — macro.c owns the section (GAP-19): push the mapper's
     * (possibly edited) shortcut attributes back into the model, then let
     * it emit the full <Macro> entries WITH their <Action> bodies. The
     * old writer emitted empty <Macro/> elements here, silently destroying
     * every recorded macro body on Save. */
    for (guint i = 0; i < G->rows[TAB_MACROS]->len; i++) {
        ShortcutRow *r =
            (ShortcutRow *)g_ptr_array_index(G->rows[TAB_MACROS], i);
        if (r->modified)
            macro_named_set_shortcut(r->command_id, r->has_ctrl, r->has_alt,
                                     r->has_shift, r->has_super, r->keycode);
    }
    macro_emit_macros_section(out);

    /* UserDefinedCommands (Run commands) — preserve body text in r->category */
    g_string_append(out, "    <UserDefinedCommands>\n");
    for (guint i = 0; i < G->rows[TAB_RUN]->len; i++) {
        ShortcutRow *r =
            (ShortcutRow *)g_ptr_array_index(G->rows[TAB_RUN], i);
        gchar *en = g_markup_escape_text(r->name ? r->name : "", -1);
        gchar *eb = g_markup_escape_text(r->category ? r->category : "", -1);
        g_string_append_printf(out, "        <Command name=\"%s\"", en);
        xml_append_shortcut_attrs(out, r);
        g_string_append_printf(out, ">%s</Command>\n", eb);
        g_free(en); g_free(eb);
    }
    g_string_append(out, "    </UserDefinedCommands>\n");

    /* PluginCommands */
    g_string_append(out, "    <PluginCommands>\n");
    for (guint i = 0; i < G->rows[TAB_PLUGIN]->len; i++) {
        ShortcutRow *r =
            (ShortcutRow *)g_ptr_array_index(G->rows[TAB_PLUGIN], i);
        if (!r->modified) continue;
        gchar *em = g_markup_escape_text(r->plugin_name ? r->plugin_name : "", -1);
        g_string_append_printf(out,
            "        <PluginCommand moduleName=\"%s\" internalID=\"%d\"",
            em, r->command_id);
        xml_append_shortcut_attrs(out, r);
        g_string_append(out, " />\n");
        g_free(em);
    }
    g_string_append(out, "    </PluginCommands>\n");

    /* ScintillaKeys */
    g_string_append(out, "    <ScintillaKeys>\n");
    for (guint i = 0; i < G->rows[TAB_SCI]->len; i++) {
        ShortcutRow *r =
            (ShortcutRow *)g_ptr_array_index(G->rows[TAB_SCI], i);
        if (!r->modified) continue;
        g_string_append_printf(out,
            "        <ScintKey ScintID=\"%d\" menuCmdID=\"0\"",
            r->command_id);
        xml_append_shortcut_attrs(out, r);
        g_string_append(out, " />\n");
    }
    g_string_append(out, "    </ScintillaKeys>\n");

    g_string_append(out, "</NotepadPlus>\n");

    g_file_set_contents(path, out->str, (gssize)out->len, NULL);
    g_string_free(out, TRUE);
    g_free(path);
}

static void do_save(void) {
    save_shortcuts_xml();
    push_live_accels();
    macro_push_accels();
    G->dirty = FALSE;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Toolbar actions                                                        */
/* ═══════════════════════════════════════════════════════════════════════ */

static void on_modify_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    SmTab t = current_tab();
    ShortcutRow *r = selected_row(t);
    if (!r) return;
    if (run_modify_dialog(GTK_WINDOW(G->window), r)) {
        refresh_tab(t);
        update_conflict_label_for(r);
    }
}

static void sync_macro_rows(void);

static void on_delete_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    SmTab t = current_tab();
    if (t != TAB_MACROS && t != TAB_RUN) return;
    ShortcutRow *r = selected_row(t);
    if (!r) return;

    GtkWidget *q = gtk_message_dialog_new(GTK_WINDOW(G->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "Delete \"%s\"?", r->name ? r->name : "");
    gint resp = gtk_dialog_run(GTK_DIALOG(q));
    gtk_widget_destroy(q);
    if (resp != GTK_RESPONSE_OK) return;

    if (t == TAB_MACROS) {
        /* macro.c owns macros — delete there, persist, resync rows
         * (indices shift down after a removal). */
        macro_named_delete(r->command_id);
        macro_save_to_shortcuts_xml();
        sync_macro_rows();
    } else {
        g_ptr_array_remove(G->rows[t], r);
        shortcut_row_free(r);
        G->dirty = TRUE;
    }
    refresh_tab(t);
    update_conflict_label_for(NULL);
}

static void on_save_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    do_save();
}

static void on_close_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (G->dirty) do_save();
    gtk_widget_hide(G->window);
}

static void on_search_changed(GtkSearchEntry *e, gpointer ud) {
    (void)e; (void)ud;
    refresh_all_tabs();
}

static void on_tab_changed(GtkNotebook *nb, GtkWidget *page, guint pn, gpointer ud) {
    (void)nb; (void)page; (void)ud;
    gtk_widget_set_sensitive(G->btn_delete,
        pn == TAB_MACROS || pn == TAB_RUN);
    update_conflict_label_for(NULL);
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *p,
                             GtkTreeViewColumn *c, gpointer ud) {
    (void)tv; (void)p; (void)c; (void)ud;
    on_modify_clicked(NULL, NULL);
}

static void on_selection_changed(GtkTreeSelection *sel, gpointer ud) {
    (void)sel; (void)ud;
    SmTab t = current_tab();
    update_conflict_label_for(selected_row(t));
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Tree-view construction                                                 */
/* ═══════════════════════════════════════════════════════════════════════ */

static GtkWidget *build_tab_view(SmTab t, gboolean show_category) {
    GtkListStore *store = gtk_list_store_new(N_COLS,
        G_TYPE_INT,    /* num */
        G_TYPE_STRING, /* name */
        G_TYPE_STRING, /* shortcut */
        G_TYPE_STRING, /* category */
        G_TYPE_POINTER /* row ptr */
    );
    G->stores[t] = store;

    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    GtkCellRenderer *tr;
    GtkTreeViewColumn *col;

    tr = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes(
        "#", tr, "text", COL_NUM, NULL);
    gtk_tree_view_column_set_min_width(col, 40);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);

    tr = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes(
        "Name", tr, "text", COL_NAME, NULL);
    gtk_tree_view_column_set_min_width(col, 280);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);

    tr = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes(
        "Shortcut", tr, "text", COL_SHORTCUT, NULL);
    gtk_tree_view_column_set_min_width(col, 160);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);

    if (show_category) {
        tr = gtk_cell_renderer_text_new();
        col = gtk_tree_view_column_new_with_attributes(
            t == TAB_PLUGIN ? "Plugin" : "Category", tr,
            "text", COL_CATEGORY, NULL);
        gtk_tree_view_column_set_min_width(col, 130);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }

    g_signal_connect(tv, "row-activated",
                     G_CALLBACK(on_row_activated), NULL);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tv));
    g_signal_connect(sel, "changed",
                     G_CALLBACK(on_selection_changed), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);

    G->views[t] = tv;
    return scroll;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Build & show                                                           */
/* ═══════════════════════════════════════════════════════════════════════ */

static void free_arrays(void) {
    for (int t = 0; t < TAB_COUNT; t++) {
        if (G->rows[t]) {
            g_ptr_array_free(G->rows[t], TRUE);
            G->rows[t] = NULL;
        }
    }
}

/* TAB_MACROS rows are a view onto macro.c's model (GAP-19): name +
 * binding, with command_id = macro index. Rebuilt on every mapper open
 * and after deletions so external saves stay in sync. */
static void sync_macro_rows(void) {
    if (!G || !G->rows[TAB_MACROS]) return;
    g_ptr_array_set_size(G->rows[TAB_MACROS], 0);
    int n = macro_named_count();
    for (int i = 0; i < n; i++) {
        const NamedMacro *nm = macro_named_get(i);
        ShortcutRow *r = g_new0(ShortcutRow, 1);
        r->name       = g_strdup(nm->name);
        r->has_ctrl   = nm->ctrl;
        r->has_alt    = nm->alt;
        r->has_shift  = nm->shift;
        r->has_super  = nm->super_mod;
        r->keycode    = nm->key;
        r->command_id = i;
        g_ptr_array_add(G->rows[TAB_MACROS], r);
    }
}

static void load_all_data(void) {
    for (int t = 0; t < TAB_COUNT; t++)
        G->rows[t] = g_ptr_array_new_with_free_func(shortcut_row_free);

    XmlCtx x = {0};
    x.macros  = g_ptr_array_new_with_free_func(shortcut_row_free);
    x.runcmds = g_ptr_array_new_with_free_func(shortcut_row_free);
    x.plugins = g_ptr_array_new_with_free_func(shortcut_row_free);
    x.intcmds = g_ptr_array_new_with_free_func(shortcut_row_free);
    x.sci_over = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                       NULL, shortcut_row_free);
    load_shortcuts_xml(&x);

    /* Macros come from macro.c, not from our own XML pass (x.macros is
     * parsed but discarded — macro.c is the single owner of that section). */
    sync_macro_rows();

    for (guint i = 0; i < x.runcmds->len; i++)
        g_ptr_array_add(G->rows[TAB_RUN], g_ptr_array_index(x.runcmds, i));
    g_ptr_array_set_free_func(x.runcmds, NULL);

    for (guint i = 0; i < x.plugins->len; i++)
        g_ptr_array_add(G->rows[TAB_PLUGIN], g_ptr_array_index(x.plugins, i));
    g_ptr_array_set_free_func(x.plugins, NULL);

    load_main_menu(G, x.intcmds);
    load_scintilla(G, x.sci_over);

    g_ptr_array_free(x.macros,  TRUE);
    g_ptr_array_free(x.runcmds, TRUE);
    g_ptr_array_free(x.plugins, TRUE);
    g_ptr_array_free(x.intcmds, TRUE);
    g_hash_table_destroy(x.sci_over);
    if (x.current_cmd_text) g_string_free(x.current_cmd_text, TRUE);
    g_free(x.current_cmd_name);
}

static gboolean on_window_delete(GtkWindow *w, gpointer ud) {
    (void)ud;
    if (G->dirty) do_save();
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;   /* swallow — just hide */
}

static void build_ui(GtkWidget *parent) {
    G->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(G->window), "Shortcut Mapper");
    gtk_window_set_default_size(GTK_WINDOW(G->window), 860, 620);
    if (parent && GTK_IS_WINDOW(parent))
        gtk_window_set_transient_for(GTK_WINDOW(G->window), GTK_WINDOW(parent));
    g_signal_connect(G->window, "close-request",
                     G_CALLBACK(on_window_delete), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(root), 8);
    gtk_container_add(GTK_CONTAINER(G->window), root);

    /* Filter search bar */
    G->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(G->search), "Filter");
    g_signal_connect(G->search, "search-changed",
                     G_CALLBACK(on_search_changed), NULL);
    npp_box_pack(GTK_BOX(root), G->search, FALSE, 0);

    /* Notebook */
    G->notebook = gtk_notebook_new();
    g_signal_connect(G->notebook, "switch-page",
                     G_CALLBACK(on_tab_changed), NULL);
    npp_box_pack(GTK_BOX(root), G->notebook, TRUE, 0);

    static const struct { SmTab t; const char *title; gboolean cat; } TABS[] = {
        { TAB_MAIN,    "Main menu",         TRUE  },
        { TAB_MACROS,  "Macros",            FALSE },
        { TAB_RUN,     "Run commands",      FALSE },
        { TAB_PLUGIN,  "Plugin commands",   TRUE  },
        { TAB_SCI,     "Scintilla commands",FALSE },
    };
    for (size_t i = 0; i < sizeof(TABS)/sizeof(TABS[0]); i++) {
        GtkWidget *page = build_tab_view(TABS[i].t, TABS[i].cat);
        gtk_notebook_append_page(GTK_NOTEBOOK(G->notebook),
                                 page, gtk_label_new(TABS[i].title));
    }

    /* Conflict label */
    G->conflict_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(G->conflict_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(G->conflict_label), TRUE);
    gtk_label_set_markup(GTK_LABEL(G->conflict_label),
        "<span size='small'>No shortcut conflicts for this item.</span>");
    npp_box_pack(GTK_BOX(root), G->conflict_label, FALSE, 0);

    /* Toolbar */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(bar, GTK_ALIGN_END);
    G->btn_modify = gtk_button_new_with_label("Modify…");
    G->btn_delete = gtk_button_new_with_label("Delete");
    G->btn_save   = gtk_button_new_with_label("Save");
    G->btn_close  = gtk_button_new_with_label("Close");
    g_signal_connect(G->btn_modify, "clicked",
                     G_CALLBACK(on_modify_clicked), NULL);
    g_signal_connect(G->btn_delete, "clicked",
                     G_CALLBACK(on_delete_clicked), NULL);
    g_signal_connect(G->btn_save,   "clicked",
                     G_CALLBACK(on_save_clicked),   NULL);
    g_signal_connect(G->btn_close,  "clicked",
                     G_CALLBACK(on_close_clicked),  NULL);
    npp_box_pack(GTK_BOX(bar), G->btn_modify, FALSE, 0);
    npp_box_pack(GTK_BOX(bar), G->btn_delete, FALSE, 0);
    npp_box_pack(GTK_BOX(bar), G->btn_save, FALSE, 0);
    npp_box_pack(GTK_BOX(bar), G->btn_close, FALSE, 0);
    npp_box_pack(GTK_BOX(root), bar, FALSE, 0);

    /* Delete is only active on Macros/Run tabs */
    gtk_widget_set_sensitive(G->btn_delete, FALSE);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Public entry point                                                     */
/* ═══════════════════════════════════════════════════════════════════════ */

void shortcut_mapper_show(GtkWidget *parent) {
    if (G && G->window) {
        if (parent && GTK_IS_WINDOW(parent))
            gtk_window_set_transient_for(GTK_WINDOW(G->window),
                                         GTK_WINDOW(parent));
        /* Macros may have been saved/deleted since the last open. */
        sync_macro_rows();
        refresh_tab(TAB_MACROS);
        gtk_widget_show_all(G->window);
        gtk_window_present(GTK_WINDOW(G->window));
        return;
    }
    if (!G) G = g_new0(ShortcutMapper, 1);

    load_all_data();
    build_ui(parent);
    refresh_all_tabs();
    gtk_widget_show_all(G->window);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Legacy API stubs (kept for header ABI compatibility)                   */
/* ═══════════════════════════════════════════════════════════════════════ */

static ShortcutEntry s_legacy[1];

ShortcutEntry *shortcut_table(int *count) {
    if (count) *count = 0;
    return s_legacy;
}

ShortcutEntry *shortcut_find(const char *id) { (void)id; return NULL; }

void shortcut_register(const char *id, GtkWidget *w, gpointer g) {
    (void)id; (void)w; (void)g;
}

void shortcut_load(void) { /* mapper loads lazily on first open */ }

void shortcut_save(void) {
    if (G) do_save();
}

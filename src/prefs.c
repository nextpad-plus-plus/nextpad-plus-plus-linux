#include "prefs.h"
#include "gtk_compat.h"
#include "branding.h"
#include "backup.h"
#include "encoding.h"
#include "sci_messages.h"
#include "i18n.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* EOL enum translation                                                */
/*                                                                     */
/* Scintilla:      SC_EOL_CRLF=0, SC_EOL_CR=1, SC_EOL_LF=2             */
/* macOS XML:      0=CRLF, 1=LF, 2=CR (kPrefEOLType)                   */
/*                                                                     */
/* `g_prefs.default_eol` is held internally in Scintilla form so it    */
/* can be passed unchanged to SCI_SETEOLMODE; the helpers below        */
/* translate at the config.xml I/O boundary so the file is             */
/* byte-identical to what macOS writes.                                */
/* ------------------------------------------------------------------ */
static int mac_eol_to_sc(int mac) {
    switch (mac) {
        case 0: return SC_EOL_CRLF;
        case 1: return SC_EOL_LF;
        case 2: return SC_EOL_CR;
        default: return SC_EOL_LF;   /* macOS default */
    }
}
static int sc_eol_to_mac(int sc) {
    switch (sc) {
        case SC_EOL_CRLF: return 0;
        case SC_EOL_LF:   return 1;
        case SC_EOL_CR:   return 2;
        default: return 1;
    }
}

/* P10 — persisted workspace roots loaded from config.xml's <Workspace>
 * group. Public via prefs_workspace_roots() so main.c can apply them
 * after the workspace widget is constructed. */
static GPtrArray *s_workspace_roots = NULL;

const char *const *prefs_workspace_roots(int *out_n) {
    if (!s_workspace_roots) { if (out_n) *out_n = 0; return NULL; }
    if (out_n) *out_n = (int)s_workspace_roots->len;
    return (const char *const *)s_workspace_roots->pdata;
}

void prefs_workspace_roots_set(const char *const *paths, int n) {
    if (s_workspace_roots) {
        for (guint i = 0; i < s_workspace_roots->len; i++)
            g_free(s_workspace_roots->pdata[i]);
        g_ptr_array_set_size(s_workspace_roots, 0);
    } else {
        s_workspace_roots = g_ptr_array_new();
    }
    for (int i = 0; i < n; i++)
        if (paths[i]) g_ptr_array_add(s_workspace_roots, g_strdup(paths[i]));
}

/* P14 — per-language indentation overrides. lang → TabOverride*. */
static GHashTable *s_tab_overrides = NULL;
static void ensure_tab_overrides(void) {
    if (!s_tab_overrides)
        s_tab_overrides = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);
}
const TabOverride *prefs_tab_override_for(const char *lang) {
    if (!s_tab_overrides || !lang) return NULL;
    return (const TabOverride *)g_hash_table_lookup(s_tab_overrides, lang);
}
void prefs_tab_override_set(const char *lang, int tab_size, gboolean use_tabs) {
    if (!lang) return;
    ensure_tab_overrides();
    TabOverride *o = g_new(TabOverride, 1);
    o->tab_size = tab_size;
    o->use_tabs = use_tabs;
    g_hash_table_insert(s_tab_overrides, g_strdup(lang), o);
}
void prefs_tab_override_clear(const char *lang) {
    if (!s_tab_overrides || !lang) return;
    g_hash_table_remove(s_tab_overrides, lang);
}
GList *prefs_tab_overrides_keys(void) {
    if (!s_tab_overrides) return NULL;
    return g_hash_table_get_keys(s_tab_overrides);
}

/* ------------------------------------------------------------------ */
/* Global prefs instance — initialised to defaults                    */
/* ------------------------------------------------------------------ */

NppPrefs g_prefs = {
    /* Indentation */
    .tab_width               = 4,
    .use_tabs                = TRUE,    /* Q-align: macOS default */
    .auto_indent             = AUTO_INDENT_ADVANCED,  /* macOS default — kPrefAutoIndent=@1 */
    .backspace_unindent      = FALSE,
    /* Editor */
    .highlight_current_line  = TRUE,
    .caret_width             = 1,
    .caret_blink_rate        = 500,     /* Q-align: macOS default */
    .scroll_beyond_last_line = FALSE,
    .word_wrap               = FALSE,
    .auto_close_brackets     = FALSE,
    .smart_highlight         = TRUE,
    .smart_hilite_case       = FALSE,
    .smart_hilite_word       = FALSE,   /* Q-align: macOS default */
    .virtual_space           = FALSE,
    .disable_text_drag_drop  = FALSE,
    .right_click_keeps_sel   = FALSE,   /* Q-align: macOS default */
    .font_quality            = 3,       /* Q-align: macOS LCD rendering */
    .date_time_reverse       = FALSE,
    .save_all_confirm        = TRUE,
    .mute_sounds             = FALSE,
    /* Margins */
    .show_line_numbers       = TRUE,
    .line_num_dyn_width      = TRUE,
    .show_bookmark_margin    = TRUE,
    .show_whitespace         = FALSE,
    .show_eol                = FALSE,
    .edge_mode               = EDGE_NONE,
    .edge_column             = 80,
    .fold_style              = FOLD_BOX_TREE,
    .padding_left            = 4,
    .padding_right           = 4,
    /* New Document */
    .default_eol             = 2,         /* SC_EOL_LF */
    .default_encoding        = "UTF-8",
    .default_language        = "",
    /* General */
    .show_full_path_in_title = FALSE,
    .show_status_bar         = TRUE,
    .copy_line_no_selection  = TRUE,
    .toolbar_icon_scale      = 3,    /* 100 % — matches macOS default */
    .remember_session        = TRUE,
    .keep_absent_session     = FALSE,
    .panel_keep_state        = TRUE,
    .appearance              = APPEAR_AUTO,
    .theme_preset            = "Default",
    /* Auto-completion */
    .autocomplete_enabled    = TRUE,
    .autocomplete_min_chars  = 1,
    .func_params_hint        = FALSE,   /* Q-align: macOS default */
    .spell_check             = FALSE,
    /* Searching */
    .fill_find_with_selection= TRUE,
    .mono_font_find          = FALSE,   /* Q-align: macOS default */
    .confirm_replace_all     = TRUE,
    .replace_and_stop        = FALSE,
    .in_sel_threshold        = 1024,
    .search_engine_url       = "https://duckduckgo.com/?q=%s",
    /* Delimiter */
    .delim_open              = "(",
    .delim_close             = ")",
    .delim_entire_doc        = FALSE,
    .word_chars              = "",
    .word_chars_use_default  = TRUE,
    /* Performance */
    .large_file_enabled            = TRUE,
    .large_file_size_mb            = 200,
    .large_file_suppress           = FALSE,
    .large_file_no_wrap            = TRUE,
    .large_file_allow_autocomplete = FALSE,
    .large_file_allow_smart_hilite = FALSE,
    .large_file_allow_brace_match  = FALSE,
    .large_file_allow_url_click    = FALSE,   /* Q-align: macOS default */
    /* Tab bar */
    .tab_close_button        = TRUE,
    .double_click_tab_close  = FALSE,
    .tab_bar_wrap            = FALSE,
    .tab_max_label_width     = 200,
    /* Backup */
    .backup_enabled          = TRUE,
    .backup_interval_secs    = 60,
};

/* ------------------------------------------------------------------ */
/* config.xml — macOS-compatible <GUIConfig> schema.                  */
/*                                                                    */
/* Matches notepad-plus-plus-macos/src/MainWindowController.mm        */
/* writeConfigXML()/readConfigXML() exactly so configs round-trip     */
/* between platforms.                                                 */
/* ------------------------------------------------------------------ */

#include "paths.h"

static inline gboolean is_yes(const char *v) {
    return v && (g_ascii_strcasecmp(v, "yes") == 0 ||
                 g_ascii_strcasecmp(v, "true") == 0 ||
                 strcmp(v, "1") == 0);
}
static inline gboolean is_show(const char *v) {
    return v && (g_ascii_strcasecmp(v, "show") == 0 ||
                 g_ascii_strcasecmp(v, "yes")  == 0 ||
                 strcmp(v, "1") == 0);
}
static inline const char *b2yn(gboolean b) { return b ? "yes" : "no"; }
static inline const char *b2sh(gboolean b) { return b ? "show" : "hide"; }

/* Encoding int <-> display string mapping (matches macOS port). */
static int encoding_str_to_int(const char *s) {
    if (!s || !*s) return 0;
    if (g_ascii_strcasecmp(s, "UTF-8") == 0) return 0;
    if (g_ascii_strcasecmp(s, "ISO-8859-1") == 0 || g_ascii_strcasecmp(s, "Latin-1") == 0) return 1;
    return 0;
}
static const char *encoding_int_to_str(int n) {
    return (n == 1) ? "ISO-8859-1" : "UTF-8";
}

/* Fold-style names matching Windows NPP. */
static int fold_str_to_int(const char *s) {
    if (!s) return 0;
    if (!g_ascii_strcasecmp(s, "circle")) return 1;
    if (!g_ascii_strcasecmp(s, "arrow"))  return 2;
    if (!g_ascii_strcasecmp(s, "simple")) return 3;
    if (!g_ascii_strcasecmp(s, "none"))   return 4;
    return 0; /* "box" / default */
}
static const char *fold_int_to_str(int n) {
    static const char *names[] = { "box", "circle", "arrow", "simple", "none" };
    return (n >= 0 && n < 5) ? names[n] : "box";
}

/* ── XML parser state ─────────────────────────────────────────────── */

typedef struct {
    char     group[64];   /* name="..." of currently-open <GUIConfig> */
    GString *text;        /* accumulated text content of the element */
} ParseState;

static void apply_attr(const char *group, const char *attr, const char *val)
{
    /* Bail on null/empty input. */
    if (!group || !attr || !val) return;

    if (!strcmp(group, "TabBar")) {
        if      (!strcmp(attr, "closeButton"))        g_prefs.tab_close_button       = is_yes(val);
        else if (!strcmp(attr, "doubleClick2Close"))  g_prefs.double_click_tab_close = is_yes(val);
        else if (!strcmp(attr, "multiLine"))          g_prefs.tab_bar_wrap           = is_yes(val);
        else if (!strcmp(attr, "tabCompactLabelLen")) g_prefs.tab_max_label_width    = atoi(val);
    }
    else if (!strcmp(group, "TabSetting")) {
        if      (!strcmp(attr, "replaceBySpace"))     g_prefs.use_tabs               = !is_yes(val);
        else if (!strcmp(attr, "size"))               g_prefs.tab_width              = atoi(val);
    }
    else if (!strcmp(group, "Caret")) {
        if      (!strcmp(attr, "width"))              g_prefs.caret_width            = atoi(val);
        else if (!strcmp(attr, "blinkRate"))          g_prefs.caret_blink_rate       = atoi(val);
    }
    else if (!strcmp(group, "titleBar")) {
        /* macOS stores `short="yes"` when path is NOT full. Invert. */
        if (!strcmp(attr, "short"))                   g_prefs.show_full_path_in_title = !is_yes(val);
    }
    else if (!strcmp(group, "insertDateTime")) {
        if (!strcmp(attr, "reverseDefaultOrder"))     g_prefs.date_time_reverse      = is_yes(val);
    }
    else if (!strcmp(group, "auto-completion")) {
        if      (!strcmp(attr, "autoCAction"))        g_prefs.autocomplete_enabled   = (atoi(val) != 0);
        else if (!strcmp(attr, "triggerFromNbChar"))  g_prefs.autocomplete_min_chars = atoi(val);
        else if (!strcmp(attr, "funcParams"))         g_prefs.func_params_hint       = is_yes(val);
    }
    else if (!strcmp(group, "auto-insert")) {
        /* macOS stores 5 separate booleans; we collapse to one toggle.
         * Treat ANY of them being "yes" as enabling auto-close. */
        if (is_yes(val)) g_prefs.auto_close_brackets = TRUE;
    }
    else if (!strcmp(group, "SmartHighLight")) {
        if      (!strcmp(attr, "matchCase"))          g_prefs.smart_hilite_case      = is_yes(val);
        else if (!strcmp(attr, "wholeWordOnly"))      g_prefs.smart_hilite_word      = is_yes(val);
    }
    else if (!strcmp(group, "Searching")) {
        if      (!strcmp(attr, "monospacedFontFindDlg"))         g_prefs.mono_font_find         = is_yes(val);
        else if (!strcmp(attr, "fillFindFieldWithSelected"))     g_prefs.fill_find_with_selection = is_yes(val);
        else if (!strcmp(attr, "confirmReplaceInAllOpenDocs"))   g_prefs.confirm_replace_all    = is_yes(val);
        else if (!strcmp(attr, "replaceStopsWithoutFindingNext"))g_prefs.replace_and_stop       = is_yes(val);
        else if (!strcmp(attr, "inSelectionAutocheckThreshold")) g_prefs.in_sel_threshold       = atoi(val);
    }
    /* DarkMode is handled in a single-pass block in xml_start to avoid
     * attribute-order dependency — see below. Only theme="…" is per-attr. */
    else if (!strcmp(group, "DarkMode")) {
        if (!strcmp(attr, "theme")) {
            strncpy(g_prefs.theme_preset, val, sizeof(g_prefs.theme_preset) - 1);
            g_prefs.theme_preset[sizeof(g_prefs.theme_preset) - 1] = '\0';
        }
    }
    else if (!strcmp(group, "Localization")) {
        if (!strcmp(attr, "language")) {
            strncpy(g_prefs.ui_language, val, sizeof(g_prefs.ui_language) - 1);
            g_prefs.ui_language[sizeof(g_prefs.ui_language) - 1] = '\0';
        }
    }
    else if (!strcmp(group, "MISC")) {
        if      (!strcmp(attr, "muteSounds"))         g_prefs.mute_sounds            = is_yes(val);
        else if (!strcmp(attr, "disableTextDragDrop"))g_prefs.disable_text_drag_drop = is_yes(val);
        else if (!strcmp(attr, "spellCheck"))         g_prefs.spell_check            = is_yes(val);
        else if (!strcmp(attr, "panelKeepState"))     g_prefs.panel_keep_state       = is_yes(val);
    }
    else if (!strcmp(group, "NewDocDefaultSettings")) {
        if      (!strcmp(attr, "format"))             g_prefs.default_eol            = mac_eol_to_sc(atoi(val));
        else if (!strcmp(attr, "encoding")) {
            const char *enc = encoding_int_to_str(atoi(val));
            strncpy(g_prefs.default_encoding, enc, sizeof(g_prefs.default_encoding) - 1);
            g_prefs.default_encoding[sizeof(g_prefs.default_encoding) - 1] = '\0';
        }
    }
    else if (!strcmp(group, "Backup")) {
        if      (!strcmp(attr, "action"))             g_prefs.backup_enabled         = (atoi(val) != 0);
        else if (!strcmp(attr, "snapshotBackupTiming"))g_prefs.backup_interval_secs  = atoi(val) / 1000;
    }
    else if (!strcmp(group, "ScintillaPrimaryView")) {
        if      (!strcmp(attr, "lineNumberMargin"))   g_prefs.show_line_numbers      = is_show(val);
        else if (!strcmp(attr, "lineNumberDynamicWidth")) g_prefs.line_num_dyn_width = is_yes(val);
        else if (!strcmp(attr, "bookMarkMargin"))     g_prefs.show_bookmark_margin   = is_show(val);
        else if (!strcmp(attr, "folderMarkStyle"))    g_prefs.fold_style             = fold_str_to_int(val);
        else if (!strcmp(attr, "virtualSpace"))       g_prefs.virtual_space          = is_yes(val);
        else if (!strcmp(attr, "scrollBeyondLastLine"))g_prefs.scroll_beyond_last_line = is_yes(val);
        else if (!strcmp(attr, "rightClickKeepsSelection")) g_prefs.right_click_keeps_sel = is_yes(val);
        else if (!strcmp(attr, "lineCopyCutWithoutSelection")) g_prefs.copy_line_no_selection = is_yes(val);
        else if (!strcmp(attr, "currentLineIndicator"))g_prefs.highlight_current_line= (atoi(val) != 0);
        else if (!strcmp(attr, "whiteSpaceShow"))     g_prefs.show_whitespace        = is_show(val);
        else if (!strcmp(attr, "eolShow"))            g_prefs.show_eol               = is_show(val);
        else if (!strcmp(attr, "eolMode"))            g_prefs.default_eol            = mac_eol_to_sc(atoi(val));
        else if (!strcmp(attr, "zoom"))               g_prefs.zoom_level             = atoi(val);
        else if (!strcmp(attr, "smoothFont"))         g_prefs.font_quality           = atoi(val);
        else if (!strcmp(attr, "paddingLeft"))        g_prefs.padding_left           = atoi(val);
        else if (!strcmp(attr, "paddingRight"))       g_prefs.padding_right          = atoi(val);
        else if (!strcmp(attr, "edgeMultiColumnPos")) g_prefs.edge_column            = (*val) ? atoi(val) : 0;
        else if (!strcmp(attr, "isEdgeBgMode")) {
            /* Final edge_mode decided in finalise step — store bg flag for now. */
            if (is_yes(val)) g_prefs.edge_mode = EDGE_BACKGROUND;
        }
        /* Linux-only attr (macOS doesn't write it). Lets us preserve the
         * OFF/LINE distinction that macOS loses in its save format. */
        else if (!strcmp(attr, "edgeMode"))           g_prefs.edge_mode              = atoi(val);
    }
    /* Linux-port additions — no equivalent in macOS schema today. */
    else if (!strcmp(group, "DelimiterSelection")) {
        if      (!strcmp(attr, "openSymbol")) {
            strncpy(g_prefs.delim_open, val, sizeof(g_prefs.delim_open) - 1);
            g_prefs.delim_open[sizeof(g_prefs.delim_open) - 1] = '\0';
        }
        else if (!strcmp(attr, "closeSymbol")) {
            strncpy(g_prefs.delim_close, val, sizeof(g_prefs.delim_close) - 1);
            g_prefs.delim_close[sizeof(g_prefs.delim_close) - 1] = '\0';
        }
        else if (!strcmp(attr, "entireDoc"))          g_prefs.delim_entire_doc       = is_yes(val);
    }
    else if (!strcmp(group, "WordCharsList")) {
        if      (!strcmp(attr, "useDefault"))         g_prefs.word_chars_use_default = is_yes(val);
        else if (!strcmp(attr, "addedChars")) {
            strncpy(g_prefs.word_chars, val, sizeof(g_prefs.word_chars) - 1);
            g_prefs.word_chars[sizeof(g_prefs.word_chars) - 1] = '\0';
        }
    }
    else if (!strcmp(group, "LargeFileRestriction")) {
        if      (!strcmp(attr, "enabled"))            g_prefs.large_file_enabled     = is_yes(val);
        else if (!strcmp(attr, "fileSizeMB"))         g_prefs.large_file_size_mb     = atoi(val);
        else if (!strcmp(attr, "suppress2GB"))        g_prefs.large_file_suppress    = is_yes(val);
        else if (!strcmp(attr, "deactivateWrap"))     g_prefs.large_file_no_wrap     = is_yes(val);
        else if (!strcmp(attr, "allowAutoComplete"))  g_prefs.large_file_allow_autocomplete = is_yes(val);
        else if (!strcmp(attr, "allowSmartHilite"))   g_prefs.large_file_allow_smart_hilite = is_yes(val);
        else if (!strcmp(attr, "allowBraceMatch"))    g_prefs.large_file_allow_brace_match  = is_yes(val);
        else if (!strcmp(attr, "allowURLClick"))      g_prefs.large_file_allow_url_click    = is_yes(val);
    }
    else if (!strcmp(group, "NppToolbar")) {
        if      (!strcmp(attr, "iconSetSize"))        g_prefs.toolbar_icon_scale     = atoi(val);
    }
    else if (!strcmp(group, "SearchEngine")) {
        if      (!strcmp(attr, "url")) {
            strncpy(g_prefs.search_engine_url, val, sizeof(g_prefs.search_engine_url) - 1);
            g_prefs.search_engine_url[sizeof(g_prefs.search_engine_url) - 1] = '\0';
        }
    }
}

static void apply_text(const char *group, const char *text) {
    if (!group || !text) return;
    /* trimmed copy */
    gchar *t = g_strstrip(g_strdup(text));
    if (!*t) { g_free(t); return; }

    if      (!strcmp(group, "StatusBar"))                    g_prefs.show_status_bar    = is_show(t);
    else if (!strcmp(group, "MaintainIndent"))               g_prefs.auto_indent        = atoi(t);
    else if (!strcmp(group, "BackspaceUnindent"))            g_prefs.backspace_unindent = is_yes(t);
    else if (!strcmp(group, "RememberLastSession"))          g_prefs.remember_session   = is_yes(t);
    else if (!strcmp(group, "KeepSessionAbsentFileEntries")) g_prefs.keep_absent_session= is_yes(t);
    else if (!strcmp(group, "SaveAllConfirm"))               g_prefs.save_all_confirm   = is_yes(t);
    else if (!strcmp(group, "SmartHighLight"))               g_prefs.smart_highlight    = is_yes(t);
    else if (!strcmp(group, "NewDocDefaultLanguage")) {
        strncpy(g_prefs.default_language, t, sizeof(g_prefs.default_language) - 1);
        g_prefs.default_language[sizeof(g_prefs.default_language) - 1] = '\0';
    }
    g_free(t);
}

static void xml_start(GMarkupParseContext *ctx, const gchar *el,
                      const gchar **names, const gchar **vals,
                      gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    ParseState *st = ud;
    /* P10 — Workspace group contains nested <Root path="..."/>. */
    if (st->group[0] && strcmp(st->group, "Workspace") == 0 &&
        strcmp(el, "Root") == 0) {
        if (!s_workspace_roots) s_workspace_roots = g_ptr_array_new();
        for (int i = 0; names[i]; i++) {
            if (!strcmp(names[i], "path")) {
                g_ptr_array_add(s_workspace_roots, g_strdup(vals[i]));
                break;
            }
        }
        return;
    }
    /* P14 — TabCustom contains nested <Language name= tabSize= useTabs=>. */
    if (st->group[0] && strcmp(st->group, "TabCustom") == 0 &&
        strcmp(el, "Language") == 0) {
        const char *lang = NULL;
        int   tab_size  = 4;
        int   use_tabs  = 0;
        for (int i = 0; names[i]; i++) {
            if      (!strcmp(names[i], "name"))    lang     = vals[i];
            else if (!strcmp(names[i], "tabSize")) tab_size = atoi(vals[i]);
            else if (!strcmp(names[i], "useTabs")) use_tabs = is_yes(vals[i]);
        }
        if (lang) prefs_tab_override_set(lang, tab_size, use_tabs);
        return;
    }
    if (strcmp(el, "GUIConfig") != 0) { st->group[0] = '\0'; return; }

    const char *name = NULL;
    for (int i = 0; names[i]; i++) {
        if (!strcmp(names[i], "name")) { name = vals[i]; break; }
    }
    if (!name) return;
    g_strlcpy(st->group, name, sizeof(st->group));
    g_string_truncate(st->text, 0);

    /* DarkMode needs a 2-pass over attrs since enable + darkModeAuto
     * together determine appearance and they can arrive in any order. */
    if (!strcmp(name, "DarkMode")) {
        int enable_yes = -1, auto_yes = -1;
        for (int i = 0; names[i]; i++) {
            if      (!strcmp(names[i], "enable"))       enable_yes = is_yes(vals[i]) ? 1 : 0;
            else if (!strcmp(names[i], "darkModeAuto")) auto_yes   = is_yes(vals[i]) ? 1 : 0;
        }
        if      (auto_yes   == 1) g_prefs.appearance = APPEAR_AUTO;
        else if (enable_yes == 1) g_prefs.appearance = APPEAR_DARK;
        else if (enable_yes == 0) g_prefs.appearance = APPEAR_LIGHT;
    }

    /* Apply each non-`name` attribute immediately (generic path). */
    for (int i = 0; names[i]; i++) {
        if (strcmp(names[i], "name") != 0)
            apply_attr(st->group, names[i], vals[i]);
    }
}

static void xml_text(GMarkupParseContext *ctx, const gchar *txt, gsize len,
                     gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    ParseState *st = ud;
    if (!st->group[0]) return;
    g_string_append_len(st->text, txt, (gssize)len);
}

static void xml_end(GMarkupParseContext *ctx, const gchar *el,
                    gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    ParseState *st = ud;
    if (strcmp(el, "GUIConfig") != 0) return;
    if (st->group[0] && st->text->len > 0)
        apply_text(st->group, st->text->str);
    st->group[0] = '\0';
    g_string_truncate(st->text, 0);
}

static GMarkupParser s_parser = { xml_start, xml_end, xml_text, NULL, NULL };

void prefs_load(void)
{
    /* Reset edge_mode each load; the attr handler can flip it to BACKGROUND. */
    g_prefs.edge_mode = EDGE_NONE;
    /* P10 — clear any prior workspace-root list before parsing. */
    if (s_workspace_roots) {
        for (guint i = 0; i < s_workspace_roots->len; i++)
            g_free(s_workspace_roots->pdata[i]);
        g_ptr_array_set_size(s_workspace_roots, 0);
    }

    gchar *path = npp_user_file(NULL, "config.xml");
    gchar *xml  = NULL;
    if (g_file_get_contents(path, &xml, NULL, NULL)) {
        ParseState st = { .text = g_string_new(NULL) };
        GMarkupParseContext *ctx = g_markup_parse_context_new(&s_parser, 0, &st, NULL);
        g_markup_parse_context_parse(ctx, xml, -1, NULL);
        g_markup_parse_context_free(ctx);
        g_string_free(st.text, TRUE);
        g_free(xml);
    }
    g_free(path);

    /* edge_mode resolution: attr handler may have flipped to BACKGROUND.
     * If we got a non-empty edgeMultiColumnPos and aren't in BACKGROUND,
     * promote to LINE. If column ended up empty, force OFF. */
    if (g_prefs.edge_column <= 0)
        g_prefs.edge_mode = EDGE_NONE;
    else if (g_prefs.edge_mode == EDGE_NONE)
        ; /* leave as configured (default OFF on a fresh load) */
    else if (g_prefs.edge_mode != EDGE_BACKGROUND)
        g_prefs.edge_mode = EDGE_LINE;

    if (g_prefs.default_encoding[0] == '\0')
        strncpy(g_prefs.default_encoding, "UTF-8", sizeof(g_prefs.default_encoding) - 1);
    if (g_prefs.search_engine_url[0] == '\0')
        strncpy(g_prefs.search_engine_url, "https://duckduckgo.com/?q=%s", sizeof(g_prefs.search_engine_url) - 1);
    if (g_prefs.theme_preset[0] == '\0')
        strncpy(g_prefs.theme_preset, "Default", sizeof(g_prefs.theme_preset) - 1);
}

/* ------------------------------------------------------------------ */
/* XML save — matches macOS writeConfigXML() byte-for-byte where      */
/* the prefs overlap. Linux-specific extensions are appended as       */
/* additional GUIConfig groups (DelimiterSelection, WordCharsList,    */
/* LargeFileRestriction, NppToolbar, SearchEngine, NewDocDefaultLanguage). */
/* ------------------------------------------------------------------ */

void prefs_save(void)
{
    gchar *dir = npp_user_dir();
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    gchar *path = npp_user_file(NULL, "config.xml");

    GString *b = g_string_new(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
        "<NotepadPlus>\n"
        "    <GUIConfigs>\n");

    /* StatusBar */
    g_string_append_printf(b, "        <GUIConfig name=\"StatusBar\">%s</GUIConfig>\n",
        b2sh(g_prefs.show_status_bar));

    /* TabBar */
    g_string_append_printf(b,
        "        <GUIConfig name=\"TabBar\" closeButton=\"%s\" doubleClick2Close=\"%s\" "
        "reduce=\"yes\" dragAndDrop=\"yes\" drawTopBar=\"yes\" drawInactiveTab=\"yes\" "
        "pinButton=\"yes\" multiLine=\"%s\" tabCompactLabelLen=\"%d\" />\n",
        b2yn(g_prefs.tab_close_button), b2yn(g_prefs.double_click_tab_close),
        b2yn(g_prefs.tab_bar_wrap), g_prefs.tab_max_label_width);

    /* TabSetting */
    g_string_append_printf(b,
        "        <GUIConfig name=\"TabSetting\" replaceBySpace=\"%s\" size=\"%d\" />\n",
        b2yn(!g_prefs.use_tabs), g_prefs.tab_width);

    /* P14 — TabCustom: nested <Language> per override, alphabetised. */
    if (s_tab_overrides && g_hash_table_size(s_tab_overrides) > 0) {
        GList *keys = g_hash_table_get_keys(s_tab_overrides);
        keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
        g_string_append(b, "        <GUIConfig name=\"TabCustom\">\n");
        for (GList *l = keys; l; l = l->next) {
            const char *lang = (const char *)l->data;
            const TabOverride *o = prefs_tab_override_for(lang);
            if (!o) continue;
            gchar *esc = g_markup_escape_text(lang, -1);
            g_string_append_printf(b,
                "            <Language name=\"%s\" tabSize=\"%d\" useTabs=\"%s\" />\n",
                esc, o->tab_size, b2yn(o->use_tabs));
            g_free(esc);
        }
        g_string_append(b, "        </GUIConfig>\n");
        g_list_free(keys);
    }

    /* MaintainIndent / BackspaceUnindent / Session toggles */
    g_string_append_printf(b, "        <GUIConfig name=\"MaintainIndent\">%d</GUIConfig>\n", g_prefs.auto_indent);
    g_string_append_printf(b, "        <GUIConfig name=\"BackspaceUnindent\">%s</GUIConfig>\n", b2yn(g_prefs.backspace_unindent));
    g_string_append_printf(b, "        <GUIConfig name=\"RememberLastSession\">%s</GUIConfig>\n", b2yn(g_prefs.remember_session));
    g_string_append_printf(b, "        <GUIConfig name=\"KeepSessionAbsentFileEntries\">%s</GUIConfig>\n", b2yn(g_prefs.keep_absent_session));
    g_string_append_printf(b, "        <GUIConfig name=\"SaveAllConfirm\">%s</GUIConfig>\n", b2yn(g_prefs.save_all_confirm));

    /* NewDocDefaultSettings — `format` uses the macOS kPrefEOLType
     * convention (0=CRLF, 1=LF, 2=CR), not SC_EOL_*. Translate. */
    g_string_append_printf(b,
        "        <GUIConfig name=\"NewDocDefaultSettings\" format=\"%d\" encoding=\"%d\" openAnsiAsUTF8=\"yes\" />\n",
        sc_eol_to_mac(g_prefs.default_eol),
        encoding_str_to_int(g_prefs.default_encoding));

    /* Backup */
    g_string_append_printf(b,
        "        <GUIConfig name=\"Backup\" action=\"%s\" isSnapshotMode=\"%s\" snapshotBackupTiming=\"%d\" />\n",
        g_prefs.backup_enabled ? "2" : "0", b2yn(g_prefs.backup_enabled),
        g_prefs.backup_interval_secs * 1000);

    /* Caret */
    g_string_append_printf(b,
        "        <GUIConfig name=\"Caret\" width=\"%d\" blinkRate=\"%d\" />\n",
        g_prefs.caret_width, g_prefs.caret_blink_rate);

    /* titleBar */
    g_string_append_printf(b, "        <GUIConfig name=\"titleBar\" short=\"%s\" />\n",
        b2yn(!g_prefs.show_full_path_in_title));

    /* insertDateTime */
    g_string_append_printf(b, "        <GUIConfig name=\"insertDateTime\" reverseDefaultOrder=\"%s\" />\n",
        b2yn(g_prefs.date_time_reverse));

    /* auto-completion */
    g_string_append_printf(b,
        "        <GUIConfig name=\"auto-completion\" autoCAction=\"%s\" triggerFromNbChar=\"%d\" funcParams=\"%s\" />\n",
        g_prefs.autocomplete_enabled ? "3" : "0", g_prefs.autocomplete_min_chars,
        b2yn(g_prefs.func_params_hint));

    /* auto-insert */
    const char *acb = b2yn(g_prefs.auto_close_brackets);
    g_string_append_printf(b,
        "        <GUIConfig name=\"auto-insert\" parentheses=\"%s\" brackets=\"%s\" curlyBrackets=\"%s\" quotes=\"%s\" doubleQuotes=\"%s\" />\n",
        acb, acb, acb, acb, acb);

    /* SmartHighLight */
    g_string_append_printf(b,
        "        <GUIConfig name=\"SmartHighLight\" matchCase=\"%s\" wholeWordOnly=\"%s\">%s</GUIConfig>\n",
        b2yn(g_prefs.smart_hilite_case), b2yn(g_prefs.smart_hilite_word), b2yn(g_prefs.smart_highlight));

    /* Searching */
    g_string_append_printf(b,
        "        <GUIConfig name=\"Searching\" monospacedFontFindDlg=\"%s\" "
        "fillFindFieldWithSelected=\"%s\" confirmReplaceInAllOpenDocs=\"%s\" "
        "replaceStopsWithoutFindingNext=\"%s\" inSelectionAutocheckThreshold=\"%d\" />\n",
        b2yn(g_prefs.mono_font_find), b2yn(g_prefs.fill_find_with_selection),
        b2yn(g_prefs.confirm_replace_all), b2yn(g_prefs.replace_and_stop),
        g_prefs.in_sel_threshold);

    /* DarkMode + theme preset. Omit theme attr when at the default to
     * keep our output byte-compatible with macOS for the common case. */
    if (g_prefs.theme_preset[0] && strcmp(g_prefs.theme_preset, "Default") != 0) {
        gchar *theme_esc = g_markup_escape_text(g_prefs.theme_preset, -1);
        g_string_append_printf(b,
            "        <GUIConfig name=\"DarkMode\" enable=\"%s\" darkModeAuto=\"%s\" theme=\"%s\" />\n",
            b2yn(g_prefs.appearance == APPEAR_DARK),
            b2yn(g_prefs.appearance == APPEAR_AUTO), theme_esc);
        g_free(theme_esc);
    } else {
        g_string_append_printf(b,
            "        <GUIConfig name=\"DarkMode\" enable=\"%s\" darkModeAuto=\"%s\" />\n",
            b2yn(g_prefs.appearance == APPEAR_DARK),
            b2yn(g_prefs.appearance == APPEAR_AUTO));
    }

    /* Localization — the UI language (empty = auto-detect from locale). */
    if (g_prefs.ui_language[0]) {
        gchar *lang_esc = g_markup_escape_text(g_prefs.ui_language, -1);
        g_string_append_printf(b,
            "        <GUIConfig name=\"Localization\" language=\"%s\" />\n",
            lang_esc);
        g_free(lang_esc);
    }

    /* MISC */
    g_string_append_printf(b,
        "        <GUIConfig name=\"MISC\" muteSounds=\"%s\" disableTextDragDrop=\"%s\" "
        "spellCheck=\"%s\" panelKeepState=\"%s\" funcListUseXML=\"yes\" />\n",
        b2yn(g_prefs.mute_sounds), b2yn(g_prefs.disable_text_drag_drop),
        b2yn(g_prefs.spell_check), b2yn(g_prefs.panel_keep_state));

    /* ScintillaPrimaryView */
    char edgeCol[32], edgeModeAttr[32];
    if (g_prefs.edge_mode != EDGE_NONE && g_prefs.edge_column > 0)
        g_snprintf(edgeCol, sizeof(edgeCol), "%d", g_prefs.edge_column);
    else
        edgeCol[0] = '\0';
    /* Linux-only edgeMode attr to disambiguate OFF vs LINE (macOS schema
     * loses this). Omit when OFF for byte-compat with macOS output. */
    if (g_prefs.edge_mode != EDGE_NONE)
        g_snprintf(edgeModeAttr, sizeof(edgeModeAttr), " edgeMode=\"%d\"", g_prefs.edge_mode);
    else
        edgeModeAttr[0] = '\0';
    g_string_append_printf(b,
        "        <GUIConfig name=\"ScintillaPrimaryView\" "
        "lineNumberMargin=\"%s\" lineNumberDynamicWidth=\"%s\" "
        "bookMarkMargin=\"%s\" folderMarkStyle=\"%s\" "
        "virtualSpace=\"%s\" scrollBeyondLastLine=\"%s\" "
        "rightClickKeepsSelection=\"%s\" lineCopyCutWithoutSelection=\"%s\" "
        "currentLineIndicator=\"%s\" "
        "whiteSpaceShow=\"%s\" eolShow=\"%s\" eolMode=\"%d\" "
        "zoom=\"%d\" smoothFont=\"%d\" paddingLeft=\"%d\" paddingRight=\"%d\" "
        "edgeMultiColumnPos=\"%s\" isEdgeBgMode=\"%s\"%s />\n",
        b2sh(g_prefs.show_line_numbers), b2yn(g_prefs.line_num_dyn_width),
        b2sh(g_prefs.show_bookmark_margin), fold_int_to_str(g_prefs.fold_style),
        b2yn(g_prefs.virtual_space), b2yn(g_prefs.scroll_beyond_last_line),
        b2yn(g_prefs.right_click_keeps_sel), b2yn(g_prefs.copy_line_no_selection),
        g_prefs.highlight_current_line ? "1" : "0",
        b2sh(g_prefs.show_whitespace), b2sh(g_prefs.show_eol),
        sc_eol_to_mac(g_prefs.default_eol),
        g_prefs.zoom_level,
        g_prefs.font_quality, g_prefs.padding_left, g_prefs.padding_right,
        edgeCol, b2yn(g_prefs.edge_mode == EDGE_BACKGROUND), edgeModeAttr);

    /* Linux-only additions — namespaced so a future macOS merge can pick
     * them up without colliding with existing macOS GUIConfig groups. */
    gchar *do_esc = g_markup_escape_text(g_prefs.delim_open,  -1);
    gchar *dc_esc = g_markup_escape_text(g_prefs.delim_close, -1);
    g_string_append_printf(b,
        "        <GUIConfig name=\"DelimiterSelection\" openSymbol=\"%s\" closeSymbol=\"%s\" entireDoc=\"%s\" />\n",
        do_esc, dc_esc, b2yn(g_prefs.delim_entire_doc));
    g_free(do_esc); g_free(dc_esc);

    gchar *wc_esc = g_markup_escape_text(g_prefs.word_chars, -1);
    g_string_append_printf(b,
        "        <GUIConfig name=\"WordCharsList\" useDefault=\"%s\" addedChars=\"%s\" />\n",
        b2yn(g_prefs.word_chars_use_default), wc_esc);
    g_free(wc_esc);

    g_string_append_printf(b,
        "        <GUIConfig name=\"LargeFileRestriction\" enabled=\"%s\" fileSizeMB=\"%d\" "
        "suppress2GB=\"%s\" deactivateWrap=\"%s\" allowAutoComplete=\"%s\" "
        "allowSmartHilite=\"%s\" allowBraceMatch=\"%s\" allowURLClick=\"%s\" />\n",
        b2yn(g_prefs.large_file_enabled), g_prefs.large_file_size_mb,
        b2yn(g_prefs.large_file_suppress), b2yn(g_prefs.large_file_no_wrap),
        b2yn(g_prefs.large_file_allow_autocomplete),
        b2yn(g_prefs.large_file_allow_smart_hilite),
        b2yn(g_prefs.large_file_allow_brace_match),
        b2yn(g_prefs.large_file_allow_url_click));

    g_string_append_printf(b,
        "        <GUIConfig name=\"NppToolbar\" iconSetSize=\"%d\" />\n",
        g_prefs.toolbar_icon_scale);

    gchar *se_esc = g_markup_escape_text(g_prefs.search_engine_url, -1);
    g_string_append_printf(b,
        "        <GUIConfig name=\"SearchEngine\" url=\"%s\" />\n", se_esc);
    g_free(se_esc);

    if (g_prefs.default_language[0]) {
        gchar *dl_esc = g_markup_escape_text(g_prefs.default_language, -1);
        g_string_append_printf(b,
            "        <GUIConfig name=\"NewDocDefaultLanguage\">%s</GUIConfig>\n", dl_esc);
        g_free(dl_esc);
    }

    /* P10 — Workspace roots. Emit only when at least one root is registered. */
    if (s_workspace_roots && s_workspace_roots->len > 0) {
        g_string_append(b, "        <GUIConfig name=\"Workspace\">\n");
        for (guint i = 0; i < s_workspace_roots->len; i++) {
            const char *p = (const char *)s_workspace_roots->pdata[i];
            if (!p) continue;
            gchar *esc = g_markup_escape_text(p, -1);
            g_string_append_printf(b,
                "            <Root path=\"%s\" />\n", esc);
            g_free(esc);
        }
        g_string_append(b, "        </GUIConfig>\n");
    }

    g_string_append(b, "    </GUIConfigs>\n</NotepadPlus>\n");
    g_file_set_contents(path, b->str, (gssize)b->len, NULL);
    g_string_free(b, TRUE);
    g_free(path);
}

/* ------------------------------------------------------------------ */
/* Dialog — forward declarations to other modules                      */
/* ------------------------------------------------------------------ */

void editor_apply_prefs(void);
void main_refresh_title(void);
void statusbar_set_visible(gboolean v);   /* defined in statusbar.c */
void toolbar_apply_icon_scale(int scale); /* may be stubbed */

/* Weak/optional hooks — provide stubs so the .c builds even when the
 * matching subsystem is not linked yet. */
__attribute__((weak)) void statusbar_set_visible(gboolean v)   { (void)v; }
__attribute__((weak)) void toolbar_apply_icon_scale(int scale) { (void)scale; }

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static GtkWidget *row(GtkWidget *grid, int r, const char *label, GtkWidget *widget)
{
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_end(lbl, 12);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, r, 1, 1);
    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), widget, 1, r, 1, 1);
    return widget;
}

static GtkWidget *make_grid(void)
{
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    gtk_widget_set_margin_start(g, 12);
    gtk_widget_set_margin_end(g, 12);
    gtk_widget_set_margin_top(g, 12);
    gtk_widget_set_margin_bottom(g, 12);
    return g;
}

/* Wrap a tab in a scrolled window so tall tabs stay usable. */
static GtkWidget *scroll(GtkWidget *child)
{
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), child);
    gtk_widget_set_size_request(sw, 540, 380);
    return sw;
}

static GtkWidget *make_check(GtkWidget *grid, int r, const char *label, gboolean active, GCallback cb)
{
    GtkWidget *c = gtk_check_button_new_with_label(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), active);
    gtk_grid_attach(GTK_GRID(grid), c, 0, r, 2, 1);
    if (cb) g_signal_connect(c, "toggled", cb, NULL);
    return c;
}

/* ------------------------------------------------------------------ */
/* Generic toggle handlers — set bool field + save                     */
/* ------------------------------------------------------------------ */
#define CHK(name, field, post)                                               \
    static void on_##name(GtkToggleButton *b, gpointer d) {                  \
        (void)d;                                                             \
        g_prefs.field = gtk_toggle_button_get_active(b);                     \
        do { post; } while (0);                                              \
        prefs_save();                                                        \
    }

CHK(bs_unindent,       backspace_unindent,         (void)0)
CHK(ac_enable,         autocomplete_enabled,       (void)0)
CHK(hl_line,           highlight_current_line,     editor_apply_prefs())
CHK(scroll_past,       scroll_beyond_last_line,    editor_apply_prefs())
CHK(full_path,         show_full_path_in_title,    main_refresh_title())
CHK(copy_line,         copy_line_no_selection,     (void)0)
CHK(word_wrap,         word_wrap,                  editor_apply_prefs())
CHK(auto_close,        auto_close_brackets,        (void)0)
CHK(smart_hilite,      smart_highlight,            (void)0)
CHK(smart_case,        smart_hilite_case,          (void)0)
CHK(smart_word,        smart_hilite_word,          (void)0)
CHK(virt_space,        virtual_space,              editor_apply_prefs())
CHK(no_drag,           disable_text_drag_drop,     editor_apply_prefs())
CHK(rmb_keep,          right_click_keeps_sel,      (void)0)
CHK(date_reverse,      date_time_reverse,          (void)0)
CHK(save_all_conf,     save_all_confirm,           (void)0)
CHK(mute,              mute_sounds,                (void)0)
CHK(show_ln,           show_line_numbers,          editor_apply_prefs())
CHK(ln_dyn,            line_num_dyn_width,         editor_apply_prefs())
CHK(show_bm,           show_bookmark_margin,       editor_apply_prefs())
CHK(show_ws,           show_whitespace,            editor_apply_prefs())
CHK(show_eol,          show_eol,                   editor_apply_prefs())
CHK(remember_sess,     remember_session,           (void)0)
CHK(keep_absent,       keep_absent_session,        (void)0)
CHK(panel_keep,        panel_keep_state,           (void)0)
CHK(fph,               func_params_hint,           (void)0)
CHK(spell,             spell_check,                (void)0)
CHK(fill_find,         fill_find_with_selection,   (void)0)
CHK(mono_find,         mono_font_find,             (void)0)
CHK(conf_rep,          confirm_replace_all,        (void)0)
CHK(rep_stop,          replace_and_stop,           (void)0)
CHK(delim_doc,         delim_entire_doc,           (void)0)
CHK(wc_default,        word_chars_use_default,     editor_apply_prefs())
CHK(lf_enable,         large_file_enabled,         (void)0)
CHK(lf_suppress,       large_file_suppress,        (void)0)
CHK(lf_nowrap,         large_file_no_wrap,         (void)0)
CHK(lf_allow_ac,       large_file_allow_autocomplete, (void)0)
CHK(lf_allow_sh,       large_file_allow_smart_hilite, (void)0)
CHK(lf_allow_bm,       large_file_allow_brace_match,  (void)0)
CHK(lf_allow_url,      large_file_allow_url_click,    (void)0)
CHK(tab_close_btn,     tab_close_button,           (void)0)
CHK(tab_dclose,        double_click_tab_close,     (void)0)
CHK(tab_wrap,          tab_bar_wrap,               (void)0)
CHK(status_visible,    show_status_bar,            statusbar_set_visible(g_prefs.show_status_bar))

static void on_tab_width(GtkSpinButton *s, gpointer d)
    { (void)d; g_prefs.tab_width = (int)gtk_spin_button_get_value(s); editor_apply_prefs(); prefs_save(); }
static void on_use_tabs(GtkToggleButton *b, gpointer d)
    { (void)d; g_prefs.use_tabs = gtk_toggle_button_get_active(b); editor_apply_prefs(); prefs_save(); }
static void on_use_spaces(GtkToggleButton *b, gpointer d)
    { (void)d; if (gtk_toggle_button_get_active(b)) { g_prefs.use_tabs = FALSE; editor_apply_prefs(); prefs_save(); } }

static void on_ai_set(int v)                                      { g_prefs.auto_indent = v; prefs_save(); }
static void on_ai_none(GtkToggleButton *b, gpointer d)            { (void)d; if (gtk_toggle_button_get_active(b)) on_ai_set(AUTO_INDENT_NONE); }
static void on_ai_basic(GtkToggleButton *b, gpointer d)           { (void)d; if (gtk_toggle_button_get_active(b)) on_ai_set(AUTO_INDENT_BASIC); }
static void on_ai_adv(GtkToggleButton *b, gpointer d)             { (void)d; if (gtk_toggle_button_get_active(b)) on_ai_set(AUTO_INDENT_ADVANCED); }

static void on_ac_min(GtkSpinButton *s, gpointer d)
    { (void)d; g_prefs.autocomplete_min_chars = (int)gtk_spin_button_get_value(s); prefs_save(); }

static void on_caret_w(GtkComboBox *c, gpointer d)
    { (void)d; g_prefs.caret_width = gtk_combo_box_get_active(c) + 1; editor_apply_prefs(); prefs_save(); }
static void on_blink(GtkSpinButton *s, gpointer d)
    { (void)d; g_prefs.caret_blink_rate = (int)gtk_spin_button_get_value(s); editor_apply_prefs(); prefs_save(); }
static void on_font_q(GtkComboBox *c, gpointer d)
    { (void)d; g_prefs.font_quality = gtk_combo_box_get_active(c); editor_apply_prefs(); prefs_save(); }

static void on_eol_set(int v)                                  { g_prefs.default_eol = v; prefs_save(); }
static void on_eol_lf(GtkToggleButton *b, gpointer d)          { (void)d; if (gtk_toggle_button_get_active(b)) on_eol_set(2); }
static void on_eol_crlf(GtkToggleButton *b, gpointer d)        { (void)d; if (gtk_toggle_button_get_active(b)) on_eol_set(0); }
static void on_eol_cr(GtkToggleButton *b, gpointer d)          { (void)d; if (gtk_toggle_button_get_active(b)) on_eol_set(1); }

static void on_enc_combo(GtkComboBox *c, gpointer d)
{
    (void)d;
    int idx = gtk_combo_box_get_active(c);
    if (idx >= 0 && idx < npp_encoding_count) {
        strncpy(g_prefs.default_encoding, npp_encodings[idx].display,
                sizeof(g_prefs.default_encoding) - 1);
        g_prefs.default_encoding[sizeof(g_prefs.default_encoding) - 1] = '\0';
    }
    prefs_save();
}

static void on_appear(GtkComboBox *c, gpointer d)
    { (void)d; g_prefs.appearance = gtk_combo_box_get_active(c); prefs_save(); }
static void on_toolbar_scale(GtkComboBox *c, gpointer d)
    { (void)d; g_prefs.toolbar_icon_scale = gtk_combo_box_get_active(c); toolbar_apply_icon_scale(g_prefs.toolbar_icon_scale); prefs_save(); }

static void on_edge_mode(GtkComboBox *c, gpointer d)
    { (void)d; g_prefs.edge_mode = gtk_combo_box_get_active(c); editor_apply_prefs(); prefs_save(); }
static void on_edge_col(GtkSpinButton *s, gpointer d)
    { (void)d; g_prefs.edge_column = (int)gtk_spin_button_get_value(s); editor_apply_prefs(); prefs_save(); }
static void on_fold(GtkComboBox *c, gpointer d)
    { (void)d; g_prefs.fold_style = gtk_combo_box_get_active(c); editor_apply_prefs(); prefs_save(); }
static void on_pad_l(GtkSpinButton *s, gpointer d)
    { (void)d; g_prefs.padding_left = (int)gtk_spin_button_get_value(s); editor_apply_prefs(); prefs_save(); }
static void on_pad_r(GtkSpinButton *s, gpointer d)
    { (void)d; g_prefs.padding_right = (int)gtk_spin_button_get_value(s); editor_apply_prefs(); prefs_save(); }
static void on_max_label(GtkSpinButton *s, gpointer d)
    { (void)d; g_prefs.tab_max_label_width = (int)gtk_spin_button_get_value(s); prefs_save(); }
static void on_lf_size(GtkSpinButton *s, gpointer d)
    { (void)d; g_prefs.large_file_size_mb = (int)gtk_spin_button_get_value(s); prefs_save(); }
static void on_in_sel(GtkSpinButton *s, gpointer d)
    { (void)d; g_prefs.in_sel_threshold = (int)gtk_spin_button_get_value(s); prefs_save(); }

static void on_str_entry(GtkEntry *e, gpointer d)
{
    /* d is a pointer to one of the char[] fields with capacity stashed
     * in the entry's "buflen" data. */
    char *dst = (char *)d;
    size_t cap = (size_t)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(e), "buflen"));
    if (!cap || !dst) return;
    const char *txt = gtk_entry_get_text(e);
    strncpy(dst, txt ? txt : "", cap - 1);
    dst[cap - 1] = '\0';
    prefs_save();
}

static GtkWidget *str_row(GtkWidget *grid, int r, const char *label,
                          char *field, size_t cap)
{
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), field);
    gtk_entry_set_max_length(GTK_ENTRY(e), (int)(cap - 1));
    g_object_set_data(G_OBJECT(e), "buflen", GUINT_TO_POINTER((guint)cap));
    g_signal_connect(e, "changed", G_CALLBACK(on_str_entry), field);
    gtk_widget_set_hexpand(e, TRUE);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), e,   1, r, 1, 1);
    return e;
}

static GtkWidget *s_backup_interval_spin = NULL;

static void on_backup_enabled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    g_prefs.backup_enabled = gtk_toggle_button_get_active(b);
    if (s_backup_interval_spin)
        gtk_widget_set_sensitive(s_backup_interval_spin, g_prefs.backup_enabled);
    backup_restart_timer();
    prefs_save();
}

static void on_backup_interval(GtkSpinButton *s, gpointer d)
{
    (void)d;
    g_prefs.backup_interval_secs = (int)gtk_spin_button_get_value(s);
    backup_restart_timer();
    prefs_save();
}

/* ------------------------------------------------------------------ */
/* Page builders                                                       */
/* ------------------------------------------------------------------ */

/* Localization — pick the UI language (Preferences > General). The
 * choice persists; i18n_set_language() reloads the translation tables so
 * dialogs opened afterwards use it (the menu bar updates on next launch). */
static void on_ui_language(GtkComboBox *c, gpointer d)
{
    (void)d;
    int idx = gtk_combo_box_get_active(c);
    const char *stem = i18n_language_stem(idx);
    if (!stem) return;
    g_strlcpy(g_prefs.ui_language, stem, sizeof(g_prefs.ui_language));
    prefs_save();
    i18n_set_language(stem);
    /* Retranslate the menu bar in place — no restart needed (#7). */
    extern void main_retranslate_menu(void);
    main_retranslate_menu();
}

static GtkWidget *page_general(void)
{
    /* Trimmed to match macOS General page: localization / title-bar /
     * status-bar / session memory. Tab Bar, Dark Mode, and the misc
     * toggles now have dedicated pages so the user can find them where
     * macOS users expect. */
    GtkWidget *g = make_grid();
    int r = 0;

    /* Localization: native names of every localization/*.xml file. */
    GtkWidget *lang = gtk_combo_box_text_new();
    int lsel = 0, lcount = i18n_language_count();
    for (int i = 0; i < lcount; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(lang),
                                       i18n_language_name(i));
        if (g_prefs.ui_language[0] &&
            g_strcmp0(i18n_language_stem(i), g_prefs.ui_language) == 0)
            lsel = i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(lang), lsel);
    g_signal_connect(lang, "changed", G_CALLBACK(on_ui_language), NULL);
    row(g, r++, "Language:", lang);

    make_check(g, r++, "Show status bar",                        g_prefs.show_status_bar,         G_CALLBACK(on_status_visible));
    make_check(g, r++, "Show full file path in title bar",       g_prefs.show_full_path_in_title, G_CALLBACK(on_full_path));
    make_check(g, r++, "Remember session on quit",               g_prefs.remember_session,        G_CALLBACK(on_remember_sess));

    return g;
}

static GtkWidget *page_tab_bar(void)
{
    /* Tab Bar — checkboxes + Max tab label width spin (macOS Tab Bar
     * page, PreferencesWindowController.mm:1101). */
    GtkWidget *g = make_grid();
    int r = 0;
    make_check(g, r++, "Show close button on tabs",      g_prefs.tab_close_button,      G_CALLBACK(on_tab_close_btn));
    make_check(g, r++, "Double-click to close tab",      g_prefs.double_click_tab_close, G_CALLBACK(on_tab_dclose));
    make_check(g, r++, "Wrap tabs to multiple lines",    g_prefs.tab_bar_wrap,          G_CALLBACK(on_tab_wrap));

    GtkWidget *ml = gtk_spin_button_new_with_range(40, 1000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ml), g_prefs.tab_max_label_width);
    row(g, r++, "Max tab width (pixels):", ml);
    g_signal_connect(ml, "value-changed", G_CALLBACK(on_max_label), NULL);
    return g;
}

/* Dark Mode radio handlers — three buttons sharing one group. Persist the
 * appearance pref through on_appear's combo path by faking an int and
 * applying directly here (the underlying field is just an int). */
/* Apply the appearance preference live — no restart needed.
 *   1. Resolve AUTO via desktop color-scheme.
 *   2. Flip GtkSettings/gtk-application-prefer-dark-theme so all GTK
 *      widgets restyle in place.
 *   3. Switch the editor theme to Default vs DarkModeDefault.
 *   4. Re-apply styles to every open Scintilla view.
 * Mirrors macOS _darkModeRadioChanged: at PreferencesWindowController.mm
 * lines 1242-1264.                                                       */
static void appearance_apply_live(void) {
    gboolean dark;
    if (g_prefs.appearance == APPEAR_DARK)       dark = TRUE;
    else if (g_prefs.appearance == APPEAR_LIGHT) dark = FALSE;
    else {
        dark = FALSE;
        GSettingsSchemaSource *src = g_settings_schema_source_get_default();
        if (src) {
            GSettingsSchema *schema = g_settings_schema_source_lookup(
                src, "org.gnome.desktop.interface", TRUE);
            if (schema) {
                if (g_settings_schema_has_key(schema, "color-scheme")) {
                    GSettings *gs = g_settings_new("org.gnome.desktop.interface");
                    if (gs) {
                        gchar *v = g_settings_get_string(gs, "color-scheme");
                        if (v && strcmp(v, "prefer-dark") == 0) dark = TRUE;
                        g_free(v); g_object_unref(gs);
                    }
                }
                g_settings_schema_unref(schema);
            }
        }
    }
    GtkSettings *s = gtk_settings_get_default();
    if (s) g_object_set(s, "gtk-application-prefer-dark-theme", dark, NULL);

    /* Swap the bundled theme presets so editor colours flip too. */
    extern void stylestore_load_theme(const char *path);
    const char *theme_xml = dark
        ? RESOURCES_DIR "/themes/DarkModeDefault.xml"
        : NULL;  /* NULL → pristine light default (stylers.model.xml),
                  * symmetric with Dark loading DarkModeDefault.xml. */
    stylestore_load_theme(theme_xml);

    /* Re-apply to every open editor view. */
    extern void editor_reapply_styles(void);
    editor_reapply_styles();
}

static void on_dm_auto (GtkToggleButton *b, gpointer d) { (void)d; if (gtk_toggle_button_get_active(b)) { g_prefs.appearance = 0; prefs_save(); appearance_apply_live(); } }
static void on_dm_light(GtkToggleButton *b, gpointer d) { (void)d; if (gtk_toggle_button_get_active(b)) { g_prefs.appearance = 1; prefs_save(); appearance_apply_live(); } }
static void on_dm_dark (GtkToggleButton *b, gpointer d) { (void)d; if (gtk_toggle_button_get_active(b)) { g_prefs.appearance = 2; prefs_save(); appearance_apply_live(); } }

static GtkWidget *page_dark_mode(void)
{
    /* Dark Mode — 3 radios matching macOS PreferencesWindowController.mm:
     * 1218 (Auto/Light/Dark). Restart-required on Linux since theme is
     * decided at gtk_init time via gtk-application-prefer-dark-theme. */
    GtkWidget *g = make_grid();
    int r = 0;
    GtkWidget *header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header), "<b>Appearance</b>");
    gtk_widget_set_halign(header, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g), header, 0, r++, 2, 1);

    GtkWidget *a = gtk_radio_button_new_with_label(NULL, "Auto (Follow System)");
    GtkWidget *l = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(a), "Light");
    GtkWidget *d = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(a), "Dark");
    if      (g_prefs.appearance == 0) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a), TRUE);
    else if (g_prefs.appearance == 1) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(l), TRUE);
    else                              gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d), TRUE);
    g_signal_connect(a, "toggled", G_CALLBACK(on_dm_auto),  NULL);
    g_signal_connect(l, "toggled", G_CALLBACK(on_dm_light), NULL);
    g_signal_connect(d, "toggled", G_CALLBACK(on_dm_dark),  NULL);
    gtk_grid_attach(GTK_GRID(g), a, 0, r++, 2, 1);
    gtk_grid_attach(GTK_GRID(g), l, 0, r++, 2, 1);
    gtk_grid_attach(GTK_GRID(g), d, 0, r++, 2, 1);

    GtkWidget *note = gtk_label_new(
        "Takes effect on next launch — GTK reads the theme preference at startup.");
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_widget_set_margin_top(note, 8);
    gtk_grid_attach(GTK_GRID(g), note, 0, r++, 2, 1);
    return g;
}

static GtkWidget *page_auto_completion(void)
{
    /* Auto-Completion — mirrors macOS _buildAutoCompletionPage at line
     * 1356: enable, from-Nth-char, function-parameter hint. The same
     * fields are also in the Editor page today; surfacing them here
     * matches macOS layout without breaking the editor page. */
    GtkWidget *g = make_grid();
    int r = 0;
    make_check(g, r++, "Enable auto-completion on each input",
        g_prefs.autocomplete_enabled, G_CALLBACK(on_ac_enable));
    make_check(g, r++, "Function parameters hint on input",
        g_prefs.func_params_hint, G_CALLBACK(on_fph));

    GtkWidget *m = gtk_spin_button_new_with_range(1, 10, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(m), g_prefs.autocomplete_min_chars);
    row(g, r++, "From Nth character:", m);
    g_signal_connect(m, "value-changed", G_CALLBACK(on_ac_min), NULL);
    return g;
}

static GtkWidget *page_misc(void)
{
    /* MISC. — collects checkboxes that macOS groups here, plus the
     * toolbar-icon-size dropdown (PreferencesWindowController.mm:1620). */
    GtkWidget *g = make_grid();
    int r = 0;
    make_check(g, r++, "Mute all sounds",                        g_prefs.mute_sounds,           G_CALLBACK(on_mute));
    make_check(g, r++, "Confirm before Save All",                g_prefs.save_all_confirm,      G_CALLBACK(on_save_all_conf));
    make_check(g, r++, "Reverse default date/time order",        g_prefs.date_time_reverse,     G_CALLBACK(on_date_reverse));
    make_check(g, r++, "Keep absent file entries in session",    g_prefs.keep_absent_session,   G_CALLBACK(on_keep_absent));
    make_check(g, r++, "Remember panel visibility across sessions", g_prefs.panel_keep_state,   G_CALLBACK(on_panel_keep));

    GtkWidget *tb = gtk_combo_box_text_new();
    /* 6-step % scale matching macOS PreferencesWindowController.mm:1656.
     * The persisted int is an index 0..5; toolbar.c maps each index to
     * a multiplier in pickScales[]. */
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(tb), "50%");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(tb), "75%");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(tb), "90%");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(tb), "100% (Default)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(tb), "125%");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(tb), "150%");
    int idx = g_prefs.toolbar_icon_scale;
    if (idx < 0 || idx > 5) idx = 3;   /* 100 % default if out of range */
    gtk_combo_box_set_active(GTK_COMBO_BOX(tb), idx);
    row(g, r++, "Toolbar icon size:", tb);
    g_signal_connect(tb, "changed", G_CALLBACK(on_toolbar_scale), NULL);
    return g;
}

static GtkWidget *page_editor(void)
{
    /* Field placement now matches macOS PreferencesWindowController.mm
     * _buildEditorPage (line ~640). Line-number, EOL, whitespace and
     * bookmark margin toggles live on the Editor tab there, even though
     * the underlying margin code is shared with the Margins tab. */
    GtkWidget *g = make_grid();
    int r = 0;

    /* Top group — view checkboxes (mirrors macOS preferences02.png). */
    make_check(g, r++, "Show line numbers",                     g_prefs.show_line_numbers,      G_CALLBACK(on_show_ln));
    make_check(g, r++, "Word wrap",                             g_prefs.word_wrap,              G_CALLBACK(on_word_wrap));
    make_check(g, r++, "Highlight current line",                g_prefs.highlight_current_line, G_CALLBACK(on_hl_line));
    make_check(g, r++, "Auto-close brackets () [] { }",         g_prefs.auto_close_brackets,    G_CALLBACK(on_auto_close));
    make_check(g, r++, "Enable virtual space",                  g_prefs.virtual_space,          G_CALLBACK(on_virt_space));
    make_check(g, r++, "Scroll beyond last line",               g_prefs.scroll_beyond_last_line,G_CALLBACK(on_scroll_past));
    make_check(g, r++, "Copy/cut line without selection",       g_prefs.copy_line_no_selection, G_CALLBACK(on_copy_line));
    make_check(g, r++, "Right-click keeps selection",           g_prefs.right_click_keeps_sel,  G_CALLBACK(on_rmb_keep));
    make_check(g, r++, "Disable selected text drag-drop",       g_prefs.disable_text_drag_drop, G_CALLBACK(on_no_drag));
    make_check(g, r++, "Show bookmark margin",                  g_prefs.show_bookmark_margin,   G_CALLBACK(on_show_bm));
    make_check(g, r++, "Show EOL markers",                      g_prefs.show_eol,               G_CALLBACK(on_show_eol));
    make_check(g, r++, "Show whitespace",                       g_prefs.show_whitespace,        G_CALLBACK(on_show_ws));

    /* Caret + font-quality group below (macOS preferences02.png bottom). */
    GtkWidget *cw = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cw), "Thin (1px)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cw), "Medium (2px)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cw), "Thick (3px)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(cw), g_prefs.caret_width - 1);
    row(g, r++, "Caret width:", cw);
    g_signal_connect(cw, "changed", G_CALLBACK(on_caret_w), NULL);

    GtkWidget *bl = gtk_spin_button_new_with_range(0, 2000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(bl), g_prefs.caret_blink_rate);
    row(g, r++, "Caret blink rate (ms):", bl);
    g_signal_connect(bl, "value-changed", G_CALLBACK(on_blink), NULL);

    GtkWidget *fq = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fq), "Default");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fq), "None");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fq), "Antialiased");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fq), "LCD Optimized");
    gtk_combo_box_set_active(GTK_COMBO_BOX(fq), g_prefs.font_quality);
    row(g, r++, "Font rendering:", fq);
    g_signal_connect(fq, "changed", G_CALLBACK(on_font_q), NULL);

    /* Spell-check stays here — Auto-Completion has its own page. */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(g), sep, 0, r++, 2, 1);
    make_check(g, r++, "Spell check while typing", g_prefs.spell_check, G_CALLBACK(on_spell));

    return g;
}

static GtkWidget *page_indentation(void)
{
    GtkWidget *g = make_grid();
    int r = 0;

    GtkWidget *tw = gtk_spin_button_new_with_range(1, 16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tw), g_prefs.tab_width);
    row(g, r++, "Tab / indent size:", tw);
    g_signal_connect(tw, "value-changed", G_CALLBACK(on_tab_width), NULL);

    GtkWidget *rtabs = gtk_radio_button_new_with_label(NULL, "Tab character");
    GtkWidget *rsp   = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(rtabs), "Space characters");
    if (g_prefs.use_tabs) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rtabs), TRUE);
    else                  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rsp),   TRUE);
    GtkWidget *ibx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_add(GTK_CONTAINER(ibx), rtabs);
    gtk_container_add(GTK_CONTAINER(ibx), rsp);
    row(g, r++, "Indent using:", ibx);
    g_signal_connect(rtabs, "toggled", G_CALLBACK(on_use_tabs),   NULL);
    g_signal_connect(rsp,   "toggled", G_CALLBACK(on_use_spaces), NULL);

    GtkWidget *an = gtk_radio_button_new_with_label(NULL, "None");
    GtkWidget *ab = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(an), "Basic");
    GtkWidget *aa = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(an), "Advanced (detect blocks)");
    if      (g_prefs.auto_indent == AUTO_INDENT_NONE)     gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(an), TRUE);
    else if (g_prefs.auto_indent == AUTO_INDENT_ADVANCED) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(aa), TRUE);
    else                                                  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ab), TRUE);
    GtkWidget *abx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_add(GTK_CONTAINER(abx), an);
    gtk_container_add(GTK_CONTAINER(abx), ab);
    gtk_container_add(GTK_CONTAINER(abx), aa);
    row(g, r++, "Auto-indent:", abx);
    g_signal_connect(an, "toggled", G_CALLBACK(on_ai_none),  NULL);
    g_signal_connect(ab, "toggled", G_CALLBACK(on_ai_basic), NULL);
    g_signal_connect(aa, "toggled", G_CALLBACK(on_ai_adv),   NULL);

    make_check(g, r++, "Backspace key removes one indent level", g_prefs.backspace_unindent, G_CALLBACK(on_bs_unindent));

    return g;
}

static GtkWidget *page_margins(void)
{
    /* Margins tab now hosts only the numeric / dropdown controls — the
     * boolean "Show …" toggles moved to the Editor page to match macOS
     * (PreferencesWindowController.mm:1135). */
    GtkWidget *g = make_grid();
    int r = 0;

    make_check(g, r++, "Dynamic line number width", g_prefs.line_num_dyn_width, G_CALLBACK(on_ln_dyn));

    GtkWidget *em = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(em), "Off");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(em), "Line");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(em), "Background");
    gtk_combo_box_set_active(GTK_COMBO_BOX(em), g_prefs.edge_mode);
    row(g, r++, "Vertical edge:", em);
    g_signal_connect(em, "changed", G_CALLBACK(on_edge_mode), NULL);

    GtkWidget *ec = gtk_spin_button_new_with_range(1, 500, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ec), g_prefs.edge_column);
    row(g, r++, "Edge column:", ec);
    g_signal_connect(ec, "value-changed", G_CALLBACK(on_edge_col), NULL);

    GtkWidget *fs = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fs), "Box tree");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fs), "Circle tree");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fs), "Arrow");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fs), "Simple +/-");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fs), "None");
    gtk_combo_box_set_active(GTK_COMBO_BOX(fs), g_prefs.fold_style);
    row(g, r++, "Fold style:", fs);
    g_signal_connect(fs, "changed", G_CALLBACK(on_fold), NULL);

    GtkWidget *pl = gtk_spin_button_new_with_range(0, 50, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pl), g_prefs.padding_left);
    row(g, r++, "Padding left (px):", pl);
    g_signal_connect(pl, "value-changed", G_CALLBACK(on_pad_l), NULL);

    GtkWidget *pr = gtk_spin_button_new_with_range(0, 50, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pr), g_prefs.padding_right);
    row(g, r++, "Padding right (px):", pr);
    g_signal_connect(pr, "value-changed", G_CALLBACK(on_pad_r), NULL);

    return g;
}

static GtkWidget *page_new_document(void)
{
    GtkWidget *g = make_grid();
    int r = 0;

    GtkWidget *lf = gtk_radio_button_new_with_label(NULL, "Unix (LF)");
    GtkWidget *cl = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(lf), "Windows (CRLF)");
    GtkWidget *cr = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(lf), "Old Mac (CR)");
    switch (g_prefs.default_eol) {
    case 0:  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cl), TRUE); break;
    case 1:  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cr), TRUE); break;
    default: gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lf), TRUE); break;
    }
    GtkWidget *eb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_add(GTK_CONTAINER(eb), lf);
    gtk_container_add(GTK_CONTAINER(eb), cl);
    gtk_container_add(GTK_CONTAINER(eb), cr);
    row(g, r++, "Default line ending:", eb);
    g_signal_connect(lf, "toggled", G_CALLBACK(on_eol_lf),   NULL);
    g_signal_connect(cl, "toggled", G_CALLBACK(on_eol_crlf), NULL);
    g_signal_connect(cr, "toggled", G_CALLBACK(on_eol_cr),   NULL);

    GtkWidget *enc = gtk_combo_box_text_new();
    int active_enc = 0;
    for (int i = 0; i < npp_encoding_count; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(enc), npp_encodings[i].display);
        if (strcmp(npp_encodings[i].display, g_prefs.default_encoding) == 0)
            active_enc = i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(enc), active_enc);
    row(g, r++, "Default encoding:", enc);
    g_signal_connect(enc, "changed", G_CALLBACK(on_enc_combo), NULL);

    str_row(g, r++, "Default language (empty = none):",
            g_prefs.default_language, sizeof(g_prefs.default_language));

    return g;
}

static GtkWidget *page_searching(void)
{
    /* Field order matches macOS preferences10.png: smart-highlight
     * checkboxes at the top, then fill-find, mono-font, confirm/replace
     * flags, then in-selection threshold. */
    GtkWidget *g = make_grid();
    int r = 0;

    make_check(g, r++, "Enable smart highlighting",          g_prefs.smart_highlight,           G_CALLBACK(on_smart_hilite));
    make_check(g, r++, "Smart highlighting: match case",     g_prefs.smart_hilite_case,         G_CALLBACK(on_smart_case));
    make_check(g, r++, "Smart highlighting: whole word only",g_prefs.smart_hilite_word,         G_CALLBACK(on_smart_word));
    make_check(g, r++, "Fill find field with selected text", g_prefs.fill_find_with_selection,  G_CALLBACK(on_fill_find));
    make_check(g, r++, "Use monospaced font in Find dialog", g_prefs.mono_font_find,            G_CALLBACK(on_mono_find));
    make_check(g, r++, "Confirm Replace All in open documents", g_prefs.confirm_replace_all,    G_CALLBACK(on_conf_rep));
    make_check(g, r++, "Replace: don't move to next occurrence", g_prefs.replace_and_stop,      G_CALLBACK(on_rep_stop));

    GtkWidget *is = gtk_spin_button_new_with_range(0, 1000000, 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(is), g_prefs.in_sel_threshold);
    row(g, r++, "In-selection auto-check threshold (bytes):", is);
    g_signal_connect(is, "value-changed", G_CALLBACK(on_in_sel), NULL);

    str_row(g, r++, "Search engine URL (%s = query):",
            g_prefs.search_engine_url, sizeof(g_prefs.search_engine_url));

    return g;
}

static GtkWidget *page_delimiter(void)
{
    GtkWidget *g = make_grid();
    int r = 0;

    str_row(g, r++, "Open delimiter:",  g_prefs.delim_open,  sizeof(g_prefs.delim_open));
    str_row(g, r++, "Close delimiter:", g_prefs.delim_close, sizeof(g_prefs.delim_close));
    make_check(g, r++, "Select across the entire document", g_prefs.delim_entire_doc, G_CALLBACK(on_delim_doc));

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(g), sep, 0, r++, 2, 1);
    GtkWidget *lb = gtk_label_new("Word characters");
    gtk_widget_set_halign(lb, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g), lb, 0, r++, 2, 1);

    make_check(g, r++, "Use Scintilla default word characters", g_prefs.word_chars_use_default, G_CALLBACK(on_wc_default));
    str_row(g, r++, "Extra word chars:", g_prefs.word_chars, sizeof(g_prefs.word_chars));

    return g;
}

static GtkWidget *page_performance(void)
{
    GtkWidget *g = make_grid();
    int r = 0;

    make_check(g, r++, "Enable large-file mode",             g_prefs.large_file_enabled, G_CALLBACK(on_lf_enable));

    GtkWidget *sz = gtk_spin_button_new_with_range(1, 100000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sz), g_prefs.large_file_size_mb);
    row(g, r++, "Trigger at file size (MB):", sz);
    g_signal_connect(sz, "value-changed", G_CALLBACK(on_lf_size), NULL);

    make_check(g, r++, "Suppress large-file warning dialog", g_prefs.large_file_suppress, G_CALLBACK(on_lf_suppress));
    make_check(g, r++, "Disable word wrap for large files",  g_prefs.large_file_no_wrap, G_CALLBACK(on_lf_nowrap));
    make_check(g, r++, "Allow auto-completion in large files", g_prefs.large_file_allow_autocomplete, G_CALLBACK(on_lf_allow_ac));
    make_check(g, r++, "Allow smart highlight in large files", g_prefs.large_file_allow_smart_hilite, G_CALLBACK(on_lf_allow_sh));
    make_check(g, r++, "Allow brace matching in large files",  g_prefs.large_file_allow_brace_match,  G_CALLBACK(on_lf_allow_bm));
    make_check(g, r++, "Allow URL hot-spot clicks in large files", g_prefs.large_file_allow_url_click, G_CALLBACK(on_lf_allow_url));

    return g;
}

static GtkWidget *page_backup(void)
{
    GtkWidget *g = make_grid();

    GtkWidget *chk = gtk_check_button_new_with_label("Enable auto-backup for unsaved changes");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), g_prefs.backup_enabled);
    gtk_grid_attach(GTK_GRID(g), chk, 0, 0, 2, 1);
    g_signal_connect(chk, "toggled", G_CALLBACK(on_backup_enabled), NULL);

    s_backup_interval_spin = gtk_spin_button_new_with_range(10, 3600, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_backup_interval_spin),
                              g_prefs.backup_interval_secs);
    gtk_widget_set_sensitive(s_backup_interval_spin, g_prefs.backup_enabled);
    row(g, 1, "Backup interval (seconds):", s_backup_interval_spin);
    g_signal_connect(s_backup_interval_spin, "value-changed",
                     G_CALLBACK(on_backup_interval), NULL);

    GtkWidget *info = gtk_label_new("Backup files are written to ~/" APP_CONFIG_DIR "/backup/\n"
                                    "and removed when the file is saved or closed.");
    gtk_widget_set_halign(info, GTK_ALIGN_START);
    gtk_widget_set_margin_top(info, 8);
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    gtk_grid_attach(GTK_GRID(g), info, 0, 2, 2, 1);

    return g;
}

/* ------------------------------------------------------------------ */
/* Dialog                                                              */
/* ------------------------------------------------------------------ */

static GtkWidget *s_prefs_dlg = NULL;

static void on_prefs_response(GtkDialog *dlg, gint r, gpointer d)
{
    (void)r; (void)d;
    gtk_widget_hide(GTK_WIDGET(dlg));
}

/* Tab label for the vertical (left-side) prefs notebook strip. The label
 * fills the tab width and right-aligns its text, so the titles sit flush
 * against the page content rather than ragged on the left. */
static GtkWidget *prefs_tab_label(const char *text)
{
    GtkWidget *lbl = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 1.0f);
    gtk_widget_set_halign(lbl, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(lbl, TRUE);
    return lbl;
}

void prefs_dialog_show(GtkWidget *parent)
{
    if (s_prefs_dlg) {
        gtk_window_set_transient_for(GTK_WINDOW(s_prefs_dlg), GTK_WINDOW(parent));
        gtk_window_present(GTK_WINDOW(s_prefs_dlg));
        return;
    }

    s_prefs_dlg = gtk_dialog_new_with_buttons(
        "Preferences",
        GTK_WINDOW(parent),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(s_prefs_dlg), 600, 500);

    GtkWidget *nb = gtk_notebook_new();
    /* Vertical tab strip down the left edge — there are 13 pages, which
     * overflow a horizontal strip; a left-side list matches the macOS
     * Preferences sidebar too. */
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_LEFT);
    /* Page order matches macOS PreferencesWindowController.mm:310-324
     * (the sidebar layout) so users moving between platforms find the
     * same tab in the same slot. */
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_general()),         prefs_tab_label("General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_editor()),          prefs_tab_label("Editor"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_indentation()),     prefs_tab_label("Indentation"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_tab_bar()),         prefs_tab_label("Tab Bar"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_dark_mode()),       prefs_tab_label("Dark Mode"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_margins()),         prefs_tab_label("Margins"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_new_document()),    prefs_tab_label("New Document"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_backup()),          prefs_tab_label("Backup"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_auto_completion()), prefs_tab_label("Auto-Completion"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_searching()),       prefs_tab_label("Searching"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_delimiter()),       prefs_tab_label("Delimiter"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_performance()),     prefs_tab_label("Performance"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll(page_misc()),            prefs_tab_label("MISC."));

    GtkWidget *ca = gtk_dialog_get_content_area(GTK_DIALOG(s_prefs_dlg));
    gtk_container_set_border_width(GTK_CONTAINER(ca), 8);
    gtk_container_add(GTK_CONTAINER(ca), nb);

    g_signal_connect(s_prefs_dlg, "response", G_CALLBACK(on_prefs_response), NULL);
    gtk_widget_show_all(s_prefs_dlg);
}

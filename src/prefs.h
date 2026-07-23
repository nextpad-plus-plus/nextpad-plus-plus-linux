#ifndef PREFS_H
#define PREFS_H

#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/* Auto-indent modes — values match macOS kPrefAutoIndent so the same
 * config.xml is interpreted identically on both platforms. macOS source:
 * PreferencesWindowController.mm:10 ("0=None 1=Advanced 2=Basic").       */
/* ------------------------------------------------------------------ */
#define AUTO_INDENT_NONE     0
#define AUTO_INDENT_ADVANCED 1
#define AUTO_INDENT_BASIC    2

/* ------------------------------------------------------------------ */
/* Edge mode (SCI_SETEDGEMODE)                                         */
/* ------------------------------------------------------------------ */
#define EDGE_NONE       0
#define EDGE_LINE       1
#define EDGE_BACKGROUND 2

/* ------------------------------------------------------------------ */
/* Fold style                                                          */
/* ------------------------------------------------------------------ */
#define FOLD_BOX_TREE    0
#define FOLD_CIRCLE_TREE 1
#define FOLD_ARROW       2
#define FOLD_SIMPLE      3
#define FOLD_NONE        4

/* ------------------------------------------------------------------ */
/* Dark-mode appearance preference                                     */
/* ------------------------------------------------------------------ */
#define APPEAR_AUTO  0
#define APPEAR_LIGHT 1
#define APPEAR_DARK  2

/* ------------------------------------------------------------------ */
/* Persistent preferences                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    /* ── Indentation tab ────────────────────────────────────── */
    int      tab_width;               /* 1-16, default 4 */
    gboolean use_tabs;                /* FALSE = spaces (default) */
    int      auto_indent;             /* AUTO_INDENT_* */
    gboolean backspace_unindent;      /* backspace removes one indent stop */

    /* ── Editor tab ─────────────────────────────────────────── */
    gboolean highlight_current_line;  /* default TRUE */
    int      caret_width;             /* 1-3 px, default 1 */
    int      line_spacing10;          /* line-spacing multiplier ×10:
                                       * 10/12/13/14/15 (macOS #149) */
    int      ac_mode;                 /* completion source — 0 Function,
                                       * 1 Word, 2 Function+Word (maps to
                                       * Windows autoCAction 1/2/3) */
    gboolean ac_brief;                /* brief list: prefix subset only */
    gboolean ac_ignore_numbers;       /* no completion for digit prefix */
    int      caret_blink_rate;        /* ms, 0 = no blink, default 600 */
    gboolean scroll_beyond_last_line; /* default FALSE */
    gboolean word_wrap;               /* default FALSE */
    /* Auto-Insert matched pairs (macOS efbb0a7 / Windows
     * AutoCompletion::insertMatchedChars). */
    gboolean ai_parens;               /* ( )  default FALSE */
    gboolean ai_brackets;             /* [ ]  default FALSE */
    gboolean ai_braces;               /* { }  default FALSE */
    gboolean ai_quotes;               /* ' '  default FALSE */
    gboolean ai_dquotes;              /* " "  default FALSE */
    gboolean ai_html;                 /* </tag> on '>'  default FALSE */
    gboolean smart_highlight;         /* default TRUE */
    gboolean smart_hilite_case;       /* default FALSE */
    gboolean smart_hilite_word;       /* default TRUE */
    gboolean virtual_space;           /* default FALSE */
    gboolean disable_text_drag_drop;  /* default FALSE */
    gboolean right_click_keeps_sel;   /* default TRUE */
    int      font_quality;            /* 0=default,1=none,2=antialiased,3=lcd */
    gboolean date_time_reverse;       /* default FALSE */
    gboolean save_all_confirm;        /* default TRUE */
    gboolean mute_sounds;             /* default FALSE */

    /* ── Margins tab ────────────────────────────────────────── */
    gboolean show_line_numbers;       /* default TRUE */
    gboolean line_num_dyn_width;      /* default TRUE */
    gboolean show_bookmark_margin;    /* default TRUE */
    gboolean show_whitespace;         /* default FALSE */
    gboolean show_eol;                /* default FALSE */
    int      edge_mode;               /* EDGE_* */
    int      edge_column;             /* default 80 */
    int      fold_style;              /* FOLD_* */
    int      padding_left;            /* px, default 4 */
    int      padding_right;           /* px, default 4 */
    int      zoom_level;              /* SCI_SETZOOM delta from base font.
                                       * Persisted in config.xml's
                                       * ScintillaPrimaryView/@zoom so the
                                       * editor reopens at the same zoom
                                       * level — matches macOS
                                       * kPrefZoomLevel. */

    /* ── New Document tab ───────────────────────────────────── */
    int      default_eol;             /* SC_EOL_LF/CRLF/CR, default SC_EOL_LF */
    char     default_encoding[32];    /* "UTF-8" etc, default "UTF-8" */
    char     default_language[32];    /* default lexer for new docs ("" = none) */

    /* ── General tab ────────────────────────────────────────── */
    gboolean show_full_path_in_title; /* default FALSE */
    gboolean show_status_bar;         /* default TRUE */
    gboolean copy_line_no_selection;  /* copy/cut whole line, default TRUE */
    int      toolbar_icon_scale;      /* index 0..5 into pickScales[]
                                       * = {0.50, 0.75, 0.90, 1.00, 1.25, 1.50}.
                                       * Matches macOS kPrefToolbarIconScale
                                       * row layout (PreferencesWindowController
                                       * .mm:1666). 3 = 100 % default. */
    gboolean remember_session;        /* default TRUE */
    gboolean keep_absent_session;     /* default FALSE */
    gboolean panel_keep_state;        /* default TRUE */
    /* GAP-82/83 — side-panel persistence (panelstate.c owns the
     * formats; prefs.c only loads/stores the raw strings). */
    char     open_side_panels[256];   /* "doclist funclist:popped" */
    char     side_panel_widths[512];  /* "doclist=320 funclist=280" */
    char     floating_panels[512];    /* "funclist=420x600:pinned" */
    int      appearance;              /* APPEAR_* */
    char     theme_preset[64];        /* "Default" / "DarkModeDefault" / ... */
    char     ui_language[64];         /* localization XML stem; "" = auto-detect */

    /* ── Auto-completion (lives on Editor tab) ──────────────── */
    gboolean autocomplete_enabled;    /* default TRUE */
    int      autocomplete_min_chars;  /* trigger after N chars, default 1 */
    gboolean func_params_hint;        /* default TRUE */
    gboolean spell_check;             /* default FALSE */

    /* ── Searching tab ──────────────────────────────────────── */
    gboolean fill_find_with_selection;/* default TRUE */
    gboolean mono_font_find;          /* default TRUE */
    gboolean confirm_replace_all;     /* default TRUE */
    gboolean replace_and_stop;        /* default FALSE */
    gboolean use_boost_regex;         /* default FALSE — opt-in Boost.Regex
                                       * engine (multi-line, lookbehind, \K);
                                       * mirrored into gNppUseBoostRegex */
    /* GAP-53 — Find window transparency (defaults match Windows/macOS). */
    gboolean find_transp_enabled;     /* default TRUE                     */
    gboolean find_transp_always;      /* FALSE = only on losing focus     */
    int      find_transp_alpha;       /* opacity %, 20–90, default 50     */

    /* GAP-70 — appearance style: 0 Classic (default, untouched) /
     * 1 Modern (Tahoe-inspired CSS). Restart-gated like macOS. */
    int      appearance_style;

    /* GAP-41 — Backspace/arrows convert a column (rectangular) selection
     * to multi-caret editing first (N++ default behaviour). */
    gboolean column_sel_to_multi_edit;

    /* GAP-27 — File Status Auto-Detection (macOS 45add16 #116). When
     * detection is off, external on-disk changes are ignored (a per-tab
     * tail -f monitor still works). "Update silently" reloads clean
     * buffers without prompting; dirty buffers always prompt. */
    gboolean file_auto_detect;        /* default TRUE  */
    gboolean file_update_silently;    /* default FALSE */

    /* GAP-37 — clickable links (macOS Cloud-and-Link pane; defaults
     * match Windows urlUnderLineFg behaviour). */
    /* GAP-46/47 — toolbar icon colorization + standard icon set. */
    int      toolbar_color_mode;     /* 0=off 1=partial 2=complete */
    int      toolbar_color_choice;   /* 0-6 palette, 7 accent, 8 custom */
    char     toolbar_color_custom[8];/* "#RRGGBB" */
    gboolean toolbar_color_plugins;
    gboolean toolbar_standard_icons;

    /* GAP-43 — Style Configurator Global override "Force ..." flags. */
    gboolean gov_fg, gov_bg, gov_font, gov_font_size;
    gboolean gov_bold, gov_italic, gov_underline;

    gboolean clickable_link_enable;       /* default TRUE  */
    gboolean clickable_link_no_underline; /* default FALSE */
    gboolean clickable_link_fullbox;      /* default FALSE */
    char     clickable_link_schemes[512]; /* space-separated URI schemes */
    int      in_sel_threshold;        /* min chars of selection to enable "in selection" */
    char     search_engine_url[256];  /* default "https://duckduckgo.com/?q=%s" */

    /* ── Delimiter tab ──────────────────────────────────────── */
    char     delim_open[8];           /* default "(" */
    char     delim_close[8];          /* default ")" */
    gboolean delim_entire_doc;        /* default FALSE */
    char     word_chars[128];         /* extra word chars */
    gboolean word_chars_use_default;  /* default TRUE */

    /* ── Performance tab (large file restriction) ──────────── */
    gboolean large_file_enabled;      /* default TRUE */
    int      large_file_size_mb;      /* default 200 MB */
    gboolean large_file_suppress;     /* suppress dialog, default FALSE */
    gboolean large_file_no_wrap;      /* default TRUE */
    gboolean large_file_allow_autocomplete; /* default FALSE */
    gboolean large_file_allow_smart_hilite; /* default FALSE */
    gboolean large_file_allow_brace_match;  /* default FALSE */
    gboolean large_file_allow_url_click;    /* default TRUE */

    /* ── Tab bar (under General) ────────────────────────────── */
    gboolean tab_close_button;        /* default TRUE */
    gboolean double_click_tab_close;  /* default FALSE */

    /* ── Backup ─────────────────────────────────────────────── */
    char     backup_custom_dir[1024]; /* "" = default <user>/backup
                                       * (macOS kPrefBackupDir) */

    /* ── Document List panel ────────────────────────────────── */
    gboolean doclist_show_ext;        /* default TRUE  */
    gboolean doclist_show_path;       /* default TRUE  */
    gboolean tab_bar_wrap;            /* default FALSE */
    gboolean tab_follow_zoom;         /* GAP-31 — tabs grow with editor zoom */
    int      tab_max_label_width;     /* px, default 200 */
    gboolean hide_tab_bar;            /* default FALSE (macOS #183) */

    /* ── Backup tab ─────────────────────────────────────────── */
    gboolean backup_enabled;          /* default TRUE */
    int      backup_interval_secs;    /* seconds between backup writes, default 60 */
} NppPrefs;

extern NppPrefs g_prefs;

/* P14 — per-language indentation overrides.
 * Maps language name (e.g. "cpp") to an override pair. Both values 0 / FALSE
 * means "use globals." */
typedef struct {
    int      tab_size;
    gboolean use_tabs;
} TabOverride;

/* Returns a pointer owned by prefs.c, or NULL if no override registered. */
const TabOverride *prefs_tab_override_for(const char *lang);

/* Set or replace an override for a language. */
void prefs_tab_override_set(const char *lang, int tab_size, gboolean use_tabs);

/* Remove an override (revert to globals for that language). */
void prefs_tab_override_clear(const char *lang);

/* Enumerate all overrides (caller MUST g_list_free; values still owned). */
GList *prefs_tab_overrides_keys(void);

/* P10 — workspace roots loaded from config.xml's <Workspace> group.
 * Returned array is owned by prefs.c; do not free. */
const char *const *prefs_workspace_roots(int *out_n);

/* Replace the persisted workspace-root list. Pointers are duplicated.
 * Pass NULL/0 to clear. */
void prefs_workspace_roots_set(const char *const *paths, int n);

/* Load from ~/<APP_CONFIG_DIR>/config.xml (call before building UI) */
void prefs_load(void);

/* Save to ~/<APP_CONFIG_DIR>/config.xml */
void prefs_save(void);

/* Show (or raise) the Preferences dialog */
void prefs_dialog_show(GtkWidget *parent);

/* Error bell honouring the "Mute all sounds" pref (macOS NSBeep sites).
 * Safe from any code — no widget required. */
void npp_beep(void);

#endif /* PREFS_H */

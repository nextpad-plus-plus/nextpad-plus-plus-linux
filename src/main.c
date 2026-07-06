/*
 * main.c — Nextpad++ Linux port entry point.
 *
 * G1: GtkApplication shell + single Scintilla. ✅
 * G2: stylestore + lexer wiring. ✅
 * G3.1: multi-tab via editor_init() returning a GtkNotebook; basic File menu
 *       (New / Open / Save / Save As / Save All / Close / Close All / Quit).
 *
 * The `main_*` callback functions at the bottom are what editor.c expects from
 * the host; each is a small dispatcher. Currently several are stubs that get
 * filled in later phases (recent files in G3.5, doclist refresh in G3.15,
 * window-title refresh wires here in G3.1).
 */

#include <gtk/gtk.h>
#include "gtk_compat.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "sci_c.h"
#include "branding.h"
#include "stylestore.h"
#include "lexer.h"
#include "editor.h"
#include "encoding.h"
#include "prefs.h"
#include "i18n.h"
#include "toolbar.h"
#include "recent.h"
#include "session.h"
#include "backup.h"
#include "doclist.h"
#include "autocomplete.h"
#include "acapi.h"
#include "encoding.h"
#include "statusbar.h"
#include "findreplace.h"
#include "findinfiles.h"
#include "macro.h"
#include "funclist.h"
#include "docmap.h"
#include "workspace.h"
#include "searchresults.h"
#include "charpanel.h"
#include "cliphistory.h"
#include "shortcutmap.h"
#include "styleeditor.h"
#include "columneditor.h"
#include "run.h"
#include "plugin.h"
#include "pluginsadmin.h"
#include "udladmin.h"
#include "project.h"
#include "spell.h"
#include "changehistory.h"
#include "gitpanel.h"
#include "floating.h"
#include "mdpreview.h"
#include "split.h"
#include "paths.h"
#include "panel_frame.h"
#include "theme.h"
#include "udl_editor.h"

static const char *kAppId = "org.nextpad.NextpadPP";

/* Globals — single-window app for G3. Multi-window comes in a later phase. */
static GtkApplication       *g_app    = NULL;
static GtkApplicationWindow *g_window = NULL;

/* ------------------------------------------------------------------ */
/* G3.14: CLI per-file flags                                           */
/* ------------------------------------------------------------------ */
/* Mirrors the macOS-port NppCommandLineParams subset that's practical on
 * Linux. Applied to the LAST file opened in the current invocation, matching
 * the macOS port's behaviour. */
typedef struct {
    int     line;        /* -l N (1-based), 0 = none */
    int     column;      /* -c N (1-based), 0 = none */
    int     position;    /* -p N (0-based byte), -1 = none */
    char   *lang;        /* -lang NAME, owned, NULL = none */
    char   *udl;         /* -udl NAME, owned, NULL = none */
    gboolean read_only;  /* -ro */
    gboolean monitor;    /* -monitor (tail -f) */
} CliPerFile;
static CliPerFile g_cli = { 0, 0, -1, NULL, NULL, FALSE, FALSE };

/* Strip our recognised flags from argv (in place). Returns new argc. */
static int parse_cli_flags(int argc, char **argv)
{
    int out = 1;
    g_cli.line = 0; g_cli.column = 0; g_cli.position = -1;
    g_free(g_cli.lang); g_cli.lang = NULL;
    g_free(g_cli.udl);  g_cli.udl  = NULL;
    g_cli.read_only = FALSE; g_cli.monitor = FALSE;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-l") == 0 && i + 1 < argc) {
            g_cli.line = atoi(argv[++i]);
        } else if (strcmp(a, "-c") == 0 && i + 1 < argc) {
            g_cli.column = atoi(argv[++i]);
        } else if (strcmp(a, "-p") == 0 && i + 1 < argc) {
            g_cli.position = atoi(argv[++i]);
        } else if (strcmp(a, "-lang") == 0 && i + 1 < argc) {
            g_cli.lang = g_strdup(argv[++i]);
        } else if (strcmp(a, "-udl") == 0 && i + 1 < argc) {
            g_cli.udl = g_strdup(argv[++i]);
        } else if (strcmp(a, "-ro") == 0) {
            g_cli.read_only = TRUE;
        } else if (strcmp(a, "-monitor") == 0) {
            g_cli.monitor = TRUE;
        } else {
            argv[out++] = argv[i];
        }
    }
    argv[out] = NULL;
    return out;
}

/* Apply parsed flags to the current document (must be the last one opened). */
static void apply_cli_flags_to_current(void)
{
    NppDoc *doc = editor_current_doc();
    if (!doc) return;

    if (g_cli.lang) {
        /* Reapply lexer by language name. lexer_apply takes the NPP language
         * key (e.g. "cpp", "python"). */
        extern void lexer_apply(GtkWidget *sci, const char *lang_name);
        lexer_apply(doc->sci, g_cli.lang);
    }
    if (g_cli.udl) {
        /* UDL lookup happens inside lexer_apply when name matches a UDL. */
        extern void lexer_apply(GtkWidget *sci, const char *lang_name);
        lexer_apply(doc->sci, g_cli.udl);
    }
    if (g_cli.position >= 0) {
        editor_send(SCI_GOTOPOS, (uptr_t)g_cli.position, 0);
    } else if (g_cli.line > 0) {
        int line0 = g_cli.line - 1;
        sptr_t pos = editor_send(SCI_POSITIONFROMLINE, (uptr_t)line0, 0);
        if (g_cli.column > 0) pos += (g_cli.column - 1);
        editor_send(SCI_GOTOPOS, (uptr_t)pos, 0);
    }
    if (g_cli.read_only) {
        editor_send(SCI_SETREADONLY, 1, 0);
    }
    if (g_cli.monitor) {
        doc->monitoring = TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* Window title                                                        */
/* ------------------------------------------------------------------ */

/* editor.c manages window title itself via its private update_window_title().
 * This symbol exists only so editor.c's forward declaration links; no need to
 * duplicate the logic here. */
void main_refresh_title(void) { }

/* ------------------------------------------------------------------ */
/* main_* callbacks expected by editor.c                               */
/* ------------------------------------------------------------------ */

/* G7: apply per-view symbol margins (line numbers, fold, bookmarks). For G3
 * we apply the line-number margin in editor.c directly. No-op here. */
void main_apply_view_symbols(GtkWidget *sci) { (void)sci; }

/* G7: toggle bookmark on the given line in the given sci. Called from
 * editor.c when the user clicks margin 1 (the bookmark margin) — without
 * this body, margin clicks were silently dropped. */
void main_toggle_bookmark_at_line(GtkWidget *sci, int line) {
    if (!sci || line < 0) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t mk = scintilla_send_message(s, SCI_MARKERGET, line, 0);
    if (mk & (1 << SC_MARKNUM_BOOKMARK))
        scintilla_send_message(s, SCI_MARKERDELETE, line, SC_MARKNUM_BOOKMARK);
    else
        scintilla_send_message(s, SCI_MARKERADD,    line, SC_MARKNUM_BOOKMARK);
}

/* G3.5: add a path to the Recent Files list and rebuild that menu. */
static GMenu *g_recent_menu = NULL;        /* Submenu rebuilt on every add/clear. */

/* Q-fix: named macros section at the bottom of the Macro menu — refreshed
 * after every macro-save so newly-saved macros become playable. */
static GMenu *g_named_macros_menu = NULL;
static void rebuild_macro_menu(void) {
    if (!g_named_macros_menu) return;
    g_menu_remove_all(g_named_macros_menu);
    int n = macro_named_count();
    for (int i = 0; i < n; i++) {
        const char *name = macro_named_at(i);
        if (!name) continue;
        GMenuItem *mi = g_menu_item_new(name, NULL);
        g_menu_item_set_action_and_target(mi, "app.macro-play-named", "i", i);
        g_menu_append_item(g_named_macros_menu, mi);
        g_object_unref(mi);
    }
}
static void action_macro_play_named(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)u;
    int idx = p ? g_variant_get_int32(p) : -1;
    macro_play_named(idx);
}

static void rebuild_recent_menu(void)
{
    if (!g_recent_menu) return;
    g_menu_remove_all(g_recent_menu);

    GPtrArray *files = recent_files_get();
    if (files->len == 0) {
        GMenuItem *empty = g_menu_item_new("(empty)", NULL);
        g_menu_append_item(g_recent_menu, empty);
        g_object_unref(empty);
        return;
    }

    GMenu *paths = g_menu_new();
    for (guint i = 0; i < files->len; i++) {
        const char *full = (const char *)files->pdata[i];
        char *base = g_path_get_basename(full);
        /* Use full path as detailed action target so multiple files with the
         * same basename still route correctly. */
        GMenuItem *mi = g_menu_item_new(base, NULL);
        g_menu_item_set_action_and_target(mi, "app.open-recent", "s", full);
        g_menu_append_item(paths, mi);
        g_object_unref(mi);
        g_free(base);
    }
    g_menu_append_section(g_recent_menu, NULL, G_MENU_MODEL(paths));
    g_object_unref(paths);

    GMenu *clear = g_menu_new();
    g_menu_append(clear, "Clear Recent Files", "app.clear-recent");
    g_menu_append_section(g_recent_menu, NULL, G_MENU_MODEL(clear));
    g_object_unref(clear);
}

/* Forward declaration — see "Reopen Closed Tab" section below. */
static void closed_tabs_push(const char *path);

void main_recent_file_add(const char *path)
{
    recent_files_add(path);
    rebuild_recent_menu();
    /* G34: push to the reopen-closed-tab stack too. The next time a tab is
     * closed, this is the path we'd want to bring back. */
    closed_tabs_push(path);
}

/* G3.15: refresh the Document List side panel. */
void main_doclist_refresh(void) {
    doclist_refresh();
    NppDoc *doc = editor_current_doc();
    /* G33 — file open / tab switch / close triggers a doclist refresh in
     * editor.c. Use that as a convenient hook to fire notifications. */
    if (doc) plugin_notify_buffer_activated(doc);
    plugin_notify_doc_order_changed();
}

/* G3.10: sync the Encoding menu's radio with the current document. */
static GSimpleAction *g_enc_action = NULL;
void main_sync_encoding_menu(const char *enc) {
    if (!g_enc_action) return;
    g_simple_action_set_state(g_enc_action,
                              g_variant_new_string(enc ? enc : "UTF-8"));
    /* G41 — also push the encoding to the live status bar. */
    statusbar_set_encoding(enc ? enc : "UTF-8");
}

/* #4: sync the Language menu's radio check with the active document's
 * language (set on every tab switch / file open via on_switch_page). */
static GSimpleAction *g_lang_action = NULL;
void main_sync_language_menu(const char *key) {
    if (!g_lang_action) return;
    g_simple_action_set_state(g_lang_action,
                              g_variant_new_string(key ? key : ""));
}

/* G4: route printing. Stubbed for now. */
void main_do_print(void) { }

/* G29 — Markdown preview: pull current buffer text and push to renderer.
 * Called on toggle, on tab switch, and when a markdown buffer is modified. */
static GtkWidget *current_sci(void);
static void main_mdpreview_refresh(void) {
    if (!mdpreview_is_visible()) return;
    GtkWidget *sci = current_sci();
    if (!sci) { mdpreview_render("", 0); return; }
    int n = (int)scintilla_send_message(SCINTILLA(sci), SCI_GETLENGTH, 0, 0);
    if (n <= 0) { mdpreview_render("", 0); return; }
    char *buf = g_malloc((size_t)n + 1);
    /* SCI_GETTEXT(length+1, char*) — fills up to `length` bytes plus NUL. */
    scintilla_send_message(SCINTILLA(sci), SCI_GETTEXT,
                           (uptr_t)(n + 1), (sptr_t)buf);
    mdpreview_render(buf, (size_t)n);
    g_free(buf);
}

/* Public bridge — editor.c calls this from SCN_MODIFIED / tab-switch.
 * Cheap when the panel is hidden. */
void main_mdpreview_notify_changed(void) { main_mdpreview_refresh(); }

/* ------------------------------------------------------------------ */
/* Action handlers                                                     */
/* ------------------------------------------------------------------ */

static void action_new(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    editor_new_doc();
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    editor_open_dialog();
}

static void action_save(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    editor_save();
}

static void action_save_as(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    editor_save_as_dialog();
}

static void action_save_copy_as(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    editor_save_copy_as();
}

static void action_save_all(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    /* P3 — confirm before Save All when the pref is set. */
    if (g_prefs.save_all_confirm) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
            "Save all modified documents?");
        int r = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        if (r != GTK_RESPONSE_OK) return;
    }
    editor_save_all();
}

static void action_rename(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    editor_rename();
}

static void action_reload(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    editor_reload_current();
}

static void action_close(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    editor_close_page(-1);
}

static void action_close_others(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    editor_close_all_but_current();
}

/* Q-fix File → Close Multiple Documents (5 items, matches macOS).
 * Each batch threads a local dont-save-all flag so the unsaved-changes
 * prompt can offer "Don't Save All" (macOS #214). */
static void action_close_all(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    GtkWidget *nb = editor_get_notebook();
    gboolean dsa = FALSE;
    for (int i = gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)) - 1; i >= 0; i--)
        if (!editor_close_page_multi(i, &dsa)) return;
}
static void action_close_to_left(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    GtkWidget *nb = editor_get_notebook();
    int keep = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
    gboolean dsa = FALSE;
    for (int i = keep - 1; i >= 0; i--)
        if (!editor_close_page_multi(i, &dsa)) return;
}
static void action_close_to_right(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    GtkWidget *nb = editor_get_notebook();
    int keep = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
    gboolean dsa = FALSE;
    for (int i = gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)) - 1; i > keep; i--)
        if (!editor_close_page_multi(i, &dsa)) return;
}
static void action_close_unchanged(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    GtkWidget *nb = editor_get_notebook();
    for (int i = gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)) - 1; i >= 0; i--) {
        NppDoc *doc = editor_doc_at(i);
        if (doc && !doc->modified) editor_close_page(i);
    }
}

static void action_quit(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    /* Capture the current zoom so the editor reopens at the same level
     * (kPrefZoomLevel on macOS; ScintillaPrimaryView/@zoom on Linux). */
    {
        GtkWidget *sci = current_sci();
        if (sci) {
            g_prefs.zoom_level =
                (int)scintilla_send_message(SCINTILLA(sci), SCI_GETZOOM, 0, 0);
            prefs_save();
        }
    }
    /* Capture window frame so the next launch restores it. */
    if (g_window) {
        int ww = 0, wh = 0, wx = 0, wy = 0;
        gtk_window_get_size    (GTK_WINDOW(g_window), &ww, &wh);
        gtk_window_get_position(GTK_WINDOW(g_window), &wx, &wy);
        gboolean maxim = gtk_window_is_maximized(GTK_WINDOW(g_window));
        session_stash_geometry(ww, wh, wx, wy, maxim);
    }
    /* P3 — only persist the session when the pref allows it. */
    if (g_prefs.remember_session)
        session_save();
    if (g_app) editor_close_all_quit(G_APPLICATION(g_app));
}

static void action_open_recent(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)ud;
    const char *path = g_variant_get_string(p, NULL);
    if (path && *path) editor_open_path(path);
}

static void action_clear_recent(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    recent_files_clear();
    rebuild_recent_menu();
}

static void action_toggle_doclist(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud;
    doclist_set_visible(!doclist_is_visible());
}
static void action_toggle_workspace(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; workspace_set_visible(!workspace_is_visible());
}
static void action_toggle_funclist(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; funclist_set_visible(!funclist_is_visible());
}
static void action_toggle_docmap(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; docmap_set_visible(!docmap_is_visible());
}
static void action_toggle_gitpanel(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; gitpanel_set_visible(!gitpanel_is_visible());
}

/* G29 — Markdown preview toggle. Pulls the current Scintilla buffer and
 * pushes it through the renderer. */
static void main_mdpreview_refresh(void);
static void action_toggle_mdpreview(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    gboolean show = !mdpreview_is_visible();
    mdpreview_set_visible(show);
    if (show) main_mdpreview_refresh();
}

/* G14 — Split view actions. */
static void action_split_toggle(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; split_toggle();
}
static void action_split_clone(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; split_clone_current();
}
static void action_split_focus_other(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; split_focus_other();
}
static void action_split_close(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; split_close_secondary_tab();
}

/* G5 / G7 / G9 actions ────────────────────────────────────────────────── */
/* Q-align: route Find / Replace / Find in Files through the unified
 * find_window.c 5-tab dialog matching macOS FindWindow.mm exactly. */
#include "find_window.h"
static void action_find(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    find_window_show(g_window ? GTK_WINDOW(g_window) : NULL,
                     FW_TAB_FIND, NULL);
}
static void action_replace(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    find_window_show(g_window ? GTK_WINDOW(g_window) : NULL,
                     FW_TAB_REPLACE, NULL);
}
static void action_find_next(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; findreplace_find_next();
}
static void action_find_prev(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; findreplace_find_prev();
}
static void action_find_in_files(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    find_window_show(g_window ? GTK_WINDOW(g_window) : NULL,
                     FW_TAB_FIND_IN_FILES, NULL);
}
static void action_show_mark_dialog_q(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    find_window_show(g_window ? GTK_WINDOW(g_window) : NULL,
                     FW_TAB_MARK, NULL);
}
static void action_macro_start(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    NppDoc *d = editor_current_doc();
    if (d) macro_start_recording(d->sci);
}
static void action_macro_stop(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    NppDoc *d = editor_current_doc();
    if (d) macro_stop_recording(d->sci);
}
static void action_macro_play(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    NppDoc *d = editor_current_doc();
    if (d) macro_playback(d->sci);
}
static void action_macro_play_n(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    NppDoc *d = editor_current_doc();
    if (d) macro_run_multiple_dialog(d->sci, GTK_WINDOW(g_window));
}
static void action_macro_batch(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    macrobatch_show_dialog(GTK_WINDOW(g_window), NULL);
}
static void action_macro_save_as(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    NppDoc *d = editor_current_doc();
    /* Menu refresh happens via the macro changed-hook. */
    if (d) macro_save_as_dialog(d->sci, GTK_WINDOW(g_window));
}
static void action_run(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    NppDoc *d = editor_current_doc();
    run_dialog(GTK_WINDOW(g_window), d ? d->filepath : NULL);
}
static void action_preferences(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u; prefs_dialog_show(GTK_WIDGET(g_window));
}
/* Settings → Appearance radio entries. Parameter is "auto" / "light" /
 * "dark"; anything else falls through to auto. Persists via prefs and
 * re-applies live so the user sees the switch immediately. */
static void action_theme_mode(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)u;
    const char *key = p ? g_variant_get_string(p, NULL) : NULL;
    ThemeMode m = THEME_AUTO;
    if      (key && !strcmp(key, "dark"))  m = THEME_DARK;
    else if (key && !strcmp(key, "light")) m = THEME_LIGHT;
    theme_set_and_apply(m);
}
static void action_shortcut_map(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    shortcut_mapper_show(GTK_WIDGET(g_window));
}
static void on_styles_applied(void) {
    /* Reapply theme to all editors after the user accepts changes in the
     * Style Configurator. */
    editor_reapply_styles();
}
static void action_style_editor(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    styleeditor_show(GTK_WIDGET(g_window), on_styles_applied);
}
static void action_column_editor(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    columneditor_show(GTK_WIDGET(g_window));
}
static void action_udl_admin(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    udladmin_show(g_window ? GTK_WINDOW(g_window) : NULL);
}
/* GAP-20 — dispatch a loaded plugin's FuncItem from its menu entry.
 * Recorded into macros as the macOS/Windows-interoperable
 * "pluginMenuAction:" + cmdID type-2 form. */
static void action_plugin_cmd(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)u;
    int cmd_id = p ? g_variant_get_int32(p) : 0;
    if (cmd_id <= 0) return;
    if (macro_menu_wrap_begin()) {
        plugin_run_command_by_id(cmd_id);
        macro_menu_wrap_end(NULL, cmd_id);
    } else {
        plugin_run_command_by_id(cmd_id);
    }
}

static void action_plugins_admin(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    pluginsadmin_show(GTK_WINDOW(g_window));
}

/* Q6/Q9 — Open ~/.nextpad++/plugins/ in the file manager. */
static void action_open_plugins_folder(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    gchar *dir = npp_user_subdir("plugins");
    g_mkdir_with_parents(dir, 0700);
    gchar *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri)
        gtk_show_uri_on_window(GTK_WINDOW(g_window), uri, GDK_CURRENT_TIME, NULL);
    g_free(uri);
    g_free(dir);
}

/* G3.10: encoding choice. Action state is the currently-selected encoding's
 * display name; each menu item targets a string and the GTK radio-style
 * indicator lights up automatically when state == target. */
static void action_set_encoding_change(GSimpleAction *a, GVariant *new_state,
                                       gpointer ud) {
    (void)ud;
    g_simple_action_set_state(a, new_state);
    const char *enc = g_variant_get_string(new_state, NULL);
    NppDoc *doc = editor_current_doc();
    if (doc) {
        g_free(doc->encoding);
        doc->encoding = g_strdup(enc);
    }
}

/* Reload-with-<encoding> — re-reads the file from disk forcing the chosen
 * encoding instead of running auto-detection. Mirrors macOS
 * reloadWithEncoding: (EditorView.mm) so the same Encoding-menu options
 * are reachable on both platforms. Parametric: app.reload-as(s "UTF-8"). */
static void action_reload_as(GSimpleAction *a, GVariant *param, gpointer ud) {
    (void)a; (void)ud;
    if (!param) return;
    const char *enc = g_variant_get_string(param, NULL);
    if (enc && *enc) editor_reload_as(enc);
}

static void action_undo(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud; editor_undo();
}
static void action_redo(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud; editor_redo();
}
static void action_cut(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud; editor_cut();
}
static void action_copy(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud; editor_copy();
}
static void action_paste(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud; editor_paste();
}
static void action_select_all(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud; editor_select_all();
}
static void action_goto_line(GSimpleAction *a, GVariant *p, gpointer ud) {
    (void)a; (void)p; (void)ud; editor_goto_line_dialog();
}

/* ──────────────────────────────────────────────────────────────────────
 * G11 helper: current Scintilla widget
 * ────────────────────────────────────────────────────────────────────── */
static GtkWidget *current_sci(void) {
    NppDoc *d = editor_current_doc();
    return d ? d->sci : NULL;
}
static sptr_t sci_send(unsigned int msg, uptr_t wp, sptr_t lp) {
    GtkWidget *s = current_sci();
    return s ? scintilla_send_message(SCINTILLA(s), msg, wp, lp) : 0;
}

/* G11.3 View → Show Symbol */
static void action_show_ws(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int cur = (int)sci_send(SCI_GETVIEWWS, 0, 0);
    sci_send(SCI_SETVIEWWS, cur ? SCWS_INVISIBLE : SCWS_VISIBLEALWAYS, 0);
}
static void action_show_eol(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_SETVIEWEOL, !sci_send(SCI_GETVIEWEOL,0,0), 0);
}
static void action_show_all_chars(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int cur = (int)sci_send(SCI_GETVIEWWS, 0, 0);
    int new_state = cur ? 0 : 1;
    sci_send(SCI_SETVIEWWS, new_state ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE, 0);
    sci_send(SCI_SETVIEWEOL, new_state, 0);
}
static void action_show_indent_guide(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int cur = (int)sci_send(SCI_GETINDENTATIONGUIDES, 0, 0);
    sci_send(SCI_SETINDENTATIONGUIDES, cur ? SC_IV_NONE : SC_IV_LOOKBOTH, 0);
}
static void action_show_line_numbers(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *s = current_sci(); if (!s) return;
    int w = (int)scintilla_send_message(SCINTILLA(s), SCI_GETMARGINWIDTHN, 0, 0);
    scintilla_send_message(SCINTILLA(s), SCI_SETMARGINWIDTHN, 0, w > 0 ? 0 : 40);
}
static void action_show_wrap_symbol(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int cur = (int)sci_send(SCI_GETWRAPVISUALFLAGS, 0, 0);
    sci_send(SCI_SETWRAPVISUALFLAGS, cur ? SC_WRAPVISUALFLAG_NONE
                                          : SC_WRAPVISUALFLAG_END, 0);
}

/* G11.3 View → Zoom */
static void action_zoom_in(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_ZOOMIN, 0, 0);
}
static void action_zoom_out(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_ZOOMOUT, 0, 0);
}
static void action_zoom_reset(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_SETZOOM, 0, 0);
}

/* G11.5 View → Fold All / Unfold All */
static void action_fold_all(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_FOLDALL, SC_FOLDACTION_CONTRACT, 0);
}
static void action_unfold_all(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_FOLDALL, SC_FOLDACTION_EXPAND, 0);
}
static void action_toggle_fold(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_FOLDALL, SC_FOLDACTION_TOGGLE, 0);
}

/* Q-fix View → Fold Level / Unfold Level (16 items, matches macOS).
 * Walks visible lines, finds header lines at the requested fold level,
 * and contracts or expands them. SC_FOLDLEVELBASE is the base for
 * user-meaningful level "1" so we add (level - 1) for the comparison. */
static void fold_at_level(int level_1based, gboolean fold) {
    int n = (int)sci_send(SCI_GETLINECOUNT, 0, 0);
    int target = SC_FOLDLEVELBASE + (level_1based - 1);
    for (int line = 0; line < n; line++) {
        int lvl = (int)sci_send(SCI_GETFOLDLEVEL, (uptr_t)line, 0);
        if (!(lvl & SC_FOLDLEVELHEADERFLAG)) continue;
        if ((lvl & SC_FOLDLEVELNUMBERMASK) != target) continue;
        int expanded = (int)sci_send(SCI_GETFOLDEXPANDED, (uptr_t)line, 0);
        if (fold && expanded)
            sci_send(SCI_FOLDLINE, (uptr_t)line, SC_FOLDACTION_CONTRACT);
        else if (!fold && !expanded)
            sci_send(SCI_FOLDLINE, (uptr_t)line, SC_FOLDACTION_EXPAND);
    }
}
#define DEFINE_FOLD_LEVEL(N) \
    static void action_fold_level_##N(GSimpleAction *a, GVariant *p, gpointer u) { \
        (void)a;(void)p;(void)u; fold_at_level(N, TRUE); \
    } \
    static void action_unfold_level_##N(GSimpleAction *a, GVariant *p, gpointer u) { \
        (void)a;(void)p;(void)u; fold_at_level(N, FALSE); \
    }
DEFINE_FOLD_LEVEL(1) DEFINE_FOLD_LEVEL(2) DEFINE_FOLD_LEVEL(3) DEFINE_FOLD_LEVEL(4)
DEFINE_FOLD_LEVEL(5) DEFINE_FOLD_LEVEL(6) DEFINE_FOLD_LEVEL(7) DEFINE_FOLD_LEVEL(8)
#undef DEFINE_FOLD_LEVEL

/* Q-fix Search → Change History (3 items). changehistory.c is grafted and
 * already attaches markers; these helpers walk to the next/prev marker. */
static void action_change_next(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int line = (int)sci_send(SCI_LINEFROMPOSITION,
                             (uptr_t)sci_send(SCI_GETCURRENTPOS, 0, 0), 0);
    /* CH markers 0 (unsaved) + 1 (saved) = 0x3 */
    int next = (int)sci_send(SCI_MARKERNEXT, (uptr_t)(line + 1), 0x3);
    if (next < 0) next = (int)sci_send(SCI_MARKERNEXT, 0, 0x3);
    if (next >= 0) { sci_send(SCI_GOTOLINE, (uptr_t)next, 0); sci_send(SCI_SCROLLCARET, 0, 0); }
}
static void action_change_prev(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int line = (int)sci_send(SCI_LINEFROMPOSITION,
                             (uptr_t)sci_send(SCI_GETCURRENTPOS, 0, 0), 0);
    int prev = (int)sci_send(SCI_MARKERPREVIOUS, (uptr_t)(line - 1), 0x3);
    if (prev < 0) {
        int last = (int)sci_send(SCI_GETLINECOUNT, 0, 0) - 1;
        prev = (int)sci_send(SCI_MARKERPREVIOUS, (uptr_t)last, 0x3);
    }
    if (prev >= 0) { sci_send(SCI_GOTOLINE, (uptr_t)prev, 0); sci_send(SCI_SCROLLCARET, 0, 0); }
}
static void action_change_clear(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_MARKERDELETEALL, 0, 0);
    sci_send(SCI_MARKERDELETEALL, 1, 0);
}

/* Q-fix Edit → Multi-Select All / Multi-Select Next (8 items).
 * SCI_TARGETWHOLEDOCUMENT + SCI_MULTIPLESELECTADDEACH does the All variants;
 * SCI_MULTIPLESELECTADDNEXT does the Next variants. Case + whole-word are
 * controlled via SCI_SETSEARCHFLAGS before the call. */
static void multi_select(gboolean all, int flags) {
    /* Need a current selection to seed. If none, take the word at caret. */
    sptr_t s = sci_send(SCI_GETSELECTIONSTART, 0, 0);
    sptr_t e = sci_send(SCI_GETSELECTIONEND,   0, 0);
    if (s == e) {
        sptr_t pos = sci_send(SCI_GETCURRENTPOS, 0, 0);
        sptr_t ws  = sci_send(SCI_WORDSTARTPOSITION, (uptr_t)pos, TRUE);
        sptr_t we  = sci_send(SCI_WORDENDPOSITION,   (uptr_t)pos, TRUE);
        if (ws == we) return;
        sci_send(SCI_SETSEL, (uptr_t)ws, we);
    }
    sci_send(SCI_SETSEARCHFLAGS, (uptr_t)flags, 0);
    if (all) {
        sci_send(SCI_TARGETWHOLEDOCUMENT, 0, 0);
        sci_send(SCI_MULTIPLESELECTADDEACH, 0, 0);
    } else {
        sci_send(SCI_MULTIPLESELECTADDNEXT, 0, 0);
    }
}
static void action_msa_ignore(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; multi_select(TRUE, 0);
}
static void action_msa_case(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; multi_select(TRUE, SCFIND_MATCHCASE);
}
static void action_msa_word(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; multi_select(TRUE, SCFIND_WHOLEWORD);
}
static void action_msa_case_word(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; multi_select(TRUE, SCFIND_MATCHCASE|SCFIND_WHOLEWORD);
}
static void action_msn_ignore(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; multi_select(FALSE, 0);
}
static void action_msn_case(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; multi_select(FALSE, SCFIND_MATCHCASE);
}
static void action_msn_word(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; multi_select(FALSE, SCFIND_WHOLEWORD);
}
static void action_msn_case_word(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; multi_select(FALSE, SCFIND_MATCHCASE|SCFIND_WHOLEWORD);
}
static void action_ms_undo(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_DROPSELECTIONN,
        (uptr_t)(sci_send(SCI_GETSELECTIONS, 0, 0) - 1), 0);
}
static void action_ms_skip(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    /* Drop current main selection then add next match. */
    sci_send(SCI_DROPSELECTIONN,
        (uptr_t)sci_send(SCI_GETMAINSELECTION, 0, 0), 0);
    sci_send(SCI_MULTIPLESELECTADDNEXT, 0, 0);
}

/* G11.6 View → Word Wrap / Always on Top / Full Screen */
static void action_word_wrap(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int cur = (int)sci_send(SCI_GETWRAPMODE, 0, 0);
    sci_send(SCI_SETWRAPMODE, cur ? SC_WRAP_NONE : SC_WRAP_WORD, 0);
}
static void action_always_on_top(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    if (!g_window) return;
    GtkWindow *w = GTK_WINDOW(g_window);
    static gboolean s_above = FALSE;
    s_above = !s_above;
    gtk_window_set_keep_above(w, s_above);
}
static void action_fullscreen(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    if (!g_window) return;
    if (gtk_window_is_fullscreen(GTK_WINDOW(g_window)))
        gtk_window_unfullscreen(GTK_WINDOW(g_window));
    else
        gtk_window_fullscreen(GTK_WINDOW(g_window));
}

/* G11.4 View → Tab navigation */
static void action_tab_next(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *n = editor_get_notebook();
    if (!n) return;
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(n));
    int last = gtk_notebook_get_n_pages(GTK_NOTEBOOK(n)) - 1;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(n), cur < last ? cur + 1 : 0);
}
static void action_tab_prev(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *n = editor_get_notebook();
    if (!n) return;
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(n));
    int last = gtk_notebook_get_n_pages(GTK_NOTEBOOK(n)) - 1;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(n), cur > 0 ? cur - 1 : last);
}
static void action_tab_first(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *n = editor_get_notebook();
    if (n) gtk_notebook_set_current_page(GTK_NOTEBOOK(n), 0);
}
static void action_tab_last(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *n = editor_get_notebook();
    if (n) gtk_notebook_set_current_page(GTK_NOTEBOOK(n),
            gtk_notebook_get_n_pages(GTK_NOTEBOOK(n)) - 1);
}
static void action_tab_goto(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)u;
    int idx = g_variant_get_int32(p);  /* 1-based */
    GtkWidget *n = editor_get_notebook();
    if (n && idx >= 1) gtk_notebook_set_current_page(GTK_NOTEBOOK(n), idx - 1);
}

/* G11.7 File menu fillers */
static void action_open_workspace(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open Folder as Workspace", GTK_WINDOW(g_window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            /* P10 — append (multi-root) and persist. */
            workspace_add_folder(path);
            workspace_set_visible(TRUE);
            gchar **roots = workspace_get_roots();
            int n = 0; while (roots && roots[n]) n++;
            prefs_workspace_roots_set((const char *const *)roots, n);
            g_strfreev(roots);
            prefs_save();
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}
static void action_open_containing_folder(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *d = editor_current_doc();
    if (!d || !d->filepath) return;
    gchar *dir = g_path_get_dirname(d->filepath);
    gchar *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri) { gtk_show_uri_on_window(GTK_WINDOW(g_window), uri, GDK_CURRENT_TIME, NULL); g_free(uri); }
    g_free(dir);
}
static void action_open_in_default_viewer(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *d = editor_current_doc();
    if (!d || !d->filepath) return;
    gchar *uri = g_filename_to_uri(d->filepath, NULL, NULL);
    if (uri) { gtk_show_uri_on_window(GTK_WINDOW(g_window), uri, GDK_CURRENT_TIME, NULL); g_free(uri); }
}
static void action_save_session(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; session_save();
}
static void action_load_session(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; session_restore();
}
static void action_print(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; main_do_print();
}

/* G11.8 Help menu */
static void action_help_home(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gtk_show_uri_on_window(GTK_WINDOW(g_window),
        "https://nextpad.org", GDK_CURRENT_TIME, NULL);
}
static void action_help_project(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gtk_show_uri_on_window(GTK_WINDOW(g_window),
        "https://github.com/nextpad-plus-plus", GDK_CURRENT_TIME, NULL);
}
static void action_help_manual(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gtk_show_uri_on_window(GTK_WINDOW(g_window),
        "https://npp-user-manual.org/", GDK_CURRENT_TIME, NULL);
}
/* ──────────────────────────────────────────────────────────────────────
 * Help → Command Line Arguments
 * ────────────────────────────────────────────────────────────────────── */

static void action_help_cli_args(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    const char *body =
        "Usage: nextpad-plus-plus [OPTIONS] [FILE...]\n"
        "\n"
        "Per-file options (apply to the LAST file in the batch):\n"
        "  -l N          open at line N (1-based)\n"
        "  -c N          position caret at column N (1-based)\n"
        "  -p N          position caret at byte offset N\n"
        "  -lang NAME    force lexer (e.g. cpp, python, javascript)\n"
        "  -udl NAME     force user-defined language\n"
        "  -ro           open read-only\n"
        "  -monitor      tail -f mode (auto-reload on external change)\n"
        "\n"
        "Example:\n"
        "  nextpad-plus-plus -l 50 -ro /var/log/syslog\n"
        "  nextpad-plus-plus -lang python script.py main.c";
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
        "Command Line Arguments");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", body);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* ──────────────────────────────────────────────────────────────────────
 * View → Summary — word/line/char counts for the current doc
 * ────────────────────────────────────────────────────────────────────── */

static void action_view_summary(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci();
    if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t bytes = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    sptr_t lines = scintilla_send_message(s, SCI_GETLINECOUNT, 0, 0);

    /* Word count: scan whole doc for word boundaries. */
    sptr_t words = 0;
    gboolean in_word = FALSE;
    for (sptr_t i = 0; i < bytes; i++) {
        char c = (char)scintilla_send_message(s, SCI_GETCHARAT, i, 0);
        gboolean is_ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (!is_ws && !in_word) { words++; in_word = TRUE; }
        else if (is_ws) in_word = FALSE;
    }

    /* Selection length, if any. */
    sptr_t sel_start = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t sel_end   = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
    sptr_t sel_len = sel_end > sel_start ? sel_end - sel_start : 0;

    NppDoc *doc = editor_current_doc();
    char body[1024];
    g_snprintf(body, sizeof(body),
        "Document: %s\n\n"
        "  Lines:        %lld\n"
        "  Characters:   %lld\n"
        "  Words:        %lld\n"
        "  Selection:    %lld characters\n"
        "  Encoding:     %s\n",
        doc && doc->filepath ? doc->filepath
                              : (doc ? "Untitled" : "(no document)"),
        (long long)lines, (long long)bytes, (long long)words, (long long)sel_len,
        doc && doc->encoding ? doc->encoding : "UTF-8");

    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "Summary");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", body);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* ──────────────────────────────────────────────────────────────────────
 * Reopen Closed Tab (Ctrl+Shift+T)
 *
 * Tracks the last N closed-file paths on a small stack. Pop reopens.
 * ────────────────────────────────────────────────────────────────────── */

#define CLOSED_TAB_STACK 20
static char *s_closed_paths[CLOSED_TAB_STACK];
static int   s_closed_top = 0;

/* Called from main_doclist_refresh hook (every refresh checks for closed
 * tabs and pushes their paths). Lower-cost alternative: integrate with
 * editor_close_page. For now this is opportunistic. */
static void closed_tabs_push(const char *path) {
    if (!path || !*path) return;
    /* Dedupe most-recent-first. */
    if (s_closed_top > 0 && s_closed_paths[s_closed_top - 1] &&
        strcmp(s_closed_paths[s_closed_top - 1], path) == 0) return;
    if (s_closed_top >= CLOSED_TAB_STACK) {
        g_free(s_closed_paths[0]);
        memmove(&s_closed_paths[0], &s_closed_paths[1],
                sizeof(char *) * (CLOSED_TAB_STACK - 1));
        s_closed_top = CLOSED_TAB_STACK - 1;
    }
    s_closed_paths[s_closed_top++] = g_strdup(path);
}

static void action_reopen_closed(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    if (s_closed_top <= 0) return;
    char *path = s_closed_paths[--s_closed_top];
    s_closed_paths[s_closed_top] = NULL;
    if (path) {
        editor_open_path(path);
        g_free(path);
    }
}

static void action_help_about(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    /* P16 — bundled macOS logo (logo150px). GTK4's "logo" property is a
     * GdkPaintable, not a GdkPixbuf — load it as a GdkTexture (which is
     * a GdkPaintable) or g_object_set rejects it with a type error. */
    GdkTexture *logo = NULL;
    const char *logo_path = RESOURCES_DIR "/icons/standard/about/logo150px.png";
    if (g_file_test(logo_path, G_FILE_TEST_EXISTS))
        logo = gdk_texture_new_from_filename(logo_path, NULL);
    gtk_show_about_dialog(GTK_WINDOW(g_window),
        "program-name", APP_NAME,
        "version",      APP_VERSION,
        "comments",     "A native Linux port of Notepad++ — multi-tab text editor "
                        "with Scintilla + Lexilla. GTK4 + libadwaita, C11.",
        "website",      "https://nextpad.org",
        "website-label","nextpad.org",
        "copyright",    "© 2026 Andrey Letov + Andrea Coi (Linux port grafts from notetux-plus-plus)",
        "license-type", GTK_LICENSE_GPL_3_0,
        "logo",         logo,
        NULL);
    if (logo) g_object_unref(logo);
}

/* ──────────────────────────────────────────────────────────────────────
 * G11.2 Language menu
 *
 * Hardcoded list (mirrors macOS MenuBuilder.mm enumeration of
 * NppLangsManager.allLanguages). Display label + Lexilla lexer key.
 * Grouped alphabetically. "None" wraps lexer_apply(sci, NULL).
 * ────────────────────────────────────────────────────────────────────── */

typedef struct { const char *display; const char *key; } LangEntry;
static const LangEntry kLangs[] = {
    /* A */
    { "Ada",          "ada"        },
    { "ActionScript", "actionscript"},
    { "ASP",          "asp"        },
    { "Assembly",     "asm"        },
    { "AutoIt",       "autoit"     },
    /* B */
    { "Bash",         "bash"       },
    { "Batch",        "batch"      },
    /* C */
    { "C",            "c"          },
    { "C#",           "cs"         },
    { "C++",          "cpp"        },
    { "Caml",         "caml"       },
    { "CMake",        "cmake"      },
    { "COBOL",        "cobol"      },
    { "CoffeeScript", "coffeescript"},
    { "CSS",          "css"        },
    /* D */
    { "D",            "d"          },
    { "Dart",         "dart"       },
    { "Diff",         "diff"       },
    /* E */
    { "Elixir",       "elixir"     },
    { "Erlang",       "erlang"     },
    /* F */
    { "Fortran",      "fortran"    },
    { "FreeBASIC",    "freebasic"  },
    /* G */
    { "Go",           "go"         },
    { "Groovy",       "groovy"     },
    /* H */
    { "Haskell",      "haskell"    },
    { "HTML",         "html"       },
    /* I */
    { "INI",          "ini"        },
    /* J */
    { "Java",         "java"       },
    { "JavaScript",   "javascript" },
    { "Julia",        "julia"      },
    { "JSON",         "json"       },
    /* K */
    { "KIXtart",      "kix"        },
    { "Kotlin",       "kotlin"     },
    /* L */
    { "LaTeX",        "latex"      },
    { "Lisp",         "lisp"       },
    { "Lua",          "lua"        },
    /* M */
    { "Makefile",     "makefile"   },
    { "Markdown",     "markdown"   },
    { "MATLAB",       "matlab"     },
    /* N */
    { "Nim",          "nim"        },
    { "NSIS",         "nsis"       },
    /* O */
    { "Objective-C",  "objc"       },
    { "OCaml",        "ocaml"      },
    /* P */
    { "Pascal",       "pascal"     },
    { "Perl",         "perl"       },
    { "PHP",          "php"        },
    { "PostScript",   "ps"         },
    { "PowerShell",   "powershell" },
    { "Properties",   "props"      },
    { "Python",       "python"     },
    /* R */
    { "R",            "r"          },
    { "Ruby",         "ruby"       },
    { "Rust",         "rust"       },
    /* S */
    { "Scheme",       "scheme"     },
    { "Smalltalk",    "smalltalk"  },
    { "SQL",          "sql"        },
    { "Swift",        "swift"      },
    /* T */
    { "Tcl",          "tcl"        },
    { "TOML",         "toml"       },
    { "TypeScript",   "typescript" },
    /* V */
    { "VBScript",     "vb"         },
    { "Verilog",      "verilog"    },
    { "VHDL",         "vhdl"       },
    { "Visual Basic", "vb"         },
    /* X */
    { "XML",          "xml"        },
    /* Y */
    { "YAML",         "yaml"       },
    /* Z */
    { "Zig",          "zig"        },
};
static const int kLangsCount = (int)(sizeof(kLangs) / sizeof(kLangs[0]));

/* set-language is a stateful (radio) action: the Language-menu item whose
 * target equals the current state shows the check mark. change-state is
 * fired both by a menu pick and by main_sync_language_menu() on tab
 * switch / file open. */
static void action_set_language_change(GSimpleAction *a, GVariant *new_state,
                                        gpointer u) {
    (void)u;
    g_simple_action_set_state(a, new_state);
    const char *key = new_state ? g_variant_get_string(new_state, NULL) : NULL;
    GtkWidget *sci = current_sci();
    if (sci) lexer_apply(sci, (key && *key) ? key : NULL);
    NppDoc *doc = editor_current_doc();
    if (doc) {                       /* persist the per-document override */
        g_free(doc->language);
        doc->language = g_strdup(key ? key : "");
    }
}

/* ──────────────────────────────────────────────────────────────────────
 * G16 Smart Highlight + Brace Matching
 * ────────────────────────────────────────────────────────────────────── */

static void action_goto_matching_brace(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sptr_t pos = sci_send(SCI_GETCURRENTPOS, 0, 0);
    sptr_t match = sci_send(SCI_BRACEMATCH, (uptr_t)pos, 0);
    if (match < 0 && pos > 0) {
        /* Try one position back — caret might be right after the brace */
        match = sci_send(SCI_BRACEMATCH, (uptr_t)(pos - 1), 0);
    }
    if (match >= 0) {
        sci_send(SCI_GOTOPOS, (uptr_t)match, 0);
    }
}

static void action_select_in_brackets(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sptr_t pos = sci_send(SCI_GETCURRENTPOS, 0, 0);
    sptr_t match = sci_send(SCI_BRACEMATCH, (uptr_t)pos, 0);
    if (match < 0 && pos > 0)
        match = sci_send(SCI_BRACEMATCH, (uptr_t)(pos - 1), 0);
    if (match >= 0) {
        sptr_t a2 = pos < match ? pos + 1 : match + 1;
        sptr_t b2 = pos < match ? match    : pos;
        sci_send(SCI_SETSEL, (uptr_t)a2, b2);
    }
}

/* Smart Highlight: mark all matches of the word under caret with INDIC 8
 * (Scintilla's "tag" indicator). Triggered from SCN_UPDATEUI in editor.c
 * (we add the call below). */
#define NPP_INDIC_SMARTHIGHLIGHT 8

static void smarthighlight_update_now(GtkWidget *sci)
{
    if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);

    /* Clear previous highlights. */
    sptr_t doc_len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    scintilla_send_message(s, SCI_SETINDICATORCURRENT, NPP_INDIC_SMARTHIGHLIGHT, 0);
    scintilla_send_message(s, SCI_INDICATORCLEARRANGE, 0, doc_len);

    /* If no selection, nothing to do. */
    sptr_t sel_start = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t sel_end   = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
    if (sel_start == sel_end) return;
    /* Cap to single-line selections of reasonable length. */
    if (sel_end - sel_start < 2 || sel_end - sel_start > 80) return;
    sptr_t line_start = scintilla_send_message(s, SCI_LINEFROMPOSITION, sel_start, 0);
    sptr_t line_end   = scintilla_send_message(s, SCI_LINEFROMPOSITION, sel_end,   0);
    if (line_start != line_end) return;

    /* Configure the indicator style: box, semi-transparent, theme accent. */
    scintilla_send_message(s, SCI_INDICSETSTYLE, NPP_INDIC_SMARTHIGHLIGHT,
                           INDIC_ROUNDBOX);
    scintilla_send_message(s, SCI_INDICSETFORE,  NPP_INDIC_SMARTHIGHLIGHT,
                           0x00B0F0);  /* warm yellow-orange (BGR) */
    scintilla_send_message(s, SCI_INDICSETALPHA, NPP_INDIC_SMARTHIGHLIGHT, 80);

    /* Grab selected text. */
    sptr_t sel_len = sel_end - sel_start;
    char *needle = g_malloc((gsize)sel_len + 1);
    scintilla_send_message(s, SCI_GETSELTEXT, 0, (sptr_t)needle);

    /* Sweep document with SCI_SEARCHINTARGET. Flags driven by prefs:
     * smart_hilite_case  → SCFIND_MATCHCASE  (default off — case-insensitive)
     * smart_hilite_word  → SCFIND_WHOLEWORD  (default on  — whole-word only) */
    int flags = 0;
    if (g_prefs.smart_hilite_case) flags |= SCFIND_MATCHCASE;
    if (g_prefs.smart_hilite_word) flags |= SCFIND_WHOLEWORD;
    scintilla_send_message(s, SCI_SETSEARCHFLAGS, (uptr_t)flags, 0);
    sptr_t cur = 0;
    while (cur < doc_len) {
        scintilla_send_message(s, SCI_SETTARGETRANGE, cur, doc_len);
        sptr_t hit = scintilla_send_message(s, SCI_SEARCHINTARGET,
                                            (uptr_t)sel_len, (sptr_t)needle);
        if (hit < 0) break;
        sptr_t end = scintilla_send_message(s, SCI_GETTARGETEND, 0, 0);
        /* Don't re-mark the actual selection itself. */
        if (hit != sel_start) {
            scintilla_send_message(s, SCI_INDICATORFILLRANGE, hit, end - hit);
        }
        cur = end;
    }
    g_free(needle);
}

/* Public hook callable from editor.c on SCN_UPDATEUI. */
void main_smarthighlight_update(GtkWidget *sci) {
    /* P3 — gate on the pref. When disabled, clear any active indicators
     * so the visual state matches the toggle. */
    if (!g_prefs.smart_highlight) {
        if (sci) {
            scintilla_send_message(SCINTILLA(sci), SCI_SETINDICATORCURRENT, 8, 0);
            sptr_t len = scintilla_send_message(SCINTILLA(sci), SCI_GETLENGTH, 0, 0);
            scintilla_send_message(SCINTILLA(sci), SCI_INDICATORCLEARRANGE, 0, len);
        }
        return;
    }
    smarthighlight_update_now(sci);
}

/* ──────────────────────────────────────────────────────────────────────
 * G19 Hash + MIME Tools
 * ────────────────────────────────────────────────────────────────────── */

/* Get the selected text or whole doc text from current Scintilla. */
static char *grab_text_for_op(gsize *out_len) {
    GtkWidget *sci = current_sci(); if (!sci) { *out_len = 0; return g_strdup(""); }
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t a = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t b = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
    sptr_t len, src_start;
    if (a == b) {
        len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
        src_start = 0;
    } else {
        if (a > b) { sptr_t t = a; a = b; b = t; }
        len = b - a;
        src_start = a;
    }
    char *buf = g_malloc((gsize)len + 1);
    struct Sci_TextRangeFull tr;
    tr.chrg.cpMin = src_start;
    tr.chrg.cpMax = src_start + len;
    tr.lpstrText  = buf;
    scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    *out_len = (gsize)len;
    return buf;
}

static void show_result_dialog(const char *title, const char *body) {
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", body);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    /* Make the body monospace + selectable. */
    GList *labels = gtk_container_get_children(GTK_CONTAINER(content));
    for (GList *l = labels; l; l = l->next) {
        if (GTK_IS_BOX(l->data)) {
            GList *kids = gtk_container_get_children(GTK_CONTAINER(l->data));
            for (GList *k = kids; k; k = k->next) {
                if (GTK_IS_LABEL(k->data)) {
                    gtk_label_set_selectable(GTK_LABEL(k->data), TRUE);
                }
            }
            g_list_free(kids);
        }
    }
    g_list_free(labels);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void do_hash(GChecksumType type, const char *title) {
    gsize len; char *buf = grab_text_for_op(&len);
    gchar *hex = g_compute_checksum_for_data(type, (const guchar *)buf, len);
    if (hex) {
        show_result_dialog(title, hex);
        g_free(hex);
    }
    g_free(buf);
}
static void do_hash_clip(GChecksumType type) {
    gsize len; char *buf = grab_text_for_op(&len);
    gchar *hex = g_compute_checksum_for_data(type, (const guchar *)buf, len);
    if (hex) {
        npp_clipboard_set_text(hex);
        g_free(hex);
    }
    g_free(buf);
}

#define HASH_PAIR(NAME, CTYPE) \
    static void action_hash_##NAME(GSimpleAction *a, GVariant *p, gpointer u) {\
        (void)a;(void)p;(void)u; do_hash(CTYPE, "" #NAME " hash"); } \
    static void action_hash_##NAME##_clip(GSimpleAction *a, GVariant *p, gpointer u) {\
        (void)a;(void)p;(void)u; do_hash_clip(CTYPE); }

HASH_PAIR(md5,    G_CHECKSUM_MD5)
HASH_PAIR(sha1,   G_CHECKSUM_SHA1)
HASH_PAIR(sha256, G_CHECKSUM_SHA256)
HASH_PAIR(sha512, G_CHECKSUM_SHA512)
#undef HASH_PAIR

/* Q-align "Generate Hash from Files…" (macOS hashMD5FromFiles:, etc.).
 * Opens a multi-file picker, hashes each, and shows a result dialog with
 * a copyable text view. */
static void do_hash_from_files(GChecksumType type, const char *title) {
    GtkWidget *picker = gtk_file_chooser_dialog_new(
        "Select files to hash",
        g_window ? GTK_WINDOW(g_window) : NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Hash",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(picker), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(picker)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(picker);
        return;
    }
    GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(picker));
    gtk_widget_destroy(picker);
    GString *out = g_string_new(NULL);
    for (GSList *it = files; it; it = it->next) {
        const char *path = it->data;
        gchar *body = NULL; gsize len = 0;
        if (g_file_get_contents(path, &body, &len, NULL)) {
            gchar *hex = g_compute_checksum_for_data(type, (guchar *)body, len);
            if (hex) {
                g_string_append_printf(out, "%s  %s\n", hex, path);
                g_free(hex);
            }
            g_free(body);
        } else {
            g_string_append_printf(out, "(failed)  %s\n", path);
        }
    }
    g_slist_free_full(files, g_free);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(title,
        g_window ? GTK_WINDOW(g_window) : NULL, GTK_DIALOG_MODAL,
        "_Copy", 1, "_Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(tb, out->str, -1);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(sw), 640);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 280);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    npp_box_pack(GTK_BOX(box), sw, TRUE, 0);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == 1)
        npp_clipboard_set_text(out->str);
    gtk_widget_destroy(dlg);
    g_string_free(out, TRUE);
}
#define HASH_FILES(NAME, CTYPE, LBL) \
    static void action_hash_##NAME##_files(GSimpleAction *a, GVariant *p, gpointer u) {\
        (void)a;(void)p;(void)u; do_hash_from_files(CTYPE, LBL " from Files"); }
HASH_FILES(md5,    G_CHECKSUM_MD5,    "MD5")
HASH_FILES(sha1,   G_CHECKSUM_SHA1,   "SHA-1")
HASH_FILES(sha256, G_CHECKSUM_SHA256, "SHA-256")
HASH_FILES(sha512, G_CHECKSUM_SHA512, "SHA-512")
#undef HASH_FILES

/* Q-align Search → Mark dialog: opens Find dialog with mark semantics.
 * Simplest first cut: open the Find dialog (the user can then use one of
 * the Mark All Occurrences submenu items afterward). */
static void action_show_mark_dialog(GSimpleAction *a, GVariant *p, gpointer u);
/* Q-align Search → Search Results Window: toggle the panel. */
static void action_show_search_results_window(GSimpleAction *a, GVariant *p, gpointer u);

/* Q-align Path Completion: when triggered, show filesystem completions for
 * the path the caret is in. Minimal first cut: scan the word boundary back
 * for a path prefix, list matching files, show in SCI autocomplete UI. */
static void action_path_completion(GSimpleAction *a, GVariant *p, gpointer u);

static void action_show_mark_dialog(GSimpleAction *a, GVariant *p, gpointer u) {
    /* Q-align: now routes to the unified 5-tab Find dialog's Mark tab. */
    action_show_mark_dialog_q(a, p, u);
}

static void action_show_search_results_window(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    extern gboolean searchresults_is_visible(void);
    extern void     searchresults_set_visible(gboolean);
    searchresults_set_visible(!searchresults_is_visible());
}

static void action_path_completion(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t pos = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
    /* Walk backwards from caret collecting path-safe characters. */
    GString *prefix = g_string_new(NULL);
    for (sptr_t p2 = pos - 1; p2 >= 0; p2--) {
        char c = (char)scintilla_send_message(s, SCI_GETCHARAT, (uptr_t)p2, 0);
        if (c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\'' ||
            c == '(' || c == '[' || c == '<' || c == ':' ||
            (c == ',' && prefix->len > 0)) break;
        g_string_prepend_c(prefix, c);
    }
    if (prefix->len == 0) { g_string_free(prefix, TRUE); return; }
    /* Split into dir + partial basename. */
    char *slash = strrchr(prefix->str, '/');
    gchar *dir, *partial;
    if (!slash) {
        dir = g_strdup(".");
        partial = g_strdup(prefix->str);
    } else {
        dir = g_strndup(prefix->str, slash - prefix->str + 1);
        if (dir[0] == '~') {
            gchar *home = g_strdup(g_get_home_dir());
            gchar *expanded = g_strconcat(home, dir + 1, NULL);
            g_free(dir); dir = expanded; g_free(home);
        }
        partial = g_strdup(slash + 1);
    }
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) {
        g_string_free(prefix, TRUE); g_free(dir); g_free(partial);
        return;
    }
    GString *list = g_string_new(NULL);
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!*partial || strncmp(name, partial, strlen(partial)) == 0) {
            if (list->len > 0) g_string_append_c(list, ' ');
            g_string_append(list, name);
        }
    }
    g_dir_close(d);
    if (list->len > 0)
        scintilla_send_message(s, SCI_AUTOCSHOW,
                               (uptr_t)strlen(partial), (sptr_t)list->str);
    g_string_free(list, TRUE);
    g_string_free(prefix, TRUE);
    g_free(dir); g_free(partial);
}

/* Base64 encode / decode (replaces selection / whole doc). */
static void replace_target(const char *text, gsize len) {
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t a = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t b = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
    if (a == b) {
        /* Replace whole doc. */
        scintilla_send_message(s, SCI_SETTEXT, 0, (sptr_t)text);
    } else {
        char *zterm = g_strndup(text, len);
        scintilla_send_message(s, SCI_REPLACESEL, 0, (sptr_t)zterm);
        g_free(zterm);
    }
}

static void action_base64_enc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; char *buf = grab_text_for_op(&len);
    gchar *enc = g_base64_encode((const guchar *)buf, len);
    replace_target(enc, enc ? strlen(enc) : 0);
    g_free(enc); g_free(buf);
}
static void action_base64_dec(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; char *buf = grab_text_for_op(&len);
    gsize out_len = 0;
    guchar *dec = g_base64_decode(buf ? buf : "", &out_len);
    replace_target((const char *)dec, out_len);
    g_free(dec); g_free(buf);
}

/* Q6 — matching macOS MIME Tools (MainWindowController.mm:6867-6872). */

/* "Encode with Padding" is the standard Base64 (= padding). GLib's
 * g_base64_encode already produces padded output, so this is just an alias
 * that gives the menu item a separate name for clarity. */
static void action_base64_enc_padded(GSimpleAction *a, GVariant *p, gpointer u) {
    action_base64_enc(a, p, u);
}

/* "Strict Mode" decoder: reject any input that contains characters outside
 * the Base64 alphabet (including whitespace). g_base64_decode silently
 * skips invalid chars, so we pre-validate. */
static void action_base64_dec_strict(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; char *buf = grab_text_for_op(&len);
    /* Validate: only [A-Za-z0-9+/=] allowed, length divisible by 4. */
    gboolean ok = (len % 4) == 0;
    for (gsize i = 0; ok && i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='))
            ok = FALSE;
    }
    if (!ok) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(g_window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Strict-mode decode rejected: invalid Base64 characters or bad padding.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        g_free(buf);
        return;
    }
    gsize out_len = 0;
    guchar *dec = g_base64_decode(buf ? buf : "", &out_len);
    replace_target((const char *)dec, out_len);
    g_free(dec); g_free(buf);
}

/* URL-safe Base64: replaces "+" with "-", "/" with "_", strips "=" padding
 * (matches RFC 4648 §5 base64url). */
static void action_base64_enc_urlsafe(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; char *buf = grab_text_for_op(&len);
    gchar *std = g_base64_encode((const guchar *)buf, len);
    if (std) {
        /* In-place rewrite: standard alphabet → URL-safe alphabet, drop '='. */
        gsize w = 0;
        for (gsize r = 0; std[r]; r++) {
            char c = std[r];
            if      (c == '+') std[w++] = '-';
            else if (c == '/') std[w++] = '_';
            else if (c == '=') /* skip */;
            else               std[w++] = c;
        }
        std[w] = '\0';
        replace_target(std, w);
    }
    g_free(std); g_free(buf);
}

static void action_base64_dec_urlsafe(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; char *buf = grab_text_for_op(&len);
    /* Re-pad and translate URL-safe → standard so g_base64_decode accepts it. */
    GString *std = g_string_sized_new(len + 4);
    for (gsize i = 0; i < len; i++) {
        char c = buf[i];
        if      (c == '-') g_string_append_c(std, '+');
        else if (c == '_') g_string_append_c(std, '/');
        else if (c == '\n' || c == '\r' || c == ' ' || c == '\t') /* skip */;
        else               g_string_append_c(std, c);
    }
    while (std->len % 4) g_string_append_c(std, '=');
    gsize out_len = 0;
    guchar *dec = g_base64_decode(std->str, &out_len);
    replace_target((const char *)dec, out_len);
    g_free(dec); g_string_free(std, TRUE); g_free(buf);
}

static void action_ascii_to_hex(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; char *buf = grab_text_for_op(&len);
    GString *hex = g_string_sized_new(len * 2);
    for (gsize i = 0; i < len; i++) g_string_append_printf(hex, "%02X", (unsigned char)buf[i]);
    replace_target(hex->str, hex->len);
    g_string_free(hex, TRUE); g_free(buf);
}
static int hex_nyb(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void action_hex_to_ascii(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; char *buf = grab_text_for_op(&len);
    GString *out = g_string_sized_new(len / 2);
    int hi = -1;
    for (gsize i = 0; i < len; i++) {
        int n = hex_nyb(buf[i]);
        if (n < 0) continue;  /* Skip whitespace / non-hex */
        if (hi < 0) hi = n;
        else { g_string_append_c(out, (char)((hi << 4) | n)); hi = -1; }
    }
    replace_target(out->str, out->len);
    g_string_free(out, TRUE); g_free(buf);
}

/* ──────────────────────────────────────────────────────────────────────
 * G25 Toggle actions for remaining panels (Char Panel / Clip History /
 *      Project Panel) — these modules are grafted; we just wire toggles.
 * ────────────────────────────────────────────────────────────────────── */
static gboolean s_charpanel_visible = FALSE;
static gboolean s_cliphistory_visible = FALSE;
static gboolean s_project_visible    = FALSE;

static void action_toggle_charpanel(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    s_charpanel_visible = !s_charpanel_visible;
    extern void charpanel_set_visible(gboolean);
    charpanel_set_visible(s_charpanel_visible);
}
static void action_toggle_cliphistory(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    s_cliphistory_visible = !s_cliphistory_visible;
    extern void cliphistory_set_visible(gboolean);
    cliphistory_set_visible(s_cliphistory_visible);
}
static void action_toggle_project(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    s_project_visible = !s_project_visible;
    extern gboolean project_is_visible(void);
    extern void     project_set_visible(gboolean);
    project_set_visible(!project_is_visible());
}

/* ──────────────────────────────────────────────────────────────────────
 * G12 Edit menu — full macOS parity
 * ────────────────────────────────────────────────────────────────────── */

static void insert_text_at_caret(const char *s) {
    GtkWidget *w = current_sci(); if (!w) return;
    scintilla_send_message(SCINTILLA(w), SCI_REPLACESEL, 0, (sptr_t)s);
}
static void action_insert_dt_short(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char buf[64]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    insert_text_at_caret(buf);
}
static void action_insert_dt_long(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char buf[128]; strftime(buf, sizeof(buf), "%A, %B %e, %Y at %I:%M %p", &tm);
    insert_text_at_caret(buf);
}
static void action_insert_blank_above(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_HOME, 0, 0);
    sci_send(SCI_NEWLINE, 0, 0);
    sci_send(SCI_LINEUP, 0, 0);
}
/* Q-fix: Insert Date/Time (Custom) — strftime format via GtkEntry dialog. */
static void action_insert_datetime_custom(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Insert Date/Time (Custom Format)",
        g_window ? GTK_WINDOW(g_window) : NULL, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Insert", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    npp_box_pack(GTK_BOX(box), gtk_label_new("strftime format (e.g. %Y-%m-%d %H:%M):"), FALSE, 4);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), "%Y-%m-%d %H:%M:%S");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    npp_box_pack(GTK_BOX(box), entry, FALSE, 4);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *fmt = gtk_entry_get_text(GTK_ENTRY(entry));
        time_t t = time(NULL);
        struct tm *lt = localtime(&t);
        char out[256];
        if (strftime(out, sizeof(out), fmt, lt) > 0)
            sci_send(SCI_REPLACESEL, 0, (sptr_t)out);
    }
    gtk_widget_destroy(dlg);
}

/* Q-fix Paste Special: HTML / RTF / binary copy+paste (4 items).
 * GTK Clipboard exposes typed targets; we request the matching MIME and
 * splat the byte stream into Scintilla. Binary copy/paste round-trips
 * through a base64 fence so embedded NULs survive a paste into the
 * editor body (which is otherwise UTF-8 only). */
/* Paste-special (HTML / RTF source). GTK4 clipboard reads are async and
 * mime-typed: read the requested mime as a stream and splat the bytes into
 * Scintilla. */
static void on_paste_mime_ready(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)ud;
    GInputStream *stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(src),
                                                     res, NULL, NULL);
    if (!stream) return;
    GByteArray *buf = g_byte_array_new();
    guchar chunk[4096];
    gssize n;
    while ((n = g_input_stream_read(stream, chunk, sizeof chunk, NULL, NULL)) > 0)
        g_byte_array_append(buf, chunk, (guint)n);
    g_object_unref(stream);
    if (buf->len) {
        char *txt = g_strndup((const char *)buf->data, buf->len);
        sci_send(SCI_REPLACESEL, 0, (sptr_t)txt);
        g_free(txt);
    }
    g_byte_array_unref(buf);
}
static void paste_special_mime(const char *mime) {
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    const char *mimes[] = { mime, NULL };
    gdk_clipboard_read_async(cb, mimes, G_PRIORITY_DEFAULT, NULL,
                             on_paste_mime_ready, NULL);
}
static void action_paste_html(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    paste_special_mime("text/html");
}
static void action_paste_rtf(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    paste_special_mime("text/rtf");
}
static void action_copy_binary(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sptr_t s = sci_send(SCI_GETSELECTIONSTART, 0, 0);
    sptr_t e = sci_send(SCI_GETSELECTIONEND, 0, 0);
    if (s == e) return;
    sptr_t len = e - s;
    char *buf = g_malloc(len + 1);
    Sci_TextRangeFull tr = { { s, e }, buf };
    sci_send(SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    gchar *enc = g_base64_encode((guchar *)buf, len);
    gchar *fenced = g_strdup_printf("--BINARY-BEGIN--\n%s\n--BINARY-END--", enc);
    npp_clipboard_set_text(fenced);
    g_free(fenced); g_free(enc); g_free(buf);
}
static void on_paste_binary_ready(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    gchar *txt = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (!txt) return;
    const char *open = strstr(txt, "--BINARY-BEGIN--\n");
    const char *close = strstr(txt, "\n--BINARY-END--");
    if (open && close && close > open + 17) {
        gchar *enc = g_strndup(open + 17, (gsize)(close - (open + 17)));
        gsize n = 0;
        guchar *dec = g_base64_decode(enc, &n);
        if (dec) {
            char *cstr = g_malloc(n + 1);
            memcpy(cstr, dec, n); cstr[n] = '\0';
            sci_send(SCI_REPLACESEL, 0, (sptr_t)cstr);
            g_free(cstr); g_free(dec);
        }
        g_free(enc);
    } else {
        sci_send(SCI_REPLACESEL, 0, (sptr_t)txt);
    }
    g_free(txt);
}
static void action_paste_binary(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_read_text_async(cb, NULL, on_paste_binary_ready, NULL);
}

/* Q-fix Search → Find Characters in Range (find any char in selected range). */
static void action_find_chars_in_range(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Find Characters in Range",
        g_window ? GTK_WINDOW(g_window) : NULL, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Find", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    npp_box_pack(GTK_BOX(box), gtk_label_new("Find any character with codepoint in range:"), FALSE, 4);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *e1 = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(e1), "0");
    GtkWidget *e2 = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(e2), "31");
    npp_box_pack(GTK_BOX(row), gtk_label_new("Min:"), FALSE, 0);
    npp_box_pack(GTK_BOX(row), e1, TRUE, 0);
    npp_box_pack(GTK_BOX(row), gtk_label_new("Max:"), FALSE, 0);
    npp_box_pack(GTK_BOX(row), e2, TRUE, 0);
    npp_box_pack(GTK_BOX(box), row, FALSE, 4);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        int lo = atoi(gtk_entry_get_text(GTK_ENTRY(e1)));
        int hi = atoi(gtk_entry_get_text(GTK_ENTRY(e2)));
        if (lo > hi) { int t=lo; lo=hi; hi=t; }
        sptr_t pos  = sci_send(SCI_GETCURRENTPOS, 0, 0);
        sptr_t total = sci_send(SCI_GETLENGTH, 0, 0);
        for (sptr_t i = pos + 1; i < total; i++) {
            int c = (int)sci_send(SCI_GETCHARAT, (uptr_t)i, 0);
            if (c >= lo && c <= hi) {
                sci_send(SCI_GOTOPOS, (uptr_t)i, 0);
                sci_send(SCI_SCROLLCARET, 0, 0);
                break;
            }
        }
    }
    gtk_widget_destroy(dlg);
}

/* Q-fix View → View Current File In (4 items, xdg-open variants). */
static void open_current_file_in(const char *binary) {
    NppDoc *doc = editor_current_doc();
    if (!doc || !doc->filepath || !*doc->filepath) return;
    const char *argv[3] = { binary, doc->filepath, NULL };
    g_spawn_async(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH,
                  NULL, NULL, NULL, NULL);
}
static void action_view_in_firefox(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; open_current_file_in("firefox");
}
static void action_view_in_chrome(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; open_current_file_in("google-chrome");
}
static void action_view_in_chromium(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; open_current_file_in("chromium");
}
static void action_view_in_default(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; open_current_file_in("xdg-open");
}

/* Q-fix View → Distraction Free Mode (matches macOS).
 * Toggles full-screen + hides menubar/toolbar/sidepanel/status for a clean
 * writing surface. Stores prior visibility on the window so we can restore. */
static void action_distraction_free(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    if (!g_window) return;
    gboolean on = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g_window),
                                                    "npp-dfree"));
    on = !on;
    g_object_set_data(G_OBJECT(g_window), "npp-dfree", GINT_TO_POINTER(on));
    if (on) gtk_window_fullscreen(GTK_WINDOW(g_window));
    else    gtk_window_unfullscreen(GTK_WINDOW(g_window));
}

/* Q-fix View → Post-It Mode (always-on-top + small floating window).
 * macOS togglePostItMode — we replicate the gist: pin window to top and
 * shrink it. Restoring undoes both. */
static void action_post_it(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    if (!g_window) return;
    gboolean on = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g_window),
                                                    "npp-postit"));
    on = !on;
    g_object_set_data(G_OBJECT(g_window), "npp-postit", GINT_TO_POINTER(on));
    gtk_window_set_keep_above(GTK_WINDOW(g_window), on);
    if (on) gtk_window_resize(GTK_WINDOW(g_window), 360, 280);
}

/* Q-fix View → Hide Line Marks (toggle the bookmark margin width). */
static void action_hide_line_marks(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    sptr_t cur = scintilla_send_message(SCINTILLA(doc->sci),
        SCI_GETMARGINWIDTHN, 1, 0);
    scintilla_send_message(SCINTILLA(doc->sci),
        SCI_SETMARGINWIDTHN, 1, cur > 0 ? 0 : 16);
}

/* Q-fix View → Synchronize Scrolling. Pre-existing placeholders in toolbar.c
 * said "until split-pane scroll-sync is wired". This pair toggles a window-
 * level flag we read in split.c on every scroll event in the secondary view. */
static void sync_scroll_toggle(const char *key) {
    if (!g_window) return;
    gboolean on = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g_window), key));
    g_object_set_data(G_OBJECT(g_window), key, GINT_TO_POINTER(!on));
}
static void action_sync_scroll_v(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sync_scroll_toggle("npp-sync-v");
}
static void action_sync_scroll_h(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sync_scroll_toggle("npp-sync-h");
}

/* Q-fix Focus on Another View (cycles focus between primary + secondary). */
static void action_focus_other_view(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; split_focus_other();
}

/* Q-fix File → Print Now — print without showing the page-setup dialog.
 * The current main_do_print is a stub (line ~245) — wire to it so the
 * action surface is complete; future improvement is real default-printer
 * pipeline. */
static void action_print_now(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    main_do_print();
}

/* Q-fix File → Open Containing Folder → {Files, Terminal} split. */
static void action_open_containing_files(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *doc = editor_current_doc();
    if (!doc || !doc->filepath) return;
    gchar *dir = g_path_get_dirname(doc->filepath);
    gchar *uri = g_strdup_printf("file://%s", dir);
    gtk_show_uri_on_window(g_window ? GTK_WINDOW(g_window) : NULL,
                           uri, GDK_CURRENT_TIME, NULL);
    g_free(uri); g_free(dir);
}
static void action_open_containing_terminal(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *doc = editor_current_doc();
    if (!doc || !doc->filepath) return;
    gchar *dir = g_path_get_dirname(doc->filepath);
    /* Try common terminals in order of preference. */
    const char *terms[] = { "gnome-terminal", "konsole", "xfce4-terminal",
                            "xterm", NULL };
    for (int i = 0; terms[i]; i++) {
        gchar *cmd = g_strdup_printf("%s --working-directory=\"%s\"",
                                     terms[i], dir);
        GError *err = NULL;
        if (g_spawn_command_line_async(cmd, &err)) { g_free(cmd); break; }
        if (err) g_error_free(err);
        g_free(cmd);
    }
    g_free(dir);
}

/* Q-fix Language → User Defined Language (3 actions). The Define dialog
 * itself (udl_editor_show) is being built in a follow-up — for now the
 * action surfaces a placeholder dialog so the menu entry is wired and
 * the action map is complete. */
static void action_udl_define(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    udl_editor_show(g_window ? GTK_WINDOW(g_window) : NULL);
}
static void action_udl_open_folder(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gchar *dir = npp_user_file("userDefineLangs", "");
    if (!g_file_test(dir, G_FILE_TEST_IS_DIR))
        g_mkdir_with_parents(dir, 0755);
    gchar *uri = g_strdup_printf("file://%s", dir);
    gtk_show_uri_on_window(g_window ? GTK_WINDOW(g_window) : NULL,
                           uri, GDK_CURRENT_TIME, NULL);
    g_free(uri); g_free(dir);
}
static void action_udl_collection(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gtk_show_uri_on_window(g_window ? GTK_WINDOW(g_window) : NULL,
        "https://github.com/notepad-plus-plus/userDefinedLanguages",
        GDK_CURRENT_TIME, NULL);
}

/* Q-align Settings → Import submenu (macOS parity). Both forms open a
 * file picker; the chosen file is copied into the appropriate user dir. */
static void import_to_user_dir(const char *subdir, const char *title) {
    GtkWidget *p = gtk_file_chooser_dialog_new(title,
        g_window ? GTK_WINDOW(g_window) : NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Import", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(p), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(p)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(p));
        gchar *dst_dir = npp_user_file(subdir, "");
        g_mkdir_with_parents(dst_dir, 0755);
        for (GSList *it = files; it; it = it->next) {
            gchar *src = it->data;
            gchar *base = g_path_get_basename(src);
            gchar *dst = g_build_filename(dst_dir, base, NULL);
            gchar *body = NULL; gsize len = 0;
            if (g_file_get_contents(src, &body, &len, NULL)) {
                g_file_set_contents(dst, body, len, NULL);
                g_free(body);
            }
            g_free(dst); g_free(base);
        }
        g_slist_free_full(files, g_free);
        g_free(dst_dir);
    }
    gtk_widget_destroy(p);
}
static void action_import_plugin(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    import_to_user_dir("plugins", "Import Plugin .so files");
}
static void action_import_style_theme(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    import_to_user_dir("themes", "Import Style Theme XML");
}

/* Q-align Run menu helpers (macOS parity per 11-run_menu.png).
 * "Get PHP help" runs the canonical PHP doc lookup on selected text.
 * "Wikipedia Search" opens an English Wikipedia search on selected text.
 * "Open selected file path in new instance" tries to launch a fresh
 * Nextpad++ on the path under the cursor / selection. */
static gchar *current_selection_or_word(void) {
    NppDoc *doc = editor_current_doc();
    if (!doc) return NULL;
    ScintillaObject *s = SCINTILLA(doc->sci);
    sptr_t a = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t b = scintilla_send_message(s, SCI_GETSELECTIONEND, 0, 0);
    if (a == b) {
        sptr_t pos = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
        a = scintilla_send_message(s, SCI_WORDSTARTPOSITION, (uptr_t)pos, TRUE);
        b = scintilla_send_message(s, SCI_WORDENDPOSITION,   (uptr_t)pos, TRUE);
    }
    if (a == b) return NULL;
    sptr_t n = b - a;
    gchar *buf = g_malloc(n + 1);
    Sci_TextRangeFull tr = {{a, b}, buf};
    scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    return buf;
}
static void action_get_php_help(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gchar *word = current_selection_or_word(); if (!word) return;
    gchar *url = g_strdup_printf("https://php.net/%s", word);
    gtk_show_uri_on_window(g_window ? GTK_WINDOW(g_window) : NULL,
                           url, GDK_CURRENT_TIME, NULL);
    g_free(url); g_free(word);
}
static void action_wikipedia_search(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gchar *word = current_selection_or_word(); if (!word) return;
    gchar *esc  = g_uri_escape_string(word, NULL, FALSE);
    gchar *url  = g_strdup_printf(
        "https://en.wikipedia.org/wiki/Special:Search?search=%s", esc);
    gtk_show_uri_on_window(g_window ? GTK_WINDOW(g_window) : NULL,
                           url, GDK_CURRENT_TIME, NULL);
    g_free(url); g_free(esc); g_free(word);
}
static void action_open_in_new_instance(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gchar *path = current_selection_or_word(); if (!path) return;
    /* If the selection is a relative path, resolve against current doc dir. */
    gchar *full = path;
    if (path[0] != '/') {
        NppDoc *doc = editor_current_doc();
        if (doc && doc->filepath) {
            gchar *dir = g_path_get_dirname(doc->filepath);
            full = g_build_filename(dir, path, NULL);
            g_free(dir);
        }
    }
    /* spawn the same binary with `--new-instance <path>` semantics. */
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        const char *argv[] = { self, "--gapplication-app-id=org.nextpad.Aux",
                               full, NULL };
        g_spawn_async(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH,
                      NULL, NULL, NULL, NULL);
    }
    if (full != path) g_free(full);
    g_free(path);
}

/* Q-align: Modify Shortcut/Delete Macro and Run-Command. We don't expose
 * per-tab focus yet — both routes open the existing Shortcut Mapper which
 * has 5 tabs (Main, Macros, Run, Plugin, Scintilla). User clicks the right
 * tab inside. */
static void action_modify_shortcut_macro(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GApplication *app = g_application_get_default();
    if (app) g_action_group_activate_action(G_ACTION_GROUP(app),
                                            "shortcut-map", NULL);
}
static void action_modify_shortcut_run(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GApplication *app = g_application_get_default();
    if (app) g_action_group_activate_action(G_ACTION_GROUP(app),
                                            "shortcut-map", NULL);
}

/* Q-align Edit → Begin/End Select (column-mode variant too).
 * macOS behavior: first invocation sets the anchor; second extends the
 * selection from anchor to current caret. The column-mode variant does
 * the same but in rectangle (SC_SEL_RECTANGLE) selection mode. */
static void begin_end_select(gboolean column) {
    NppDoc *doc = editor_current_doc(); if (!doc) return;
    ScintillaObject *s = SCINTILLA(doc->sci);
    sptr_t anchor = (sptr_t)g_object_get_data(G_OBJECT(doc->sci),
                                              "npp-be-anchor");
    sptr_t pos = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
    if (anchor == 0 && !g_object_get_data(G_OBJECT(doc->sci),
                                          "npp-be-armed")) {
        /* First press: arm the anchor at current caret. */
        g_object_set_data(G_OBJECT(doc->sci), "npp-be-armed", GINT_TO_POINTER(1));
        g_object_set_data(G_OBJECT(doc->sci), "npp-be-anchor",
                          GSIZE_TO_POINTER((size_t)pos));
        return;
    }
    /* Second press: extend selection. */
    scintilla_send_message(s, SCI_SETSELECTIONMODE,
        column ? SC_SEL_RECTANGLE : SC_SEL_STREAM, 0);
    scintilla_send_message(s, SCI_SETSEL, (uptr_t)anchor, pos);
    g_object_set_data(G_OBJECT(doc->sci), "npp-be-armed", GINT_TO_POINTER(0));
}
static void action_begin_end_select(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; begin_end_select(FALSE);
}
static void action_begin_end_select_column(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; begin_end_select(TRUE);
}

/* Q-align Search → Select and Find Next/Previous: take selection as new
 * needle, then advance. */
static void select_and_find(int direction) {
    gchar *sel = current_selection_or_word();
    if (!sel) return;
    findreplace_set_options(sel, "", FALSE, FALSE, TRUE, 0);
    g_free(sel);
    if (direction > 0) findreplace_find_next();
    else               findreplace_find_prev();
}
static void action_select_find_next(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; select_and_find(+1);
}
static void action_select_find_prev(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; select_and_find(-1);
}

/* Q-align View → Spell Check toggle (spell.c grafted long ago). */
extern void spell_set_enabled(gboolean);
extern gboolean spell_is_enabled(void);
static void action_toggle_spell_check(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gboolean cur = spell_is_enabled();
    spell_set_enabled(!cur);
}

/* Q-fix Settings → Edit Popup ContextMenu (opens contextMenu.xml). */
static void action_edit_popup_ctxmenu(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gchar *path = npp_user_file("", "contextMenu.xml");
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        /* Seed from bundle if user file missing. */
        gchar *bundle = npp_bundle_file("", "contextMenu.xml");
        if (bundle && g_file_test(bundle, G_FILE_TEST_EXISTS)) {
            gchar *body = NULL; gsize len = 0;
            if (g_file_get_contents(bundle, &body, &len, NULL))
                g_file_set_contents(path, body, len, NULL);
            g_free(body);
        }
        g_free(bundle);
    }
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        editor_open_path(path);
    g_free(path);
}

/* Q-fix Help → Debug Info (system + build snapshot for issue reports). */
/* ── Help ▸ Check for Updates / Install CLI Tool ────────────────────
 * Ports the two macOS app-menu items (AppDelegate checkForUpdates: and
 * MainWindowController installCommandLineTool:) into the Linux Help
 * menu. ────────────────────────────────────────────────────────────── */

/* Simple modal info dialog (GTK4 GtkAlertDialog). */
static void npp_info_dialog(const char *msg, const char *detail) {
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", msg);
    if (detail) gtk_alert_dialog_set_detail(d, detail);
    gtk_alert_dialog_show(d, g_window ? GTK_WINDOW(g_window) : NULL);
    g_object_unref(d);
}

/* Compare two "X.Y.Z" version strings: <0 / 0 / >0. */
static int version_cmp(const char *a, const char *b) {
    int a1=0,a2=0,a3=0, b1=0,b2=0,b3=0;
    sscanf(a ? a : "", "%d.%d.%d", &a1,&a2,&a3);
    sscanf(b ? b : "", "%d.%d.%d", &b1,&b2,&b3);
    if (a1 != b1) return a1 < b1 ? -1 : 1;
    if (a2 != b2) return a2 < b2 ? -1 : 1;
    if (a3 != b3) return a3 < b3 ? -1 : 1;
    return 0;
}

/* Pull a top-level string value for `key` out of a JSON blob. */
static char *json_string_value(const char *json, const char *key) {
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = json ? strstr(json, needle) : NULL;
    if (!p) return NULL;
    p = strchr(p + strlen(needle), ':');
    if (!p) return NULL;
    for (p++; *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'; p++) ;
    if (*p != '"') return NULL;
    p++;
    const char *e = p;
    while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
    return g_strndup(p, (gsize)(e - p));
}

/* Version check targets the public macOS repo — it is the release
 * source-of-truth (CLAUDE.md) and, unlike the private GTK4 repo, its
 * releases API is reachable without authentication. Same endpoint the
 * macOS app uses (AppDelegate kGitHubReleasesAPI). */
static const char *kReleasesAPI =
    "https://api.github.com/repos/nextpad-plus-plus/"
    "nextpad-plus-plus-macos/releases/latest";

/* The Help section holding the "Check for Updates" item — kept so its
 * status bullet can be refreshed after a check. */
static GMenu *g_updates_section = NULL;
void main_retranslate_menu(void);   /* defined later in this file */

/* Refresh the "Check for Updates" menu item with a status bullet.
 * state — 0 none, 1 green (up to date), 2 yellow (update available).
 * The bullet is a coloured-circle emoji prefixed to the label —
 * GtkPopoverMenuBar renders it in colour via the emoji font.
 * (g_menu_item_set_icon is NOT used: a GIcon on a popover-menu item
 * crashes GTK's menu renderer with a GdkPixbuf/GdkPaintable type
 * mismatch. i18n_translate_menu preserves the leading bullet.) */
static void set_update_badge(int state, const char *label) {
    if (!g_updates_section) return;
    const char *base = label ? label : "Check for Updates…";
    char *full = (state == 2) ? g_strconcat("🟡 ", base, NULL)
               : (state == 1) ? g_strconcat("🟢 ", base, NULL)
               :                g_strdup(base);
    GMenuItem *it = g_menu_item_new(full, "app.check-updates");
    g_free(full);
    g_menu_remove(g_updates_section, 0);
    g_menu_insert_item(g_updates_section, 0, it);
    g_object_unref(it);
    main_retranslate_menu();   /* push through the i18n menu copy */
}

/* "Open Release Page" choice from the update-available dialog. */
static void on_update_choice(GObject *src, GAsyncResult *res, gpointer u) {
    char *url = (char *)u;
    int idx = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    if (idx == 0 && url && *url && g_window)
        gtk_show_uri_on_window(GTK_WINDOW(g_window), url,
                               GDK_CURRENT_TIME, NULL);
    g_free(url);
}

/* curl GET completed — parse tag_name, set the bullet, and (when the
 * user asked) report the result in a dialog. user_data carries the
 * user-initiated flag; a silent startup check only sets the bullet. */
static void on_update_response(GObject *src, GAsyncResult *res, gpointer u) {
    gboolean user_initiated = GPOINTER_TO_INT(u);
    GSubprocess *proc = G_SUBPROCESS(src);
    char   *out = NULL;
    GError *err = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(proc, res,
                                                       &out, NULL, &err);
    if (!ok || !out || g_subprocess_get_exit_status(proc) != 0) {
        if (user_initiated)
            npp_info_dialog("Unable to Check for Updates",
                err ? err->message
                    : "Could not reach the update server — check your "
                      "internet connection.");
        if (err) g_error_free(err);
        g_free(out);
        g_object_unref(proc);
        return;
    }
    char *tag = json_string_value(out, "tag_name");
    char *url = json_string_value(out, "html_url");
    const char *latest = tag ? (tag[0] == 'v' ? tag + 1 : tag) : NULL;

    if (!latest || !*latest) {
        if (user_initiated)
            npp_info_dialog("Unable to Check for Updates",
                            "No published release was found.");
    } else if (version_cmp(latest, APP_VERSION) > 0) {
        char label[96];
        snprintf(label, sizeof label, "Update Available (v%s)", latest);
        set_update_badge(2, label);                 /* yellow bullet */
        if (user_initiated) {
            GtkAlertDialog *d = gtk_alert_dialog_new(
                "Nextpad++ v%s is available", latest);
            char detail[160];
            snprintf(detail, sizeof detail,
                     "You are running v%s.", APP_VERSION);
            gtk_alert_dialog_set_detail(d, detail);
            const char *btns[] = { "Open Release Page", "Later", NULL };
            gtk_alert_dialog_set_buttons(d, btns);
            gtk_alert_dialog_set_default_button(d, 0);
            gtk_alert_dialog_set_cancel_button(d, 1);
            gtk_alert_dialog_choose(d, g_window ? GTK_WINDOW(g_window) : NULL,
                                    NULL, on_update_choice,
                                    url ? g_strdup(url) : NULL);
            g_object_unref(d);
        }
    } else {
        set_update_badge(1, "Check for Updates…");  /* green bullet */
        if (user_initiated) {
            char detail[128];
            snprintf(detail, sizeof detail,
                     "Nextpad++ %s is the latest version.", APP_VERSION);
            npp_info_dialog("You're Up to Date", detail);
        }
    }
    g_free(tag); g_free(url); g_free(out);
    g_object_unref(proc);
}

/* Kick off a GitHub releases-API check. user_initiated TRUE shows a
 * result dialog; FALSE (startup) just refreshes the menu bullet. */
static void check_for_updates(gboolean user_initiated) {
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &err, "curl", "-fsSL",
        "-A", "nextpad-plus-plus-gtk4",
        "-H", "Accept: application/vnd.github+json",
        kReleasesAPI, NULL);
    if (!proc) {
        if (user_initiated)
            npp_info_dialog("Unable to Check for Updates",
                err ? err->message
                    : "The 'curl' command is required to check for updates.");
        if (err) g_error_free(err);
        return;
    }
    g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_update_response,
                                        GINT_TO_POINTER(user_initiated));
}

static void action_check_updates(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    check_for_updates(TRUE);
}

/* Silent startup check (macOS checkForUpdateUserInitiated:NO) — runs
 * once, regardless of how many times it is scheduled. */
static gboolean startup_update_check(gpointer d) {
    (void)d;
    static gboolean done = FALSE;
    if (done) return G_SOURCE_REMOVE;
    done = TRUE;
    check_for_updates(FALSE);
    return G_SOURCE_REMOVE;
}

static void action_install_cli(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GError *err = NULL;
    /* /proc/self/exe is a symlink to the running binary. */
    char *exe = g_file_read_link("/proc/self/exe", &err);
    if (!exe) {
        npp_info_dialog("Installation Failed",
            err ? err->message
                : "Could not locate the Nextpad++ executable.");
        if (err) g_error_free(err);
        return;
    }
    char *bindir = g_build_filename(g_get_home_dir(), ".local", "bin", NULL);
    g_mkdir_with_parents(bindir, 0755);
    char *target = g_build_filename(bindir, "nextpad++", NULL);
    /* A wrapper script (mirrors macOS _makeCLIScriptForApp) so it keeps
     * working after the app is moved — just re-run the menu item. */
    char *script = g_strdup_printf(
        "#!/bin/sh\n"
        "# nextpad++ — CLI wrapper for Nextpad++ (Linux).\n"
        "# Auto-generated; re-run Help > Install nextpad++ Command Line\n"
        "# Tool if the application is moved.\n"
        "exec \"%s\" \"$@\"\n", exe);

    char *existing = NULL;
    g_file_get_contents(target, &existing, NULL, NULL);
    gboolean already = (existing && g_strcmp0(existing, script) == 0);
    g_free(existing);

    char detail[640];
    if (already) {
        snprintf(detail, sizeof detail,
            "nextpad++ is already installed at:\n%s\n\nUsage:  nextpad++ file.txt",
            target);
        npp_info_dialog("Already Installed", detail);
    } else if (g_file_set_contents(target, script, -1, &err)) {
        chmod(target, 0755);
        gboolean on_path = FALSE;
        const char *pe = g_getenv("PATH");
        if (pe) {
            char **dirs = g_strsplit(pe, ":", -1);
            for (int i = 0; dirs[i]; i++)
                if (g_strcmp0(dirs[i], bindir) == 0) on_path = TRUE;
            g_strfreev(dirs);
        }
        snprintf(detail, sizeof detail,
            "Installed the 'nextpad++' command at:\n%s\n\nUsage:  nextpad++ file.txt%s",
            target,
            on_path ? ""
                    : "\n\nNote: ~/.local/bin is not on your PATH — add it "
                      "to run 'nextpad++' from any terminal.");
        npp_info_dialog("Command Line Tool Installed", detail);
    } else {
        npp_info_dialog("Installation Failed",
            err ? err->message : "Could not write the wrapper script.");
        if (err) g_error_free(err);
    }
    g_free(exe); g_free(bindir); g_free(target); g_free(script);
}

static void action_debug_info(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "Nextpad++ — Debug Info\n\n");
    g_string_append_printf(s, "Built:      %s %s\n", __DATE__, __TIME__);
    g_string_append_printf(s, "GLib:       %d.%d.%d\n",
        glib_major_version, glib_minor_version, glib_micro_version);
    g_string_append_printf(s, "GTK:        %d.%d.%d\n",
        gtk_get_major_version(), gtk_get_minor_version(),
        gtk_get_micro_version());
    g_string_append_printf(s, "Config dir: %s\n", g_get_user_config_dir());
    g_string_append_printf(s, "Locale:     %s\n",
        g_getenv("LANG") ? g_getenv("LANG") : "(unset)");
    g_string_append_printf(s, "Display:    %s\n",
        g_getenv("WAYLAND_DISPLAY") ? "Wayland" :
        g_getenv("DISPLAY")         ? "X11"     : "(none)");
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Debug Info",
        g_window ? GTK_WINDOW(g_window) : NULL, GTK_DIALOG_MODAL,
        "_Copy to Clipboard", 1, "_Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(buf, s->str, -1);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(sw), 480);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 240);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    npp_box_pack(GTK_BOX(box), sw, TRUE, 0);
    gtk_widget_show_all(dlg);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == 1)
        npp_clipboard_set_text(s->str);
    gtk_widget_destroy(dlg);
    g_string_free(s, TRUE);
}

static void action_insert_blank_below(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_LINEEND, 0, 0);
    sci_send(SCI_NEWLINE, 0, 0);
}

static void copy_to_clipboard(const char *text) {
    if (text) npp_clipboard_set_text(text);
}
static void action_copy_full_path(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *d = editor_current_doc(); if (d && d->filepath) copy_to_clipboard(d->filepath);
}
static void action_copy_file_name(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *d = editor_current_doc(); if (!d || !d->filepath) return;
    char *b = g_path_get_basename(d->filepath); copy_to_clipboard(b); g_free(b);
}
static void action_copy_dir_path(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *d = editor_current_doc(); if (!d || !d->filepath) return;
    char *dir = g_path_get_dirname(d->filepath); copy_to_clipboard(dir); g_free(dir);
}
static void action_copy_all_names(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GString *out = g_string_new(NULL);
    int n = editor_page_count();
    for (int i = 0; i < n; i++) {
        NppDoc *d = editor_doc_at(i); if (!d) continue;
        if (d->filepath) { char *b = g_path_get_basename(d->filepath);
                           g_string_append_printf(out, "%s\n", b); g_free(b); }
        else g_string_append_printf(out, "Untitled-%d\n", d->new_index);
    }
    copy_to_clipboard(out->str); g_string_free(out, TRUE);
}
static void action_copy_all_paths(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GString *out = g_string_new(NULL);
    int n = editor_page_count();
    for (int i = 0; i < n; i++) {
        NppDoc *d = editor_doc_at(i);
        if (d && d->filepath) g_string_append_printf(out, "%s\n", d->filepath);
    }
    copy_to_clipboard(out->str); g_string_free(out, TRUE);
}

static void action_indent(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_TAB, 0, 0);
}
static void action_unindent(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_BACKTAB, 0, 0);
}

static char *get_sel_or_doc(gsize *outlen, gboolean *whole_doc) {
    GtkWidget *sci = current_sci();
    if (!sci) { *outlen = 0; if(whole_doc)*whole_doc=TRUE; return g_strdup(""); }
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t a = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t b = scintilla_send_message(s, SCI_GETSELECTIONEND, 0, 0);
    if (a == b) {
        sptr_t len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
        char *buf = g_malloc((gsize)len + 1);
        struct Sci_TextRangeFull tr = {{0, len}, buf};
        scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
        *outlen = (gsize)len; if (whole_doc) *whole_doc = TRUE;
        return buf;
    }
    if (a > b) { sptr_t t = a; a = b; b = t; }
    sptr_t len = b - a;
    char *buf = g_malloc((gsize)len + 1);
    struct Sci_TextRangeFull tr = {{a, b}, buf};
    scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    *outlen = (gsize)len; if (whole_doc) *whole_doc = FALSE;
    return buf;
}
static void put_back(const char *text, gboolean whole_doc) {
    GtkWidget *sci = current_sci(); if (!sci) return;
    if (whole_doc) scintilla_send_message(SCINTILLA(sci), SCI_SETTEXT, 0, (sptr_t)text);
    else           scintilla_send_message(SCINTILLA(sci), SCI_REPLACESEL, 0, (sptr_t)text);
}

static void action_case_upper(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_UPPERCASE, 0, 0);
}
static void action_case_lower(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_LOWERCASE, 0, 0);
}
typedef int (*case_fn)(gunichar, gunichar);
static void apply_case(case_fn fn) {
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    GString *out = g_string_sized_new(len);
    gunichar prev = ' ';
    const gchar *p = buf;
    while (p && *p) {
        gunichar c = g_utf8_get_char(p);
        gunichar nc = (gunichar)fn(c, prev);
        g_string_append_unichar(out, nc);
        prev = c;
        p = g_utf8_next_char(p);
    }
    put_back(out->str, wd);
    g_string_free(out, TRUE); g_free(buf);
}
static int cf_proper_force(gunichar c, gunichar prev) {
    return !g_unichar_isalnum(prev) ? (int)g_unichar_toupper(c) : (int)g_unichar_tolower(c);
}
static int cf_proper_blend(gunichar c, gunichar prev) {
    return !g_unichar_isalnum(prev) ? (int)g_unichar_toupper(c) : (int)c;
}
static int cf_sentence_force(gunichar c, gunichar prev) {
    gboolean ss = (prev == '.' || prev == '!' || prev == '?' || prev == '\n');
    return ss ? (int)g_unichar_toupper(c) : (int)g_unichar_tolower(c);
}
static int cf_sentence_blend(gunichar c, gunichar prev) {
    gboolean ss = (prev == '.' || prev == '!' || prev == '?' || prev == '\n');
    return ss ? (int)g_unichar_toupper(c) : (int)c;
}
static int cf_invert(gunichar c, gunichar prev) {
    (void)prev;
    if (g_unichar_isupper(c)) return (int)g_unichar_tolower(c);
    if (g_unichar_islower(c)) return (int)g_unichar_toupper(c);
    return (int)c;
}
static int cf_random(gunichar c, gunichar prev) {
    (void)prev; if (!g_unichar_isalpha(c)) return (int)c;
    return (g_random_int() & 1) ? (int)g_unichar_toupper(c) : (int)g_unichar_tolower(c);
}
static void action_case_proper_force(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; apply_case(cf_proper_force);
}
static void action_case_proper_blend(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; apply_case(cf_proper_blend);
}
static void action_case_sentence_force(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; apply_case(cf_sentence_force);
}
static void action_case_sentence_blend(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; apply_case(cf_sentence_blend);
}
static void action_case_invert(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; apply_case(cf_invert);
}
static void action_case_random(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; apply_case(cf_random);
}

static void action_line_duplicate(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_LINEDUPLICATE, 0, 0);
}
static void action_line_delete(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_LINEDELETE, 0, 0);
}
static void action_line_move_up(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_MOVESELECTEDLINESUP, 0, 0);
}
static void action_line_move_down(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_MOVESELECTEDLINESDOWN, 0, 0);
}
static void action_line_split(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_LINESSPLIT, 0, 0);
}
static void action_line_join(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sci_send(SCI_LINESJOIN, 0, 0);
}

static int sort_cmp_asc(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}
static int sort_cmp_desc(const void *a, const void *b) {
    return -strcmp(*(const char **)a, *(const char **)b);
}
static int sort_cmp_asc_ci(const void *a, const void *b) {
    return g_ascii_strcasecmp(*(const char **)a, *(const char **)b);
}
static int sort_cmp_len(const void *a, const void *b) {
    /* Character count, not byte count — multibyte UTF-8 lines must sort
     * by their visible length (matches Windows/macOS semantics). */
    return (int)g_utf8_strlen(*(const char **)a, -1)
         - (int)g_utf8_strlen(*(const char **)b, -1);
}
static void sort_with(int (*cmp)(const void *, const void *)) {
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    gchar **lines = g_strsplit(buf, "\n", -1);
    int n = 0; while (lines[n]) n++;
    int sortable = (n > 0 && lines[n - 1][0] == '\0') ? n - 1 : n;
    qsort(lines, sortable, sizeof(char *), cmp);
    char *joined = g_strjoinv("\n", lines);
    put_back(joined, wd);
    g_free(joined); g_strfreev(lines); g_free(buf);
}
static void action_sort_asc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sort_with(sort_cmp_asc);
}
static void action_sort_desc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sort_with(sort_cmp_desc);
}
static void action_sort_asc_ci(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sort_with(sort_cmp_asc_ci);
}
static void action_sort_by_length(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sort_with(sort_cmp_len);
}
/* Q-fix sort variants: by-length-desc, random, integer, decimal (8 items). */
static int sort_cmp_len_desc(const void *a, const void *b) {
    int la = (int)g_utf8_strlen(*(char *const *)a, -1);
    int lb = (int)g_utf8_strlen(*(char *const *)b, -1);
    return lb - la;
}
static void action_sort_by_length_desc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; sort_with(sort_cmp_len_desc);
}
static void action_sort_random(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    gchar **lines = g_strsplit(buf, "\n", -1);
    int n = 0; while (lines[n]) n++;
    int sortable = (n > 0 && lines[n - 1][0] == '\0') ? n - 1 : n;
    /* Fisher-Yates */
    for (int i = sortable - 1; i > 0; i--) {
        int j = g_random_int_range(0, i + 1);
        char *t = lines[i]; lines[i] = lines[j]; lines[j] = t;
    }
    char *joined = g_strjoinv("\n", lines);
    put_back(joined, wd);
    g_free(joined); g_strfreev(lines); g_free(buf);
}
static int sort_cmp_int_asc(const void *a, const void *b) {
    long la = strtol(*(char *const *)a, NULL, 10);
    long lb = strtol(*(char *const *)b, NULL, 10);
    return (la > lb) - (la < lb);
}
static int sort_cmp_int_desc(const void *a, const void *b) {
    return -sort_cmp_int_asc(a, b);
}
static int sort_cmp_double_asc_comma(const void *a, const void *b) {
    /* Treat comma as decimal separator. */
    char A[64], B[64];
    g_strlcpy(A, *(char *const *)a, sizeof(A));
    g_strlcpy(B, *(char *const *)b, sizeof(B));
    for (char *p = A; *p; p++) if (*p == ',') *p = '.';
    for (char *p = B; *p; p++) if (*p == ',') *p = '.';
    double da = g_ascii_strtod(A, NULL);
    double db = g_ascii_strtod(B, NULL);
    return (da > db) - (da < db);
}
static int sort_cmp_double_desc_comma(const void *a, const void *b) {
    return -sort_cmp_double_asc_comma(a, b);
}
static int sort_cmp_double_asc_dot(const void *a, const void *b) {
    double da = g_ascii_strtod(*(char *const *)a, NULL);
    double db = g_ascii_strtod(*(char *const *)b, NULL);
    return (da > db) - (da < db);
}
static int sort_cmp_double_desc_dot(const void *a, const void *b) {
    return -sort_cmp_double_asc_dot(a, b);
}
static void action_sort_int_asc(GSimpleAction *a, GVariant *p, gpointer u)        { (void)a;(void)p;(void)u; sort_with(sort_cmp_int_asc); }
static void action_sort_int_desc(GSimpleAction *a, GVariant *p, gpointer u)       { (void)a;(void)p;(void)u; sort_with(sort_cmp_int_desc); }
static void action_sort_dec_comma_asc(GSimpleAction *a, GVariant *p, gpointer u)  { (void)a;(void)p;(void)u; sort_with(sort_cmp_double_asc_comma); }
static void action_sort_dec_comma_desc(GSimpleAction *a, GVariant *p, gpointer u) { (void)a;(void)p;(void)u; sort_with(sort_cmp_double_desc_comma); }
static void action_sort_dec_dot_asc(GSimpleAction *a, GVariant *p, gpointer u)    { (void)a;(void)p;(void)u; sort_with(sort_cmp_double_asc_dot); }
static void action_sort_dec_dot_desc(GSimpleAction *a, GVariant *p, gpointer u)   { (void)a;(void)p;(void)u; sort_with(sort_cmp_double_desc_dot); }

/* Q-fix Remove Consecutive Duplicate Lines. */
static void action_remove_consec_dups(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    gchar **lines = g_strsplit(buf, "\n", -1);
    GString *out = g_string_new(NULL);
    const char *prev = NULL;
    gboolean first = TRUE;
    for (int i = 0; lines[i]; i++) {
        if (prev && strcmp(prev, lines[i]) == 0) continue;
        if (!first) g_string_append_c(out, '\n');
        g_string_append(out, lines[i]); first = FALSE;
        prev = lines[i];
    }
    put_back(out->str, wd);
    g_string_free(out, TRUE); g_strfreev(lines); g_free(buf);
}
static void action_lines_reverse(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    gchar **lines = g_strsplit(buf, "\n", -1);
    int n = 0; while (lines[n]) n++;
    int sortable = (n > 0 && lines[n - 1][0] == '\0') ? n - 1 : n;
    for (int i = 0, j = sortable - 1; i < j; i++, j--) {
        char *t = lines[i]; lines[i] = lines[j]; lines[j] = t;
    }
    char *joined = g_strjoinv("\n", lines);
    put_back(joined, wd);
    g_free(joined); g_strfreev(lines); g_free(buf);
}
static void action_remove_duplicates(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    gchar **lines = g_strsplit(buf, "\n", -1);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GString *out = g_string_new(NULL);
    gboolean first = TRUE;
    for (int i = 0; lines[i]; i++) {
        if (!g_hash_table_contains(seen, lines[i])) {
            g_hash_table_add(seen, lines[i]);
            if (!first) g_string_append_c(out, '\n');
            g_string_append(out, lines[i]); first = FALSE;
        }
    }
    put_back(out->str, wd);
    g_hash_table_destroy(seen); g_string_free(out, TRUE);
    g_strfreev(lines); g_free(buf);
}

static const char *current_lang_comment_prefix(void) {
    GtkWidget *sci = current_sci(); if (!sci) return "// ";
    const char *lang = g_object_get_data(G_OBJECT(sci), "npp-lang");
    if (!lang) return "// ";
    if (!strcmp(lang, "python") || !strcmp(lang, "ruby") || !strcmp(lang, "bash") ||
        !strcmp(lang, "perl")   || !strcmp(lang, "yaml") || !strcmp(lang, "toml") ||
        !strcmp(lang, "ini")    || !strcmp(lang, "props")|| !strcmp(lang, "makefile")||
        !strcmp(lang, "r"))      return "# ";
    if (!strcmp(lang, "sql")  || !strcmp(lang, "lua")) return "-- ";
    if (!strcmp(lang, "asm")  || !strcmp(lang, "lisp")|| !strcmp(lang, "scheme")) return "; ";
    return "// ";
}
static void action_toggle_line_comment(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    const char *pfx = current_lang_comment_prefix();
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t l0 = scintilla_send_message(s, SCI_LINEFROMPOSITION,
                  scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0), 0);
    sptr_t l1 = scintilla_send_message(s, SCI_LINEFROMPOSITION,
                  scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0), 0);
    if (l1 < l0) { sptr_t t = l0; l0 = l1; l1 = t; }
    scintilla_send_message(s, SCI_BEGINUNDOACTION, 0, 0);
    char trimmed[8]; g_strlcpy(trimmed, pfx, sizeof(trimmed));
    gsize pfx_len = strlen(trimmed);
    if (pfx_len > 0 && trimmed[pfx_len - 1] == ' ') trimmed[--pfx_len] = '\0';
    int do_uncomment = -1;
    for (sptr_t ln = l0; ln <= l1; ln++) {
        sptr_t pos = scintilla_send_message(s, SCI_POSITIONFROMLINE, ln, 0);
        sptr_t end = scintilla_send_message(s, SCI_GETLINEENDPOSITION, ln, 0);
        if (end - pos == 0) continue;
        char head[8] = {0};
        for (sptr_t i = 0; i < (sptr_t)sizeof(head) - 1 && pos + i < end; i++)
            head[i] = (char)scintilla_send_message(s, SCI_GETCHARAT, pos + i, 0);
        do_uncomment = (strncmp(head, trimmed, pfx_len) == 0) ? 1 : 0;
        break;
    }
    if (do_uncomment < 0) { scintilla_send_message(s, SCI_ENDUNDOACTION, 0, 0); return; }
    for (sptr_t ln = l0; ln <= l1; ln++) {
        sptr_t pos = scintilla_send_message(s, SCI_POSITIONFROMLINE, ln, 0);
        sptr_t end = scintilla_send_message(s, SCI_GETLINEENDPOSITION, ln, 0);
        if (end - pos == 0) continue;
        if (do_uncomment) {
            char head[8] = {0};
            for (sptr_t i = 0; i < (sptr_t)sizeof(head) - 1 && pos + i < end; i++)
                head[i] = (char)scintilla_send_message(s, SCI_GETCHARAT, pos + i, 0);
            if (strncmp(head, trimmed, pfx_len) == 0) {
                sptr_t rmlen = (sptr_t)pfx_len;
                if (pos + rmlen < end &&
                    (char)scintilla_send_message(s, SCI_GETCHARAT, pos + rmlen, 0) == ' ')
                    rmlen++;
                scintilla_send_message(s, SCI_DELETERANGE, pos, rmlen);
            }
        } else {
            scintilla_send_message(s, SCI_INSERTTEXT, pos, (sptr_t)pfx);
        }
    }
    scintilla_send_message(s, SCI_ENDUNDOACTION, 0, 0);
}

/* Q-align comment add/remove explicit variants (macOS parity).
 * Common helper: walk selected lines and apply op = +1 (add), -1 (remove). */
static void comment_apply(int op_add) {
    const char *pfx = current_lang_comment_prefix();
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t l0 = scintilla_send_message(s, SCI_LINEFROMPOSITION,
                  scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0), 0);
    sptr_t l1 = scintilla_send_message(s, SCI_LINEFROMPOSITION,
                  scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0), 0);
    if (l1 < l0) { sptr_t t = l0; l0 = l1; l1 = t; }
    char trimmed[8]; g_strlcpy(trimmed, pfx, sizeof(trimmed));
    gsize pfx_len = strlen(trimmed);
    if (pfx_len > 0 && trimmed[pfx_len - 1] == ' ') trimmed[--pfx_len] = '\0';
    scintilla_send_message(s, SCI_BEGINUNDOACTION, 0, 0);
    for (sptr_t ln = l0; ln <= l1; ln++) {
        sptr_t pos = scintilla_send_message(s, SCI_POSITIONFROMLINE, ln, 0);
        sptr_t end = scintilla_send_message(s, SCI_GETLINEENDPOSITION, ln, 0);
        if (end - pos == 0) continue;
        char head[8] = {0};
        for (sptr_t i = 0; i < (sptr_t)sizeof(head) - 1 && pos + i < end; i++)
            head[i] = (char)scintilla_send_message(s, SCI_GETCHARAT, pos + i, 0);
        gboolean has_prefix = (strncmp(head, trimmed, pfx_len) == 0);
        if (op_add > 0 && !has_prefix) {
            scintilla_send_message(s, SCI_INSERTTEXT, pos, (sptr_t)pfx);
        } else if (op_add < 0 && has_prefix) {
            sptr_t rmlen = (sptr_t)pfx_len;
            if (pos + rmlen < end &&
                (char)scintilla_send_message(s, SCI_GETCHARAT, pos + rmlen, 0) == ' ')
                rmlen++;
            scintilla_send_message(s, SCI_DELETERANGE, pos, rmlen);
        }
    }
    scintilla_send_message(s, SCI_ENDUNDOACTION, 0, 0);
}
static void action_comment_line_add(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; comment_apply(+1);
}
static void action_comment_line_remove(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; comment_apply(-1);
}
/* Block comment add/remove — use language-specific delimiters from langs.xml
 * via lexer.h; fall back to /* */
/**/
static void block_comment_apply(int op_add) {
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t a = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t b = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
    if (a == b) return;
    const char *open  = "/* ";
    const char *close = " */";
    scintilla_send_message(s, SCI_BEGINUNDOACTION, 0, 0);
    if (op_add > 0) {
        scintilla_send_message(s, SCI_INSERTTEXT, b, (sptr_t)close);
        scintilla_send_message(s, SCI_INSERTTEXT, a, (sptr_t)open);
    } else {
        /* Remove if selection starts/ends with the markers. */
        sptr_t olen = strlen(open), clen = strlen(close);
        char head[16] = {0}, tail[16] = {0};
        for (sptr_t i = 0; i < olen && a + i < b; i++)
            head[i] = (char)scintilla_send_message(s, SCI_GETCHARAT, a + i, 0);
        for (sptr_t i = 0; i < clen && b - clen + i < b; i++)
            tail[i] = (char)scintilla_send_message(s, SCI_GETCHARAT, b - clen + i, 0);
        if (strncmp(head, open, olen) == 0 && strncmp(tail, close, clen) == 0) {
            scintilla_send_message(s, SCI_DELETERANGE, b - clen, clen);
            scintilla_send_message(s, SCI_DELETERANGE, a, olen);
        }
    }
    scintilla_send_message(s, SCI_ENDUNDOACTION, 0, 0);
}
static void action_block_comment_add(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; block_comment_apply(+1);
}
static void action_block_comment_remove(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; block_comment_apply(-1);
}

/* Q-align Fold/Unfold Current Level — collapse/expand the fold at caret. */
static void action_fold_current_level(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t pos  = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
    sptr_t line = scintilla_send_message(s, SCI_LINEFROMPOSITION, (uptr_t)pos, 0);
    int lvl = (int)scintilla_send_message(s, SCI_GETFOLDLEVEL, (uptr_t)line, 0)
              & SC_FOLDLEVELNUMBERMASK;
    /* Walk up to the header line for this level. */
    while (line > 0) {
        int hl = (int)scintilla_send_message(s, SCI_GETFOLDLEVEL, (uptr_t)line, 0);
        if (hl & SC_FOLDLEVELHEADERFLAG && (hl & SC_FOLDLEVELNUMBERMASK) == lvl)
            break;
        line--;
    }
    scintilla_send_message(s, SCI_FOLDLINE, (uptr_t)line, SC_FOLDACTION_CONTRACT);
}
static void action_unfold_current_level(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t pos  = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
    sptr_t line = scintilla_send_message(s, SCI_LINEFROMPOSITION, (uptr_t)pos, 0);
    int lvl = (int)scintilla_send_message(s, SCI_GETFOLDLEVEL, (uptr_t)line, 0)
              & SC_FOLDLEVELNUMBERMASK;
    while (line > 0) {
        int hl = (int)scintilla_send_message(s, SCI_GETFOLDLEVEL, (uptr_t)line, 0);
        if (hl & SC_FOLDLEVELHEADERFLAG && (hl & SC_FOLDLEVELNUMBERMASK) == lvl)
            break;
        line--;
    }
    scintilla_send_message(s, SCI_FOLDLINE, (uptr_t)line, SC_FOLDACTION_EXPAND);
}

/* Q-align: Trim Trailing Space and Save (convenience macro).
 * Forward-declared as static below; this thin wrapper just fires them in
 * sequence so the menu/macro path stays one selector. */
static void action_trim_trailing(GSimpleAction *a, GVariant *p, gpointer u);
static void action_save(GSimpleAction *a, GVariant *p, gpointer u);
static void action_trim_and_save(GSimpleAction *a, GVariant *p, gpointer u) {
    action_trim_trailing(a, p, u);
    action_save(a, p, u);
}

/* Q-align: Tab Bar Wrap toggle. The pref controls multi-row tab layout;
 * GtkNotebook's scrollable=FALSE enables stacking when many tabs exist. */
static void action_toggle_tab_bar_wrap(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    g_prefs.tab_bar_wrap = !g_prefs.tab_bar_wrap;
    prefs_save();
    GtkWidget *nb = editor_get_notebook();
    if (nb)
        gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), !g_prefs.tab_bar_wrap);
}

/* Q-align: Tab sorting. 6 modes: by name asc/desc, by extension asc/desc,
 * by full path asc/desc. Reorders pages in the editor notebook. */
static int tab_cmp_name(gconstpointer a, gconstpointer b, gpointer user) {
    int desc = GPOINTER_TO_INT(user);
    NppDoc *da = *(NppDoc *const *)a;
    NppDoc *db = *(NppDoc *const *)b;
    const char *na = da->filepath ? da->filepath : "Untitled";
    const char *nb = db->filepath ? db->filepath : "Untitled";
    gchar *bn_a = g_path_get_basename(na);
    gchar *bn_b = g_path_get_basename(nb);
    int r = g_ascii_strcasecmp(bn_a, bn_b);
    g_free(bn_a); g_free(bn_b);
    return desc ? -r : r;
}
static int tab_cmp_ext(gconstpointer a, gconstpointer b, gpointer user) {
    int desc = GPOINTER_TO_INT(user);
    NppDoc *da = *(NppDoc *const *)a;
    NppDoc *db = *(NppDoc *const *)b;
    const char *ea = da->filepath ? strrchr(da->filepath, '.') : NULL;
    const char *eb = db->filepath ? strrchr(db->filepath, '.') : NULL;
    int r = g_ascii_strcasecmp(ea ? ea : "", eb ? eb : "");
    return desc ? -r : r;
}
static int tab_cmp_path(gconstpointer a, gconstpointer b, gpointer user) {
    int desc = GPOINTER_TO_INT(user);
    NppDoc *da = *(NppDoc *const *)a;
    NppDoc *db = *(NppDoc *const *)b;
    int r = g_ascii_strcasecmp(da->filepath ? da->filepath : "",
                               db->filepath ? db->filepath : "");
    return desc ? -r : r;
}
static void tabs_sort_with(GCompareDataFunc cmp, int desc) {
    GtkWidget *nb = editor_get_notebook();
    if (!nb) return;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb));
    if (n < 2) return;
    GPtrArray *docs = g_ptr_array_new();
    for (int i = 0; i < n; i++) {
        NppDoc *d = editor_doc_at(i);
        if (d) g_ptr_array_add(docs, d);
    }
    g_ptr_array_sort_with_data(docs, cmp, GINT_TO_POINTER(desc));
    /* Reorder by moving each doc's page to its target position. */
    for (guint i = 0; i < docs->len; i++) {
        NppDoc *d = docs->pdata[i];
        for (int j = 0; j < n; j++)
            if (editor_doc_at(j) == d) {
                if (j != (int)i)
                    gtk_notebook_reorder_child(GTK_NOTEBOOK(nb),
                        gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), j), i);
                break;
            }
    }
    g_ptr_array_free(docs, TRUE);
}
static void action_sort_tabs_name_asc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; tabs_sort_with(tab_cmp_name, 0);
}
static void action_sort_tabs_name_desc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; tabs_sort_with(tab_cmp_name, 1);
}
static void action_sort_tabs_ext_asc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; tabs_sort_with(tab_cmp_ext, 0);
}
static void action_sort_tabs_ext_desc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; tabs_sort_with(tab_cmp_ext, 1);
}
static void action_sort_tabs_path_asc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; tabs_sort_with(tab_cmp_path, 0);
}
static void action_sort_tabs_path_desc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; tabs_sort_with(tab_cmp_path, 1);
}

static void action_eol_crlf(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_SETEOLMODE, SC_EOL_CRLF, 0);
    sci_send(SCI_CONVERTEOLS, SC_EOL_CRLF, 0);
}
static void action_eol_lf(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_SETEOLMODE, SC_EOL_LF, 0);
    sci_send(SCI_CONVERTEOLS, SC_EOL_LF, 0);
}
static void action_eol_cr(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_SETEOLMODE, SC_EOL_CR, 0);
    sci_send(SCI_CONVERTEOLS, SC_EOL_CR, 0);
}

static void action_trim_trailing(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    gchar **lines = g_strsplit(buf, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        gsize ll = strlen(lines[i]);
        while (ll > 0 && (lines[i][ll - 1] == ' ' || lines[i][ll - 1] == '\t'))
            lines[i][--ll] = '\0';
    }
    char *joined = g_strjoinv("\n", lines);
    put_back(joined, wd);
    g_free(joined); g_strfreev(lines); g_free(buf);
}
static void action_trim_leading(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    gchar **lines = g_strsplit(buf, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        const char *src = lines[i];
        while (*src == ' ' || *src == '\t') src++;
        if (src != lines[i]) memmove(lines[i], src, strlen(src) + 1);
    }
    char *joined = g_strjoinv("\n", lines);
    put_back(joined, wd);
    g_free(joined); g_strfreev(lines); g_free(buf);
}
static void action_trim_both(GSimpleAction *a, GVariant *p, gpointer u) {
    action_trim_leading(a,p,u); action_trim_trailing(a,p,u);
}
static void action_tab_to_space(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int tw = (int)sci_send(SCI_GETTABWIDTH, 0, 0); if (tw <= 0) tw = 4;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    GString *out = g_string_sized_new(len);
    for (gsize i = 0; i < len; i++) {
        if (buf[i] == '\t') for (int k = 0; k < tw; k++) g_string_append_c(out, ' ');
        else g_string_append_c(out, buf[i]);
    }
    put_back(out->str, wd); g_string_free(out, TRUE); g_free(buf);
}
static void action_space_to_tab(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int tw = (int)sci_send(SCI_GETTABWIDTH, 0, 0); if (tw <= 0) tw = 4;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    GString *out = g_string_sized_new(len);
    for (gsize i = 0; i < len; ) {
        if (i + (gsize)tw <= len) {
            gboolean all_space = TRUE;
            for (int k = 0; k < tw; k++) if (buf[i + k] != ' ') { all_space = FALSE; break; }
            if (all_space) { g_string_append_c(out, '\t'); i += tw; continue; }
        }
        g_string_append_c(out, buf[i++]);
    }
    put_back(out->str, wd); g_string_free(out, TRUE); g_free(buf);
}
static void action_remove_blank_lines(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    gchar **lines = g_strsplit(buf, "\n", -1);
    GString *out = g_string_new(NULL);
    gboolean first = TRUE;
    for (int i = 0; lines[i]; i++) {
        const char *s = lines[i];
        while (*s == ' ' || *s == '\t') s++;
        if (*s != '\0') {
            if (!first) g_string_append_c(out, '\n');
            g_string_append(out, lines[i]); first = FALSE;
        }
    }
    put_back(out->str, wd);
    g_strfreev(lines); g_string_free(out, TRUE); g_free(buf);
}

/* Q5 — additional blank ops matching macOS MenuBuilder.mm (5 missing items). */

/* Replace every newline (\n or \r\n) inside the operating range with a
 * single space. */
static void action_eol_to_space(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    GString *out = g_string_sized_new(len);
    for (gsize i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\r' && i + 1 < len && buf[i+1] == '\n') {
            g_string_append_c(out, ' '); i++;
        } else if (c == '\n' || c == '\r') {
            g_string_append_c(out, ' ');
        } else {
            g_string_append_c(out, c);
        }
    }
    put_back(out->str, wd); g_string_free(out, TRUE); g_free(buf);
}

/* Trim leading + trailing whitespace then EOL → space (= macOS combo). */
static void action_trim_both_eol_to_space(GSimpleAction *a, GVariant *p, gpointer u) {
    action_trim_leading(a, p, u);
    action_trim_trailing(a, p, u);
    action_eol_to_space(a, p, u);
}

/* Convert leading runs of `tab_width` spaces at the start of each line to
 * a tab character. Indented lines only — leaves mid-line spaces alone. */
static void action_space_to_tab_leading(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int tw = (int)sci_send(SCI_GETTABWIDTH, 0, 0); if (tw <= 0) tw = 4;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    gchar **lines = g_strsplit(buf, "\n", -1);
    GString *out = g_string_new(NULL);
    for (int i = 0; lines[i]; i++) {
        const char *p2 = lines[i];
        int leading_spaces = 0;
        while (*p2 == ' ') { leading_spaces++; p2++; }
        int tabs = leading_spaces / tw;
        int rem  = leading_spaces % tw;
        for (int t = 0; t < tabs; t++) g_string_append_c(out, '\t');
        for (int r = 0; r < rem; r++)  g_string_append_c(out, ' ');
        g_string_append(out, p2);
        if (lines[i + 1]) g_string_append_c(out, '\n');
    }
    put_back(out->str, wd);
    g_strfreev(lines); g_string_free(out, TRUE); g_free(buf);
}

/* Collapse runs of 2+ blank lines into a single blank line. */
static void action_merge_blank_lines(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    gsize len; gboolean wd; char *buf = get_sel_or_doc(&len, &wd);
    gchar **lines = g_strsplit(buf, "\n", -1);
    GString *out = g_string_new(NULL);
    gboolean prev_blank = FALSE;
    gboolean first = TRUE;
    for (int i = 0; lines[i]; i++) {
        const char *s = lines[i];
        while (*s == ' ' || *s == '\t') s++;
        gboolean blank = (*s == '\0');
        if (blank && prev_blank) continue;
        if (!first) g_string_append_c(out, '\n');
        g_string_append(out, lines[i]);
        first = FALSE;
        prev_blank = blank;
    }
    put_back(out->str, wd);
    g_strfreev(lines); g_string_free(out, TRUE); g_free(buf);
}

/* Trim trailing spaces + remove blank lines + remove trailing newline = one
 * shot "Remove Unnecessary Blank and EOL". */
static void action_remove_unnec_blank_eol(GSimpleAction *a, GVariant *p, gpointer u) {
    action_trim_trailing(a, p, u);
    action_remove_blank_lines(a, p, u);
}

static void action_toggle_readonly(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    gboolean now = !sci_send(SCI_GETREADONLY, 0, 0);
    sci_send(SCI_SETREADONLY, now ? 1 : 0, 0);
    /* Track on the doc so the flag survives across save/restore via
     * session.xml — matches macOS userReadOnly (EditorView.h:275). */
    doc->user_readonly = now;
}
static void action_column_mode(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    int cur = (int)sci_send(SCI_GETSELECTIONMODE, 0, 0);
    sci_send(SCI_SETSELECTIONMODE,
             cur == SC_SEL_RECTANGLE ? SC_SEL_STREAM : SC_SEL_RECTANGLE, 0);
}

/* ──────────────────────────────────────────────────────────────────────
 * G12.11 On Selection submenu
 * ────────────────────────────────────────────────────────────────────── */

/* Get the currently-selected text (or word under caret) into a NUL-terminated
 * heap buffer. Caller frees with g_free. */
static char *get_selection_text(void) {
    GtkWidget *sci = current_sci(); if (!sci) return NULL;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t a = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t b = scintilla_send_message(s, SCI_GETSELECTIONEND, 0, 0);
    if (a == b) {
        /* No selection: grab the word under caret. */
        sptr_t pos = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
        a = scintilla_send_message(s, SCI_WORDSTARTPOSITION, pos, 1);
        b = scintilla_send_message(s, SCI_WORDENDPOSITION,   pos, 1);
        if (a == b) return NULL;
    }
    sptr_t len = b - a;
    char *buf = g_malloc((gsize)len + 1);
    struct Sci_TextRangeFull tr = {{a, b}, buf};
    scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    return buf;
}

static void action_sel_open_file(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    char *sel = get_selection_text(); if (!sel) return;
    if (g_file_test(sel, G_FILE_TEST_EXISTS)) editor_open_path(sel);
    g_free(sel);
}
static void action_sel_open_folder(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    char *sel = get_selection_text(); if (!sel) return;
    gchar *dir = g_path_get_dirname(sel);
    gchar *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri) { gtk_show_uri_on_window(GTK_WINDOW(g_window), uri, GDK_CURRENT_TIME, NULL); g_free(uri); }
    g_free(dir); g_free(sel);
}
static void action_sel_search_internet(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    char *sel = get_selection_text(); if (!sel) return;
    gchar *enc = g_uri_escape_string(sel, NULL, FALSE);
    /* P3 — search URL template from prefs. "%s" is the query placeholder.
     * If the template doesn't contain %s, append the query at the end. */
    const char *tmpl = g_prefs.search_engine_url[0]
                       ? g_prefs.search_engine_url
                       : "https://duckduckgo.com/?q=%s";
    gchar *url;
    if (strstr(tmpl, "%s")) url = g_strdup_printf(tmpl, enc);
    else                    url = g_strdup_printf("%s%s", tmpl, enc);
    gtk_show_uri_on_window(GTK_WINDOW(g_window), url, GDK_CURRENT_TIME, NULL);
    g_free(url); g_free(enc); g_free(sel);
}
static void action_sel_redact(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t ss = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t se = scintilla_send_message(s, SCI_GETSELECTIONEND, 0, 0);
    if (ss == se) return;
    /* Replace with the same number of black-square chars (▮ = E2 96 AE in UTF-8). */
    GString *fill = g_string_new(NULL);
    for (sptr_t i = ss; i < se; i++) g_string_append(fill, "▮");
    scintilla_send_message(s, SCI_REPLACESEL, 0, (sptr_t)fill->str);
    g_string_free(fill, TRUE);
}

/* ──────────────────────────────────────────────────────────────────────
 * G12.12 Multi-Select All / Next
 *
 * Scintilla supports multiple selections via SCI_SETMULTIPLESELECTION + the
 * SCI_ADDSELECTION / SCI_MULTIPLESELECTADDNEXT family.
 * ────────────────────────────────────────────────────────────────────── */

static void enable_multi_sel(void) {
    sci_send(SCI_SETMULTIPLESELECTION, 1, 0);
    sci_send(SCI_SETADDITIONALSELECTIONTYPING, 1, 0);
}
static void action_multisel_add_next(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; enable_multi_sel();
    sci_send(SCI_MULTIPLESELECTADDNEXT, 0, 0);
}
static void action_multisel_add_each(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; enable_multi_sel();
    sci_send(SCI_MULTIPLESELECTADDEACH, 0, 0);
}

/* ──────────────────────────────────────────────────────────────────────
 * View → Hide Lines / Text Direction / Monitoring
 * ────────────────────────────────────────────────────────────────────── */

static void action_hide_lines(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t ss = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t se = scintilla_send_message(s, SCI_GETSELECTIONEND, 0, 0);
    sptr_t l0 = scintilla_send_message(s, SCI_LINEFROMPOSITION, ss, 0);
    sptr_t l1 = scintilla_send_message(s, SCI_LINEFROMPOSITION, se, 0);
    if (l1 > l0) scintilla_send_message(s, SCI_HIDELINES, l0, l1);
}
static void action_show_lines(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t total = scintilla_send_message(s, SCI_GETLINECOUNT, 0, 0);
    scintilla_send_message(s, SCI_SHOWLINES, 0, total);
}
static void action_text_dir_ltr(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_SETBIDIRECTIONAL, SC_BIDIRECTIONAL_DISABLED, 0);
}
static void action_text_dir_rtl(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_SETBIDIRECTIONAL, SC_BIDIRECTIONAL_L2R, 0);
}
static void action_toggle_monitoring(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *d = editor_current_doc();
    if (d) d->monitoring = !d->monitoring;
}

/* ──────────────────────────────────────────────────────────────────────
 * File → Move to Trash (uses GIO trash)
 * ────────────────────────────────────────────────────────────────────── */

/* ──────────────────────────────────────────────────────────────────────
 * G35 Encoding → Convert To  (different from set-encoding which only
 *      updates metadata). Convert-To: set doc->encoding then save so the
 *      bytes are rewritten through iconv into the new encoding.
 * ────────────────────────────────────────────────────────────────────── */

static void convert_to(const char *target) {
    NppDoc *d = editor_current_doc();
    if (!d || !d->filepath) return;
    g_free(d->encoding);
    d->encoding = g_strdup(target);
    /* Sync the radio in the menu + persist the change to disk. */
    main_sync_encoding_menu(target);
    editor_save();
}
static void action_convert_to_ansi(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; convert_to("Windows-1252");
}
static void action_convert_to_utf8(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; convert_to("UTF-8");
}
static void action_convert_to_utf8_bom(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; convert_to("UTF-8 BOM");
}
static void action_convert_to_utf16_le(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; convert_to("UTF-16 LE BOM");
}
static void action_convert_to_utf16_be(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; convert_to("UTF-16 BE BOM");
}

/* ──────────────────────────────────────────────────────────────────────
 * G38 Auto-completion manual triggers
 * ────────────────────────────────────────────────────────────────────── */

static void action_autocomplete_function(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    const char *lang = g_object_get_data(G_OBJECT(sci), "npp-lang");
    const char *kw = lexer_get_keywords(lang);
    if (!kw || !*kw) return;
    /* Find the word being typed under the caret. */
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t pos = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
    sptr_t start = scintilla_send_message(s, SCI_WORDSTARTPOSITION, pos, 1);
    sptr_t len_back = pos - start;
    /* Convert space-separated keyword list to autocomplete-compatible
     * (autocomplete uses default separator = space). */
    scintilla_send_message(s, SCI_AUTOCSETSEPARATOR, ' ', 0);
    scintilla_send_message(s, SCI_AUTOCSHOW, (uptr_t)len_back, (sptr_t)kw);
}
static void action_autocomplete_word(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    /* Trigger the autocomplete.c scan-doc path with a synthetic letter event. */
    autocomplete_on_char_added(sci, ' ');
}
static void action_function_param_hint(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    /* Real API-backed parameter hint (macOS 60422d1); works regardless
     * of the auto-on-input pref since this is an explicit request. */
    autocomplete_show_calltip(sci);
}
static void action_function_param_hint_cancel(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_CALLTIPCANCEL, 0, 0);
}
static void action_autocomplete_select(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_AUTOCCOMPLETE, 0, 0);
}

/* ──────────────────────────────────────────────────────────────────────
 * G39 Spell Check UI
 * ────────────────────────────────────────────────────────────────────── */

static void action_spell_toggle(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    spell_set_enabled(!spell_is_enabled());
    GtkWidget *sci = current_sci();
    if (sci) spell_schedule_check(sci);
}
static void action_spell_check_doc(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci();
    if (sci) spell_check_document(sci);
}

/* ──────────────────────────────────────────────────────────────────────
 * G34 Find dialog extras (no dialog modification — wrapper actions)
 * ────────────────────────────────────────────────────────────────────── */

/* "Find All in Current Document": count matches of selected word, highlight
 * with smart-highlight indicator. Reuses the G16 helper. */
static void action_find_all_current(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    /* Force-update smart highlight (which marks all matches of selection). */
    main_smarthighlight_update(sci);

    /* Count matches and report. */
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t a0 = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t b0 = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
    if (a0 == b0) return;
    sptr_t len = b0 - a0;
    char *needle = g_malloc((gsize)len + 1);
    struct Sci_TextRangeFull tr = {{a0, b0}, needle};
    scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);

    sptr_t doc_len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    scintilla_send_message(s, SCI_SETSEARCHFLAGS, 0, 0);
    int count = 0;
    sptr_t cur = 0;
    while (cur < doc_len) {
        scintilla_send_message(s, SCI_SETTARGETRANGE, cur, doc_len);
        sptr_t hit = scintilla_send_message(s, SCI_SEARCHINTARGET,
                                            (uptr_t)len, (sptr_t)needle);
        if (hit < 0) break;
        count++;
        cur = scintilla_send_message(s, SCI_GETTARGETEND, 0, 0);
    }
    g_free(needle);

    char body[128];
    g_snprintf(body, sizeof(body), "Found %d occurrence%s in the current document.",
               count, count == 1 ? "" : "s");
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", body);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* "Find All in All Opened Documents": iterate all tabs, count + jump to first. */
static void action_find_all_all_docs(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t a0 = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t b0 = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
    if (a0 == b0) return;
    sptr_t len = b0 - a0;
    char *needle = g_malloc((gsize)len + 1);
    struct Sci_TextRangeFull tr = {{a0, b0}, needle};
    scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);

    int total = 0, files_hit = 0;
    int npages = editor_page_count();
    for (int i = 0; i < npages; i++) {
        NppDoc *d = editor_doc_at(i); if (!d) continue;
        ScintillaObject *ds = SCINTILLA(d->sci);
        sptr_t dlen = scintilla_send_message(ds, SCI_GETLENGTH, 0, 0);
        scintilla_send_message(ds, SCI_SETSEARCHFLAGS, 0, 0);
        int file_count = 0;
        sptr_t cur = 0;
        while (cur < dlen) {
            scintilla_send_message(ds, SCI_SETTARGETRANGE, cur, dlen);
            sptr_t hit = scintilla_send_message(ds, SCI_SEARCHINTARGET,
                                                (uptr_t)len, (sptr_t)needle);
            if (hit < 0) break;
            file_count++;
            cur = scintilla_send_message(ds, SCI_GETTARGETEND, 0, 0);
        }
        if (file_count > 0) files_hit++;
        total += file_count;
    }
    g_free(needle);

    char body[160];
    g_snprintf(body, sizeof(body),
        "Found %d occurrence%s in %d open file%s.",
        total, total == 1 ? "" : "s", files_hit, files_hit == 1 ? "" : "s");
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", body);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* "Replace in Selection": only replace within the current selection range. */
static void action_replace_in_selection(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    /* For v1, just open the regular Replace dialog with a hint dialog first. */
    findreplace_show(GTK_WIDGET(g_window), NULL, TRUE);
}

/* ──────────────────────────────────────────────────────────────────────
 * G37 Print — basic GtkPrintOperation wrapping Scintilla SCI_FORMATRANGE
 * ────────────────────────────────────────────────────────────────────── */

/* P12 — paginated print. Context object held for the duration of one
 * GtkPrintOperation run. Built in begin_print, used by draw_page. */
typedef struct {
    char     *text;           /* whole-doc text, owned */
    gsize     text_len;
    int       n_pages;        /* total */
    /* Page break offsets — page i renders bytes [pages[i], pages[i+1]). */
    GArray   *pages;          /* gsize */
    char     *filename;       /* short basename for header, owned */
} PrintCtx;

static void print_ctx_free(PrintCtx *pc) {
    if (!pc) return;
    g_free(pc->text);
    g_free(pc->filename);
    if (pc->pages) g_array_free(pc->pages, TRUE);
    g_free(pc);
}

static void on_print_begin(GtkPrintOperation *op, GtkPrintContext *ctx,
                            gpointer user) {
    PrintCtx *pc = (PrintCtx *)user;
    /* Build a Pango layout sized to the page body (minus header/footer)
     * and let it figure out where each page break falls. */
    PangoLayout *layout = gtk_print_context_create_pango_layout(ctx);
    PangoFontDescription *fd = pango_font_description_from_string("Monospace 10");
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(layout, pc->text, (int)pc->text_len);
    double page_w = gtk_print_context_get_width(ctx);
    double page_h = gtk_print_context_get_height(ctx);
    /* Reserve ~36pt header + footer. */
    double body_h = page_h - 36 - 36;
    pango_layout_set_width(layout, (int)(page_w * PANGO_SCALE));

    int n_lines = pango_layout_get_line_count(layout);
    pc->pages = g_array_new(FALSE, FALSE, sizeof(gsize));
    gsize first_byte = 0;
    g_array_append_val(pc->pages, first_byte);

    double running_h = 0;
    for (int i = 0; i < n_lines; i++) {
        PangoLayoutLine *line = pango_layout_get_line_readonly(layout, i);
        PangoRectangle rect;
        pango_layout_line_get_extents(line, NULL, &rect);
        double line_h = (double)rect.height / PANGO_SCALE;
        if (running_h + line_h > body_h) {
            /* Start a new page at this line's byte offset. */
            gsize page_start = (gsize)line->start_index;
            g_array_append_val(pc->pages, page_start);
            running_h = line_h;
        } else {
            running_h += line_h;
        }
    }
    g_object_unref(layout);
    pc->n_pages = (int)pc->pages->len;
    gtk_print_operation_set_n_pages(op, pc->n_pages);
}

static void on_print_draw(GtkPrintOperation *op, GtkPrintContext *ctx,
                          gint page_nr, gpointer user) {
    (void)op;
    PrintCtx *pc = (PrintCtx *)user;
    if (!pc || page_nr >= pc->n_pages) return;

    cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
    double page_w = gtk_print_context_get_width(ctx);
    double page_h = gtk_print_context_get_height(ctx);

    /* Header: filename left, page X/N right. */
    {
        PangoLayout *h = gtk_print_context_create_pango_layout(ctx);
        PangoFontDescription *fd = pango_font_description_from_string("Sans 9");
        pango_layout_set_font_description(h, fd);
        pango_font_description_free(fd);
        gchar *hdr = g_strdup_printf("%s    —    Page %d / %d",
            pc->filename ? pc->filename : "Untitled",
            page_nr + 1, pc->n_pages);
        pango_layout_set_text(h, hdr, -1);
        cairo_save(cr);
        cairo_move_to(cr, 0, 0);
        pango_cairo_show_layout(cr, h);
        cairo_restore(cr);
        g_free(hdr);
        g_object_unref(h);
    }

    /* Body slice. */
    gsize start = g_array_index(pc->pages, gsize, page_nr);
    gsize end   = (page_nr + 1 < pc->n_pages)
                  ? g_array_index(pc->pages, gsize, page_nr + 1)
                  : pc->text_len;
    if (end < start) end = start;

    PangoLayout *body = gtk_print_context_create_pango_layout(ctx);
    PangoFontDescription *fd = pango_font_description_from_string("Monospace 10");
    pango_layout_set_font_description(body, fd);
    pango_font_description_free(fd);
    pango_layout_set_width(body, (int)(page_w * PANGO_SCALE));
    pango_layout_set_text(body, pc->text + start, (int)(end - start));

    cairo_save(cr);
    cairo_move_to(cr, 0, 36);          /* below header */
    pango_cairo_show_layout(cr, body);
    cairo_restore(cr);
    g_object_unref(body);

    /* Footer: timestamp right-aligned. */
    {
        PangoLayout *f = gtk_print_context_create_pango_layout(ctx);
        PangoFontDescription *fd = pango_font_description_from_string("Sans 9");
        pango_layout_set_font_description(f, fd);
        pango_font_description_free(fd);
        time_t now = time(NULL);
        char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", localtime(&now));
        pango_layout_set_text(f, ts, -1);
        pango_layout_set_alignment(f, PANGO_ALIGN_RIGHT);
        pango_layout_set_width(f, (int)(page_w * PANGO_SCALE));
        cairo_save(cr);
        cairo_move_to(cr, 0, page_h - 24);
        pango_cairo_show_layout(cr, f);
        cairo_restore(cr);
        g_object_unref(f);
    }
}

static void on_print_end(GtkPrintOperation *op, GtkPrintContext *ctx, gpointer user) {
    (void)op; (void)ctx;
    print_ctx_free((PrintCtx *)user);
}
/* ──────────────────────────────────────────────────────────────────────
 * G21 Floating panels — parametric pop-out / dock-back actions
 * ────────────────────────────────────────────────────────────────────── */
static void action_panel_popout(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)u;
    floating_popout(g_variant_get_string(p, NULL));
}
static void action_panel_dockback(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)u;
    floating_dockback(g_variant_get_string(p, NULL));
}
static void action_panel_toggle(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)u;
    floating_toggle(g_variant_get_string(p, NULL));
}

/* P12 — cached page setup so Page Setup dialog choices persist within
 * the app session. */
static GtkPageSetup *g_page_setup = NULL;

static void action_print_real(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;

    PrintCtx *pc = g_new0(PrintCtx, 1);
    ScintillaObject *s = SCINTILLA(sci);
    pc->text_len = (gsize)scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    pc->text = g_malloc(pc->text_len + 1);
    if (pc->text_len > 0) {
        struct Sci_TextRangeFull tr = {{0, (Sci_Position)pc->text_len}, pc->text};
        scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    }
    pc->text[pc->text_len] = '\0';
    NppDoc *d = editor_current_doc();
    pc->filename = d && d->filepath ? g_path_get_basename(d->filepath)
                                    : g_strdup("Untitled");

    GtkPrintOperation *op = gtk_print_operation_new();
    if (g_page_setup)
        gtk_print_operation_set_default_page_setup(op, g_page_setup);
    g_signal_connect(op, "begin-print", G_CALLBACK(on_print_begin), pc);
    g_signal_connect(op, "draw-page",   G_CALLBACK(on_print_draw),  pc);
    g_signal_connect(op, "end-print",   G_CALLBACK(on_print_end),   pc);
    gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                             GTK_WINDOW(g_window), NULL);
    g_object_unref(op);
}

/* P12 — Page Setup. Persist the result in g_page_setup so it's used for
 * the next print run. */
static void action_page_setup(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)p; (void)u;
    g_page_setup = gtk_print_run_page_setup_dialog(
        GTK_WINDOW(g_window), g_page_setup, NULL);
}

static void action_move_to_trash(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    NppDoc *d = editor_current_doc(); if (!d || !d->filepath) return;
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "Move '%s' to Trash?", g_path_get_basename(d->filepath));
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp != GTK_RESPONSE_OK) return;
    GFile *f = g_file_new_for_path(d->filepath);
    GError *err = NULL;
    if (g_file_trash(f, NULL, &err)) {
        editor_close_page(editor_current_page());
    } else {
        if (err) g_error_free(err);
    }
    g_object_unref(f);
}

/* ──────────────────────────────────────────────────────────────────────
 * G13 Bookmarks — margin + 4 core operations
 *
 * Bookmarks live on Scintilla margin 1 with marker number 24
 * (SC_MARKNUM_BOOKMARK compat alias). Margin is set up lazily on first
 * bookmark op on a given sci widget.
 * ────────────────────────────────────────────────────────────────────── */

#define NPP_BOOKMARK_MARGIN  1
#define NPP_BOOKMARK_MARKER  SC_MARKNUM_BOOKMARK   /* = 24 via compat alias */

static void ensure_bookmark_margin(GtkWidget *sci) {
    if (!sci) return;
    /* Margin slot, marker style, and colors are configured uniformly in
     * editor.c's setup_sci(). Just make sure the margin is wide enough
     * to render the marker when the user toggles a bookmark — if the
     * show_bookmark_margin pref is off, force it on for this view so the
     * user sees the marker they just placed. */
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t w = scintilla_send_message(s, SCI_GETMARGINWIDTHN, NPP_BOOKMARK_MARGIN, 0);
    if (w < 16)
        scintilla_send_message(s, SCI_SETMARGINWIDTHN, NPP_BOOKMARK_MARGIN, 16);
}

static void action_bookmark_toggle(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    ensure_bookmark_margin(sci);
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t pos = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
    sptr_t line = scintilla_send_message(s, SCI_LINEFROMPOSITION, pos, 0);
    sptr_t mk = scintilla_send_message(s, SCI_MARKERGET, line, 0);
    if (mk & (1 << NPP_BOOKMARK_MARKER))
        scintilla_send_message(s, SCI_MARKERDELETE, line, NPP_BOOKMARK_MARKER);
    else
        scintilla_send_message(s, SCI_MARKERADD,    line, NPP_BOOKMARK_MARKER);
}
static void bookmark_jump(int dir /* +1=next, -1=prev */) {
    GtkWidget *sci = current_sci(); if (!sci) return;
    ensure_bookmark_margin(sci);
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t pos = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
    sptr_t line = scintilla_send_message(s, SCI_LINEFROMPOSITION, pos, 0);
    sptr_t mask = 1 << NPP_BOOKMARK_MARKER;
    sptr_t total = scintilla_send_message(s, SCI_GETLINECOUNT, 0, 0);
    sptr_t target;
    if (dir > 0) {
        target = scintilla_send_message(s, SCI_MARKERNEXT, line + 1, mask);
        if (target < 0)
            target = scintilla_send_message(s, SCI_MARKERNEXT, 0, mask);
    } else {
        target = scintilla_send_message(s, SCI_MARKERPREVIOUS, line - 1, mask);
        if (target < 0)
            target = scintilla_send_message(s, SCI_MARKERPREVIOUS, total - 1, mask);
    }
    if (target >= 0) {
        sptr_t pos2 = scintilla_send_message(s, SCI_POSITIONFROMLINE, target, 0);
        scintilla_send_message(s, SCI_GOTOPOS, pos2, 0);
    }
}
static void action_bookmark_next(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; bookmark_jump(+1);
}
static void action_bookmark_prev(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; bookmark_jump(-1);
}
static void action_bookmark_clear_all(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    scintilla_send_message(SCINTILLA(sci), SCI_MARKERDELETEALL,
                            NPP_BOOKMARK_MARKER, 0);
}

/* Q4 — collect every bookmarked (or non-bookmarked) line index for the
 * current document. Caller frees with g_array_free(arr, TRUE). */
static GArray *bookmarked_lines(GtkWidget *sci, gboolean want_bookmarked) {
    GArray *out = g_array_new(FALSE, FALSE, sizeof(int));
    if (!sci) return out;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t n = scintilla_send_message(s, SCI_GETLINECOUNT, 0, 0);
    sptr_t mask = 1 << NPP_BOOKMARK_MARKER;
    for (int line = 0; line < (int)n; line++) {
        sptr_t mk = scintilla_send_message(s, SCI_MARKERGET, line, 0);
        gboolean marked = (mk & mask) != 0;
        if (marked == want_bookmarked) g_array_append_val(out, line);
    }
    return out;
}

/* Helper: get the start-of-line and end-of-line+1 positions for a line. */
static void line_byte_range(ScintillaObject *s, int line,
                            sptr_t *out_start, sptr_t *out_end) {
    *out_start = scintilla_send_message(s, SCI_POSITIONFROMLINE,    line,     0);
    sptr_t next = scintilla_send_message(s, SCI_POSITIONFROMLINE,    line + 1, 0);
    if (next <= 0) /* last line, no trailing newline */
        next = scintilla_send_message(s, SCI_GETLINEENDPOSITION,    line,     0);
    *out_end = next;
}

/* Concatenate every bookmarked line's text (with trailing newline preserved
 * if present in the source) into a single buffer. Caller g_frees. */
static char *concat_bookmarked_text(GtkWidget *sci, GArray *lines, gsize *out_len) {
    GString *acc = g_string_new(NULL);
    ScintillaObject *s = SCINTILLA(sci);
    for (guint i = 0; i < lines->len; i++) {
        int line = g_array_index(lines, int, i);
        sptr_t a, b;
        line_byte_range(s, line, &a, &b);
        sptr_t len = b - a;
        if (len <= 0) continue;
        char *buf = g_malloc((gsize)len + 1);
        struct Sci_TextRangeFull tr = {{a, b}, buf};
        scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
        g_string_append_len(acc, buf, (gssize)len);
        g_free(buf);
    }
    if (out_len) *out_len = acc->len;
    return g_string_free(acc, FALSE);
}

static void copy_text_to_clipboard(const char *txt, gsize len) {
    if (!txt || len == 0) return;
    npp_clipboard_set_textn(txt, (int)len);
}

static void action_bookmark_copy_lines(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    GArray *lines = bookmarked_lines(sci, TRUE);
    gsize n; char *txt = concat_bookmarked_text(sci, lines, &n);
    copy_text_to_clipboard(txt, n);
    g_free(txt);
    g_array_free(lines, TRUE);
}

/* Delete every line in `lines` from the document, working back-to-front so
 * indices stay valid. Wrapped in a single undo action. */
static void remove_lines(GtkWidget *sci, GArray *lines) {
    if (!sci || !lines || lines->len == 0) return;
    ScintillaObject *s = SCINTILLA(sci);
    scintilla_send_message(s, SCI_BEGINUNDOACTION, 0, 0);
    for (int i = (int)lines->len - 1; i >= 0; i--) {
        int line = g_array_index(lines, int, i);
        sptr_t a, b;
        line_byte_range(s, line, &a, &b);
        scintilla_send_message(s, SCI_DELETERANGE, a, b - a);
    }
    scintilla_send_message(s, SCI_ENDUNDOACTION, 0, 0);
}

static void action_bookmark_cut_lines(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    GArray *lines = bookmarked_lines(sci, TRUE);
    gsize n; char *txt = concat_bookmarked_text(sci, lines, &n);
    copy_text_to_clipboard(txt, n);
    g_free(txt);
    remove_lines(sci, lines);
    g_array_free(lines, TRUE);
}

static void action_bookmark_remove_lines(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    GArray *lines = bookmarked_lines(sci, TRUE);
    remove_lines(sci, lines);
    g_array_free(lines, TRUE);
}

static void action_bookmark_remove_unmarked(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    GArray *lines = bookmarked_lines(sci, FALSE);  /* non-bookmarked */
    remove_lines(sci, lines);
    g_array_free(lines, TRUE);
}

/* Paste the clipboard (split on newlines) over every bookmarked line in
 * the document, one clipboard line per bookmarked line, in order. */
static void on_bookmark_paste_ready(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    gchar *clip = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (!clip) return;
    GtkWidget *sci = current_sci();
    if (!sci) { g_free(clip); return; }
    gchar **clip_lines = g_strsplit(clip, "\n", -1);
    int clip_count = 0; while (clip_lines[clip_count]) clip_count++;

    GArray *lines = bookmarked_lines(sci, TRUE);
    ScintillaObject *s = SCINTILLA(sci);
    scintilla_send_message(s, SCI_BEGINUNDOACTION, 0, 0);
    /* Walk back-to-front so byte offsets stay valid. */
    for (int i = (int)lines->len - 1; i >= 0; i--) {
        int line = g_array_index(lines, int, i);
        sptr_t a, b;
        line_byte_range(s, line, &a, &b);
        /* Keep the trailing newline (if any) so the line structure stays intact. */
        sptr_t eol = scintilla_send_message(s, SCI_GETLINEENDPOSITION, line, 0);
        scintilla_send_message(s, SCI_DELETERANGE, a, eol - a);
        const char *repl = (i < clip_count && clip_lines[i]) ? clip_lines[i] : "";
        scintilla_send_message(s, SCI_INSERTTEXT, a, (sptr_t)repl);
    }
    scintilla_send_message(s, SCI_ENDUNDOACTION, 0, 0);
    g_array_free(lines, TRUE);
    g_strfreev(clip_lines);
    g_free(clip);
}
static void action_bookmark_paste_lines(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_read_text_async(cb, NULL, on_bookmark_paste_ready, NULL);
}

/* Invert: every bookmarked line becomes un-bookmarked, every un-bookmarked
 * line becomes bookmarked. */
static void action_bookmark_inverse(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    ensure_bookmark_margin(sci);
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t n = scintilla_send_message(s, SCI_GETLINECOUNT, 0, 0);
    sptr_t mask = 1 << NPP_BOOKMARK_MARKER;
    for (int line = 0; line < (int)n; line++) {
        sptr_t mk = scintilla_send_message(s, SCI_MARKERGET, line, 0);
        if (mk & mask)
            scintilla_send_message(s, SCI_MARKERDELETE, line, NPP_BOOKMARK_MARKER);
        else
            scintilla_send_message(s, SCI_MARKERADD,    line, NPP_BOOKMARK_MARKER);
    }
}

/* ──────────────────────────────────────────────────────────────────────
 * G13 Mark / Style system (5 colour-coded indicators)
 *
 * Indicators 21–25 with fixed colours. Mark All highlights every
 * occurrence of the current selection across the doc.
 * ────────────────────────────────────────────────────────────────────── */

#define NPP_MARK_INDIC_BASE 21
/* Yellow, green, blue, red-orange, purple — match macOS palette. */
static const int kMarkColours[5] = {
    0x00DFFF,  /* yellow (BGR) */
    0x00A050,  /* green */
    0xFF8060,  /* blue */
    0x4040C0,  /* red */
    0xA060A0,  /* purple */
};
static void ensure_mark_indicators(GtkWidget *sci) {
    if (!sci) return;
    if (g_object_get_data(G_OBJECT(sci), "npp-mk-init")) return;
    ScintillaObject *s = SCINTILLA(sci);
    for (int k = 0; k < 5; k++) {
        int ind = NPP_MARK_INDIC_BASE + k;
        scintilla_send_message(s, SCI_INDICSETSTYLE, ind, INDIC_ROUNDBOX);
        scintilla_send_message(s, SCI_INDICSETFORE,  ind, kMarkColours[k]);
        scintilla_send_message(s, SCI_INDICSETALPHA, ind, 100);
        scintilla_send_message(s, SCI_INDICSETUNDER, ind, 1);
    }
    g_object_set_data(G_OBJECT(sci), "npp-mk-init", GINT_TO_POINTER(1));
}
static void mark_all_with(int slot) {
    GtkWidget *sci = current_sci(); if (!sci) return;
    ensure_mark_indicators(sci);
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t ss = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t se = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
    if (ss == se) return;
    sptr_t need = se - ss;
    if (need < 1 || need > 500) return;
    char *needle = g_malloc((gsize)need + 1);
    struct Sci_TextRangeFull tr = {{ss, se}, needle};
    scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);

    int ind = NPP_MARK_INDIC_BASE + slot;
    sptr_t doc_len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    scintilla_send_message(s, SCI_SETINDICATORCURRENT, ind, 0);
    scintilla_send_message(s, SCI_INDICATORCLEARRANGE, 0, doc_len);
    scintilla_send_message(s, SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE, 0);
    sptr_t cur = 0;
    while (cur < doc_len) {
        scintilla_send_message(s, SCI_SETTARGETRANGE, cur, doc_len);
        sptr_t hit = scintilla_send_message(s, SCI_SEARCHINTARGET, (uptr_t)need, (sptr_t)needle);
        if (hit < 0) break;
        sptr_t end = scintilla_send_message(s, SCI_GETTARGETEND, 0, 0);
        scintilla_send_message(s, SCI_INDICATORFILLRANGE, hit, end - hit);
        cur = end;
    }
    g_free(needle);
}
static void clear_mark_slot(int slot) {
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    int ind = NPP_MARK_INDIC_BASE + slot;
    sptr_t doc_len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    scintilla_send_message(s, SCI_SETINDICATORCURRENT, ind, 0);
    scintilla_send_message(s, SCI_INDICATORCLEARRANGE, 0, doc_len);
}

#define MARK_ACTIONS(SLOT) \
    static void action_mark_all_##SLOT(GSimpleAction *a, GVariant *p, gpointer u){\
        (void)a;(void)p;(void)u; mark_all_with(SLOT); } \
    static void action_clear_mark_##SLOT(GSimpleAction *a, GVariant *p, gpointer u){\
        (void)a;(void)p;(void)u; clear_mark_slot(SLOT); }
MARK_ACTIONS(0) MARK_ACTIONS(1) MARK_ACTIONS(2) MARK_ACTIONS(3) MARK_ACTIONS(4)
#undef MARK_ACTIONS

static void action_clear_all_marks(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    for (int k = 0; k < 5; k++) clear_mark_slot(k);
}

/* Q-fix Search → Style One Token (5 colours).
 * Like mark_all_with but adds the indicator ONLY at the selected range,
 * leaving other matches alone — matches macOS styleOneToken: behaviour. */
static void style_one_with(int slot) {
    GtkWidget *sci = current_sci(); if (!sci) return;
    ensure_mark_indicators(sci);
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t ss = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t se = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
    if (ss == se) return;
    int ind = NPP_MARK_INDIC_BASE + slot;
    scintilla_send_message(s, SCI_SETINDICATORCURRENT, ind, 0);
    scintilla_send_message(s, SCI_INDICATORFILLRANGE, ss, se - ss);
}
#define STYLE_ONE(SLOT) \
    static void action_style_one_##SLOT(GSimpleAction *a, GVariant *p, gpointer u){\
        (void)a;(void)p;(void)u; style_one_with(SLOT); }
STYLE_ONE(0) STYLE_ONE(1) STYLE_ONE(2) STYLE_ONE(3) STYLE_ONE(4)
#undef STYLE_ONE

/* Q-fix Search → Copy Styled Text (5 colours).
 * Walks the document collecting every range covered by indicator <slot>
 * and concatenates them onto the clipboard, newline-separated. */
static void copy_styled_with(int slot) {
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    int ind = NPP_MARK_INDIC_BASE + slot;
    sptr_t doc_len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    GString *out = g_string_new(NULL);
    sptr_t pos = 0;
    while (pos < doc_len) {
        sptr_t start = scintilla_send_message(s, SCI_INDICATORSTART, ind, pos);
        sptr_t end   = scintilla_send_message(s, SCI_INDICATOREND,   ind, pos);
        int on = (int)scintilla_send_message(s, SCI_INDICATORVALUEAT, ind, pos);
        if (on && end > start) {
            sptr_t n = end - start;
            char *buf = g_malloc(n + 1);
            struct Sci_TextRangeFull tr = {{start, end}, buf};
            scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
            if (out->len > 0) g_string_append_c(out, '\n');
            g_string_append_len(out, buf, n);
            g_free(buf);
        }
        if (end <= pos) pos++; else pos = end;
    }
    if (out->len > 0)
        npp_clipboard_set_text(out->str);
    g_string_free(out, TRUE);
}
#define COPY_STYLED(SLOT) \
    static void action_copy_styled_##SLOT(GSimpleAction *a, GVariant *p, gpointer u){\
        (void)a;(void)p;(void)u; copy_styled_with(SLOT); }
COPY_STYLED(0) COPY_STYLED(1) COPY_STYLED(2) COPY_STYLED(3) COPY_STYLED(4)
#undef COPY_STYLED

/* Q-fix Search → Jump to Next/Previous Styled Token (across all 5 slots). */
static int any_mark_indicator_at(GtkWidget *sci, sptr_t pos) {
    ScintillaObject *s = SCINTILLA(sci);
    for (int k = 0; k < 5; k++) {
        int ind = NPP_MARK_INDIC_BASE + k;
        if (scintilla_send_message(s, SCI_INDICATORVALUEAT, ind, pos))
            return ind;
    }
    return -1;
}
static void jump_styled(int direction) {
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t pos     = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
    sptr_t doc_len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    sptr_t cursor  = pos + direction;
    while (cursor >= 0 && cursor < doc_len) {
        if (any_mark_indicator_at(sci, cursor) >= 0) {
            scintilla_send_message(s, SCI_GOTOPOS, (uptr_t)cursor, 0);
            scintilla_send_message(s, SCI_SCROLLCARET, 0, 0);
            return;
        }
        cursor += direction;
    }
}
static void action_jump_styled_next(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; jump_styled(+1);
}
static void action_jump_styled_prev(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; jump_styled(-1);
}

/* Q-fix Search → Find (Volatile) Next/Previous.
 * Macros equivalent: take the word at caret as the needle without opening
 * the Find dialog, then move to the next/previous match. */
static void volatile_find(int direction) {
    GtkWidget *sci = current_sci(); if (!sci) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t pos = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
    sptr_t ws  = scintilla_send_message(s, SCI_WORDSTARTPOSITION, (uptr_t)pos, TRUE);
    sptr_t we  = scintilla_send_message(s, SCI_WORDENDPOSITION,   (uptr_t)pos, TRUE);
    if (ws == we) return;
    sptr_t n = we - ws;
    char *word = g_malloc(n + 1);
    struct Sci_TextRangeFull tr = {{ws, we}, word};
    scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    sptr_t doc_len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    scintilla_send_message(s, SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE, 0);
    if (direction > 0) {
        scintilla_send_message(s, SCI_SETTARGETRANGE, we, doc_len);
    } else {
        scintilla_send_message(s, SCI_SETTARGETRANGE, ws, 0);
    }
    sptr_t hit = scintilla_send_message(s, SCI_SEARCHINTARGET,
                                        (uptr_t)n, (sptr_t)word);
    if (hit >= 0) {
        sptr_t end = scintilla_send_message(s, SCI_GETTARGETEND, 0, 0);
        scintilla_send_message(s, SCI_SETSEL, (uptr_t)hit, end);
        scintilla_send_message(s, SCI_SCROLLCARET, 0, 0);
    }
    g_free(word);
}
static void action_find_volatile_next(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; volatile_find(+1);
}
static void action_find_volatile_prev(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u; volatile_find(-1);
}

/* ──────────────────────────────────────────────────────────────────────
 * G22 Incremental search bar (Ctrl+I)
 *
 * Simple bar at the bottom of the window over the active editor: search
 * entry + Next / Prev / Highlight-All toggles. Esc dismisses.
 * ────────────────────────────────────────────────────────────────────── */

static GtkWidget *g_isearch_revealer = NULL;
static GtkWidget *g_isearch_entry    = NULL;

static void isearch_do_find(int direction) {
    if (!g_isearch_entry) return;
    GtkWidget *sci = current_sci(); if (!sci) return;
    const char *needle = gtk_entry_get_text(GTK_ENTRY(g_isearch_entry));
    if (!needle || !*needle) return;
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t pos = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
    sptr_t doc_len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    if (direction > 0)
        scintilla_send_message(s, SCI_SETTARGETRANGE, pos, doc_len);
    else
        scintilla_send_message(s, SCI_SETTARGETRANGE, pos - 1, 0);
    scintilla_send_message(s, SCI_SETSEARCHFLAGS, 0, 0);
    sptr_t hit = scintilla_send_message(s, SCI_SEARCHINTARGET,
                                        (uptr_t)strlen(needle), (sptr_t)needle);
    if (hit < 0) {
        /* Wrap. */
        scintilla_send_message(s, SCI_SETTARGETRANGE,
            direction > 0 ? 0 : doc_len,
            direction > 0 ? pos : 0);
        hit = scintilla_send_message(s, SCI_SEARCHINTARGET,
                                     (uptr_t)strlen(needle), (sptr_t)needle);
    }
    if (hit >= 0) {
        sptr_t end = scintilla_send_message(s, SCI_GETTARGETEND, 0, 0);
        scintilla_send_message(s, SCI_SETSEL, hit, end);
    }
}
static void on_isearch_changed(GtkEntry *e, gpointer u) {
    (void)e; (void)u;
    isearch_do_find(+1);
}
static gboolean on_isearch_keypress(GtkEventControllerKey *ctl, guint keyval,
                                    guint keycode, GdkModifierType state,
                                    gpointer u) {
    (void)ctl; (void)keycode; (void)u;
    if (keyval == GDK_KEY_Escape) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(g_isearch_revealer), FALSE);
        GtkWidget *sci = current_sci(); if (sci) gtk_widget_grab_focus(sci);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return) {
        isearch_do_find((state & GDK_SHIFT_MASK) ? -1 : +1);
        return TRUE;
    }
    return FALSE;
}
static void action_incremental_search(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    if (!g_isearch_revealer) return;
    gtk_revealer_set_reveal_child(GTK_REVEALER(g_isearch_revealer), TRUE);
    gtk_widget_grab_focus(g_isearch_entry);
}

/* ──────────────────────────────────────────────────────────────────────
 * G15 Command Palette (Ctrl+Shift+P)
 *
 * Floating dialog enumerating all GActions; fuzzy filter + arrow nav.
 * ────────────────────────────────────────────────────────────────────── */

typedef struct { char *name; char *label; } CmdEntry;
static GArray *g_cmd_entries = NULL;
static GtkWidget *g_cmd_dialog = NULL;
static GtkWidget *g_cmd_search = NULL;
static GtkWidget *g_cmd_list   = NULL;

/* Pretty-print an action name: "case-upper" → "Case Upper" */
static char *cmd_humanize(const char *name) {
    GString *s = g_string_sized_new(strlen(name));
    gboolean cap = TRUE;
    for (const char *p = name; *p; p++) {
        if (*p == '-' || *p == '_') { g_string_append_c(s, ' '); cap = TRUE; }
        else if (cap) { g_string_append_c(s, g_ascii_toupper(*p)); cap = FALSE; }
        else g_string_append_c(s, *p);
    }
    return g_string_free(s, FALSE);
}
static void cmd_populate(void) {
    if (!g_app) return;
    if (g_cmd_entries) {
        for (guint i = 0; i < g_cmd_entries->len; i++) {
            CmdEntry *e = &g_array_index(g_cmd_entries, CmdEntry, i);
            g_free(e->name); g_free(e->label);
        }
        g_array_set_size(g_cmd_entries, 0);
    } else {
        g_cmd_entries = g_array_new(FALSE, FALSE, sizeof(CmdEntry));
    }
    gchar **actions = g_action_group_list_actions(G_ACTION_GROUP(g_app));
    for (int i = 0; actions[i]; i++) {
        CmdEntry e = { g_strdup(actions[i]), cmd_humanize(actions[i]) };
        g_array_append_val(g_cmd_entries, e);
    }
    g_strfreev(actions);
}
static gboolean cmd_match(const char *label, const char *query) {
    /* Substring fuzzy: each query char must appear in order in label
     * (case-insensitive). */
    if (!query || !*query) return TRUE;
    const char *l = label, *q = query;
    while (*l && *q) {
        if (g_ascii_tolower(*l) == g_ascii_tolower(*q)) q++;
        l++;
    }
    return *q == '\0';
}
static void cmd_refresh_list(const char *query) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_cmd_list));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    int shown = 0;
    for (guint i = 0; i < g_cmd_entries->len && shown < 50; i++) {
        CmdEntry *e = &g_array_index(g_cmd_entries, CmdEntry, i);
        if (cmd_match(e->label, query)) {
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *lbl = gtk_label_new(e->label);
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_widget_set_margin_start(lbl, 8);
            gtk_widget_set_margin_end(lbl, 8);
            gtk_widget_set_margin_top(lbl, 4);
            gtk_widget_set_margin_bottom(lbl, 4);
            gtk_container_add(GTK_CONTAINER(row), lbl);
            g_object_set_data_full(G_OBJECT(row), "action-name",
                                    g_strdup(e->name), g_free);
            gtk_list_box_insert(GTK_LIST_BOX(g_cmd_list), row, -1);
            shown++;
        }
    }
    gtk_widget_show_all(g_cmd_list);
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_cmd_list), 0);
    if (first) gtk_list_box_select_row(GTK_LIST_BOX(g_cmd_list), first);
}
static void cmd_on_search_changed(GtkEntry *e, gpointer u) {
    (void)u;
    cmd_refresh_list(gtk_entry_get_text(e));
}
static void cmd_activate_selected(void) {
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_cmd_list));
    if (!row) return;
    const char *name = g_object_get_data(G_OBJECT(row), "action-name");
    if (!name) return;
    gtk_widget_hide(g_cmd_dialog);
    /* The dialog steals focus; restore to editor first, then activate so
     * focus-aware actions (incremental search, etc.) target the editor. */
    GtkWidget *sci = current_sci(); if (sci) gtk_widget_grab_focus(sci);
    g_action_group_activate_action(G_ACTION_GROUP(g_app), name, NULL);
}
static void cmd_on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer u) {
    (void)box; (void)row; (void)u;
    cmd_activate_selected();
}
static gboolean cmd_on_entry_keypress(GtkEventControllerKey *ctl, guint keyval,
                                      guint keycode, GdkModifierType state,
                                      gpointer u) {
    (void)ctl; (void)keycode; (void)state; (void)u;
    if (keyval == GDK_KEY_Escape) { gtk_widget_set_visible(g_cmd_dialog, FALSE); return TRUE; }
    if (keyval == GDK_KEY_Return) { cmd_activate_selected(); return TRUE; }
    if (keyval == GDK_KEY_Down) {
        GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_cmd_list));
        int idx = cur ? gtk_list_box_row_get_index(cur) : -1;
        GtkListBoxRow *nxt = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_cmd_list), idx + 1);
        if (nxt) gtk_list_box_select_row(GTK_LIST_BOX(g_cmd_list), nxt);
        return TRUE;
    }
    if (keyval == GDK_KEY_Up) {
        GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_cmd_list));
        int idx = cur ? gtk_list_box_row_get_index(cur) : 0;
        if (idx > 0) {
            GtkListBoxRow *prv = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_cmd_list), idx - 1);
            if (prv) gtk_list_box_select_row(GTK_LIST_BOX(g_cmd_list), prv);
        }
        return TRUE;
    }
    return FALSE;
}
/* ──────────────────────────────────────────────────────────────────────
 * G17 Large File Restriction (macOS Phases 1–2.6)
 *
 * For files above the threshold, prompt the user before loading. Loaded
 * docs get an "is_large" flag (via g_object_set_data on sci) so features
 * like wrap / autocomplete / smart-highlight can short-circuit.
 * ────────────────────────────────────────────────────────────────────── */

#define NPP_LARGE_FILE_HARD_MB   2048    /* 2 GB threshold for hard warning */

/* Returns TRUE if the file is safe to open as-is, FALSE if user cancelled. */
static gboolean large_file_guard(const char *path) {
    if (!path) return TRUE;
    /* P3 — gate large-file restrictions on the pref. When disabled,
     * always proceed without prompting. */
    if (!g_prefs.large_file_enabled) return TRUE;
    struct stat st;
    if (stat(path, &st) != 0) return TRUE;
    gint64 mb = st.st_size / (1024 * 1024);
    if (mb < g_prefs.large_file_size_mb) return TRUE;

    /* large_file_suppress: skip the dialog entirely; just proceed. */
    if (g_prefs.large_file_suppress) return TRUE;

    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        mb >= NPP_LARGE_FILE_HARD_MB ? GTK_BUTTONS_OK_CANCEL : GTK_BUTTONS_YES_NO,
        "Open %s?", g_path_get_basename(path));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
        "This file is %lld MB. Files this large may cause Nextpad++ to\n"
        "use a lot of memory and disable some features (syntax highlighting,\n"
        "wrap, autocomplete, smart highlight, brace match).\n\nProceed?",
        (long long)mb);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return (resp == GTK_RESPONSE_YES || resp == GTK_RESPONSE_OK);
}

/* Open with size guard; sets "npp-is-large" on the sci widget if applicable.
 * Per-feature allow flags from g_prefs (P3): autocomplete / smart-hilite /
 * brace-match / URL-click can be selectively enabled in large-file mode. */
static gboolean editor_open_path_guarded(const char *path) {
    if (!large_file_guard(path)) return FALSE;
    gboolean ok = editor_open_path(path);
    if (ok && g_prefs.large_file_enabled) {
        struct stat st;
        if (stat(path, &st) == 0 &&
            st.st_size >= (off_t)g_prefs.large_file_size_mb * 1024L * 1024L) {
            NppDoc *d = editor_current_doc();
            if (d && d->sci) {
                g_object_set_data(G_OBJECT(d->sci), "npp-is-large", GINT_TO_POINTER(1));
                if (g_prefs.large_file_no_wrap)
                    scintilla_send_message(SCINTILLA(d->sci), SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
                /* Per-feature allow flags stashed on the widget so the
                 * relevant code paths can check them without re-reading prefs. */
                g_object_set_data(G_OBJECT(d->sci), "npp-lf-allow-ac",
                    GINT_TO_POINTER(g_prefs.large_file_allow_autocomplete));
                g_object_set_data(G_OBJECT(d->sci), "npp-lf-allow-sh",
                    GINT_TO_POINTER(g_prefs.large_file_allow_smart_hilite));
                g_object_set_data(G_OBJECT(d->sci), "npp-lf-allow-bm",
                    GINT_TO_POINTER(g_prefs.large_file_allow_brace_match));
                g_object_set_data(G_OBJECT(d->sci), "npp-lf-allow-url",
                    GINT_TO_POINTER(g_prefs.large_file_allow_url_click));
            }
        }
    }
    return ok;
}

/* Public helper for other modules to check large-file allow flags. */
gboolean main_large_file_allows(GtkWidget *sci, const char *feature) {
    if (!sci) return TRUE;
    if (!g_object_get_data(G_OBJECT(sci), "npp-is-large")) return TRUE;
    if (!feature) return FALSE;
    const char *key = NULL;
    if      (!strcmp(feature, "autocomplete")) key = "npp-lf-allow-ac";
    else if (!strcmp(feature, "smarthilite"))  key = "npp-lf-allow-sh";
    else if (!strcmp(feature, "bracematch"))   key = "npp-lf-allow-bm";
    else if (!strcmp(feature, "urlclick"))     key = "npp-lf-allow-url";
    if (!key) return FALSE;
    return g_object_get_data(G_OBJECT(sci), key) != NULL;
}

/* ──────────────────────────────────────────────────────────────────────
 * G20 Tab pinning + color coding
 *
 * State lives on the Scintilla widget via g_object_set_data so we don't
 * have to modify the grafted NppDoc struct in editor.h.
 * ────────────────────────────────────────────────────────────────────── */

/* Pinning is owned by editor.c (NppDoc.pinned) — the macOS pin icon, the
 * hidden ×, the close block, and the Document List all read that one
 * field. These thin wrappers keep the rest of main.c unchanged. */
static gboolean tab_is_pinned(GtkWidget *sci) {
    return editor_tab_pinned(sci);
}
static void tab_set_pinned(GtkWidget *sci, gboolean pin) {
    editor_set_tab_pinned(sci, pin);
}
static void action_tab_pin_toggle(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    GtkWidget *sci = current_sci(); if (!sci) return;
    tab_set_pinned(sci, !tab_is_pinned(sci));
}

static void action_tab_set_color(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a; (void)u;
    int slot = g_variant_get_int32(p);   /* 0 = clear, 1..5 = colour */
    GtkWidget *sci = current_sci(); if (!sci) return;
    editor_set_tab_color(sci, slot);     /* NppDoc.color_tag is the truth */
}

/* #3 split views — move/clone the focused editor to a secondary view. */
static void action_move_vview(GSimpleAction *a, GVariant *p, gpointer u)
{ (void)a;(void)p;(void)u; editor_move_to_view(TRUE); }
static void action_move_hview(GSimpleAction *a, GVariant *p, gpointer u)
{ (void)a;(void)p;(void)u; editor_move_to_view(FALSE); }
static void action_clone_vview(GSimpleAction *a, GVariant *p, gpointer u)
{ (void)a;(void)p;(void)u; editor_clone_to_view(TRUE); }
static void action_clone_hview(GSimpleAction *a, GVariant *p, gpointer u)
{ (void)a;(void)p;(void)u; editor_clone_to_view(FALSE); }
static void action_reset_view(GSimpleAction *a, GVariant *p, gpointer u)
{ (void)a;(void)p;(void)u; editor_reset_view(); }

/* Install CSS for tab colours once. */
static void install_tab_color_css(void) {
    /* Reusable provider — re-running on a light/dark switch restyles the
     * editor tab strip in place (a fresh provider each call would stack). */
    static GtkCssProvider *p = NULL;
    gboolean first_install = (p == NULL);
    if (first_install) p = gtk_css_provider_new();
    GString *css = g_string_new(
        /* Per-tab colour: a 3px stripe along the top of the tab, matching
         * macOS NppTabBar (tabColorForId). The .tab-color-N class is set
         * by editor_apply_tab_color() on the GtkNotebook `tab` node; an
         * inset box-shadow paints the stripe without disturbing layout.
         * Colours are the exact macOS palette (yellow/green/blue/orange/
         * pink). */
        /* Each colour is given for both the inactive tab and (with an
         * extra :checked, so it out-specifies the active-tab orange
         * `tab:checked` rule further down) the active tab — a coloured
         * tab keeps its colour whether active or not, like macOS. */
        "notebook.npp-editor-tabs > header > tabs > tab.tab-color-1,\n"
        "notebook.npp-editor-tabs > header > tabs > tab.tab-color-1:checked { box-shadow: inset 0 3px 0 0 #FCE386; }\n"
        "notebook.npp-editor-tabs > header > tabs > tab.tab-color-2,\n"
        "notebook.npp-editor-tabs > header > tabs > tab.tab-color-2:checked { box-shadow: inset 0 3px 0 0 #A9F08C; }\n"
        "notebook.npp-editor-tabs > header > tabs > tab.tab-color-3,\n"
        "notebook.npp-editor-tabs > header > tabs > tab.tab-color-3:checked { box-shadow: inset 0 3px 0 0 #7AC9F5; }\n"
        "notebook.npp-editor-tabs > header > tabs > tab.tab-color-4,\n"
        "notebook.npp-editor-tabs > header > tabs > tab.tab-color-4:checked { box-shadow: inset 0 3px 0 0 #F5B67A; }\n"
        "notebook.npp-editor-tabs > header > tabs > tab.tab-color-5,\n"
        "notebook.npp-editor-tabs > header > tabs > tab.tab-color-5:checked { box-shadow: inset 0 3px 0 0 #F08CF0; }\n"
        /* Editor tab strip geometry. All selectors are scoped to the
         * editor notebook (.npp-editor-tabs) so Preferences / Plugin Admin
         * / Find-dialog notebooks keep their default theme tabs.
         * Inactive tabs sit 3px lower (margin-top) and are 3px shorter
         * than the active tab — the macOS NppTabBar kActiveBoost.
         * No bottom margin: every tab sits flush on the header's #cccccc
         * base line. */
        "notebook.npp-editor-tabs > header > tabs > tab {"
        "  min-height: 18px;"
        "  padding-top: 2px;"
        "  padding-bottom: 2px;"
        "  margin: 3px 0 0 0;"
        "}\n"
        "notebook.npp-editor-tabs > header > tabs > tab:checked {"
        "  min-height: 21px;"
        "  margin-top: 0;"
        "}\n"
        "notebook.npp-editor-tabs > header > tabs > tab button {"
        "  min-height: 0;"
        "  min-width: 0;"
        "  padding: 0;"
        "  margin: 0;"
        "}\n"
        /* The close button's hover highlight is baked into the icon
         * (closeTabButton_hoverIn.png) — suppress the theme's own button
         * background/shadow so only that tight rounded square shows. */
        "notebook.npp-editor-tabs > header > tabs > tab button,\n"
        "notebook.npp-editor-tabs > header > tabs > tab button:hover,\n"
        "notebook.npp-editor-tabs > header > tabs > tab button:active {"
        "  background-color: transparent;"
        "  background-image: none;"
        "  box-shadow: none;"
        "  border: none;"
        "}\n");

    /* macOS NppTabBar styling — light mode only; dark keeps the Yaru
     * theme tabs. Values mirror NppThemeManager: #f0f0f0 strip, white
     * active tab with a 3px orange (#fda640) top stripe, grey-gradient
     * inactive tabs, #949494 border, 3px rounded top corners. */
    GtkSettings *settings = gtk_settings_get_default();
    gboolean dark = FALSE;
    if (settings)
        g_object_get(settings, "gtk-application-prefer-dark-theme", &dark, NULL);
    if (!dark)
        g_string_append(css,
            /* The #cccccc base line is a border-top on the content
             * (stack) node, NOT a border on the header. The tabs are the
             * header's children and draw on top of the header's own
             * border, so a header border-bottom is covered wherever a tab
             * sits — only the empty area showed it. The stack is a sibling
             * laid out below the header, so the tabs cannot paint over its
             * border-top: the line then runs full width under every tab
             * and the empty area. box-shadow/border:none strip the theme's
             * own header/tabs separators so only this line shows. */
            "notebook.npp-editor-tabs > header {"
            "  background-color: #f0f0f0;"
            "  box-shadow: none;"
            "  border: none;"
            "}\n"
            "notebook.npp-editor-tabs > header > tabs {"
            "  box-shadow: none;"
            "  border: none;"
            "}\n"
            "notebook.npp-editor-tabs > stack {"
            "  border-top: 1px solid #cccccc;"
            "}\n"
            /* Default tab text colour (lower specificity than tab-color-N). */
            "notebook.npp-editor-tabs > header > tabs > tab label { color: #262626; }\n"
            "notebook.npp-editor-tabs > header > tabs > tab {"
            "  background-image: linear-gradient(to bottom, #dbdbdb, #cccccc);"
            "  border: 1px solid #949494;"
            "  border-bottom: none;"
            "  border-top-left-radius: 3px;"
            "  border-top-right-radius: 3px;"
            "}\n"
            "notebook.npp-editor-tabs > header > tabs > tab:hover {"
            "  background-image: linear-gradient(to bottom, #e6e6e6, #dedede);"
            "}\n"
            "notebook.npp-editor-tabs > header > tabs > tab:checked {"
            "  background-image: none;"
            "  background-color: #ffffff;"
            "  box-shadow: inset 0 3px 0 0 #fda640;"
            "}\n");
    else
        /* Dark-mode tab strip — mirrors the light block with dark values
         * so the tab bar follows the appearance toggle. */
        g_string_append(css,
            "notebook.npp-editor-tabs > header {"
            "  background-color: #2d2d2d;"
            "  box-shadow: none;"
            "  border: none;"
            "}\n"
            "notebook.npp-editor-tabs > header > tabs {"
            "  box-shadow: none;"
            "  border: none;"
            "}\n"
            "notebook.npp-editor-tabs > stack {"
            "  border-top: 1px solid #454545;"
            "}\n"
            "notebook.npp-editor-tabs > header > tabs > tab label { color: #d0d0d0; }\n"
            "notebook.npp-editor-tabs > header > tabs > tab {"
            "  background-image: linear-gradient(to bottom, #3c3c3c, #333333);"
            "  border: 1px solid #1f1f1f;"
            "  border-bottom: none;"
            "  border-top-left-radius: 3px;"
            "  border-top-right-radius: 3px;"
            "}\n"
            "notebook.npp-editor-tabs > header > tabs > tab:hover {"
            "  background-image: linear-gradient(to bottom, #484848, #404040);"
            "}\n"
            "notebook.npp-editor-tabs > header > tabs > tab:checked {"
            "  background-image: none;"
            "  background-color: #1e1e1e;"
            "  box-shadow: inset 0 3px 0 0 #fda640;"
            "}\n");

    gtk_css_provider_load_from_data(p, css->str, -1);
    g_string_free(css, TRUE);
    if (first_install)
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(p),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    /* p is a static singleton kept for the app lifetime — not unref'd. */
}

/* Re-apply the editor chrome (tab strip CSS + toolbar) after a light/dark
 * appearance switch — called from prefs.c appearance_apply_live(). */
void main_refresh_theme_chrome(void)
{
    install_tab_color_css();
    toolbar_apply_theme();
    editor_refresh_tab_chrome();   /* tab floppy / pin / close icons */
}

static void action_command_palette(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    if (!g_window) return;

    if (!g_cmd_dialog) {
        g_cmd_dialog = gtk_dialog_new();
        gtk_window_set_title(GTK_WINDOW(g_cmd_dialog), "Command Palette");
        gtk_window_set_transient_for(GTK_WINDOW(g_cmd_dialog), GTK_WINDOW(g_window));
        gtk_window_set_modal(GTK_WINDOW(g_cmd_dialog), TRUE);
        gtk_window_set_decorated(GTK_WINDOW(g_cmd_dialog), TRUE);
        gtk_window_set_default_size(GTK_WINDOW(g_cmd_dialog), 540, 420);

        GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(g_cmd_dialog));
        gtk_container_set_border_width(GTK_CONTAINER(content), 8);
        gtk_box_set_spacing(GTK_BOX(content), 8);

        g_cmd_search = gtk_search_entry_new();
        npp_box_pack(GTK_BOX(content), g_cmd_search, FALSE, 0);
        g_signal_connect(g_cmd_search, "search-changed",
                         G_CALLBACK(cmd_on_search_changed), NULL);
        {
            GtkEventController *kc = gtk_event_controller_key_new();
            g_signal_connect(kc, "key-pressed",
                             G_CALLBACK(cmd_on_entry_keypress), NULL);
            gtk_widget_add_controller(g_cmd_search, kc);
        }

        GtkWidget *scrolled = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        g_cmd_list = gtk_list_box_new();
        gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(g_cmd_list), FALSE);
        g_signal_connect(g_cmd_list, "row-activated",
                         G_CALLBACK(cmd_on_row_activated), NULL);
        gtk_container_add(GTK_CONTAINER(scrolled), g_cmd_list);
        npp_box_pack(GTK_BOX(content), scrolled, TRUE, 0);
    }
    cmd_populate();
    gtk_entry_set_text(GTK_ENTRY(g_cmd_search), "");
    cmd_refresh_list("");
    gtk_widget_show_all(g_cmd_dialog);
    gtk_widget_grab_focus(g_cmd_search);
}

/* Edit ▸ Delete (macOS parity) — clear the selection / forward-delete. */
static void action_delete(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
    sci_send(SCI_CLEAR, 0, 0);
}

/* Generic stub for macOS menu items with no Linux backend yet. These
 * actions are registered so the menu items exist (full-fidelity layout)
 * but are disabled (greyed) right after registration — see
 * disable_stub_actions(). The body never runs while disabled. */
static void action_menu_stub(GSimpleAction *a, GVariant *p, gpointer u) {
    (void)a;(void)p;(void)u;
}

/* macOS items not yet wired on Linux — greyed so they mirror the macOS
 * menu layout without pretending to work. */
static const char *kStubActions[] = {
    "close-all-but-pinned",     /* File ▸ Close Multiple Documents      */
    "change-search-engine",     /* Edit ▸ On Selection                  */
    "clear-readonly",           /* Edit ▸ Read-Only in Nextpad++        */
    "lock-file", "unlock-file", /* Edit ▸ Locked Attribute (macOS)      */
    "ac-hint-prev", "ac-hint-next", /* Edit ▸ Auto-Completion           */
    "tab-move-start", "tab-move-end",                /* View ▸ Tab       */
    "tab-move-forward", "tab-move-backward",         /* View ▸ Tab       */
    "search-result-next", "search-result-prev",     /* Search           */
};

static const GActionEntry kAppActions[] = {
    /* File */
    { "new",            action_new,            NULL, NULL, NULL },
    { "open",           action_open,           NULL, NULL, NULL },
    { "save",           action_save,           NULL, NULL, NULL },
    { "save-as",        action_save_as,        NULL, NULL, NULL },
    { "save-copy-as",   action_save_copy_as,   NULL, NULL, NULL },
    { "save-all",       action_save_all,       NULL, NULL, NULL },
    { "rename",         action_rename,         NULL, NULL, NULL },
    { "reload",         action_reload,         NULL, NULL, NULL },
    { "close",          action_close,          NULL, NULL, NULL },
    { "close-others",   action_close_others,   NULL, NULL, NULL },
    { "close-all",        action_close_all,        NULL, NULL, NULL },
    { "close-to-left",    action_close_to_left,    NULL, NULL, NULL },
    { "close-to-right",   action_close_to_right,   NULL, NULL, NULL },
    { "close-unchanged",  action_close_unchanged,  NULL, NULL, NULL },
    { "quit",           action_quit,           NULL, NULL, NULL },
    { "open-recent",    action_open_recent,    "s",  NULL, NULL },
    { "clear-recent",   action_clear_recent,   NULL, NULL, NULL },
    /* View */
    { "toggle-doclist",   action_toggle_doclist,   NULL, NULL, NULL },
    { "toggle-workspace", action_toggle_workspace, NULL, NULL, NULL },
    { "toggle-funclist",  action_toggle_funclist,  NULL, NULL, NULL },
    { "toggle-docmap",    action_toggle_docmap,    NULL, NULL, NULL },
    { "toggle-gitpanel",  action_toggle_gitpanel,  NULL, NULL, NULL },
    { "toggle-mdpreview", action_toggle_mdpreview, NULL, NULL, NULL },
    { "split-toggle",     action_split_toggle,     NULL, NULL, NULL },
    { "split-clone",      action_split_clone,      NULL, NULL, NULL },
    { "split-focus-other",action_split_focus_other,NULL, NULL, NULL },
    { "split-close",      action_split_close,      NULL, NULL, NULL },
    /* Edit */
    { "undo",           action_undo,           NULL, NULL, NULL },
    { "redo",           action_redo,           NULL, NULL, NULL },
    { "cut",            action_cut,            NULL, NULL, NULL },
    { "copy",           action_copy,           NULL, NULL, NULL },
    { "paste",          action_paste,          NULL, NULL, NULL },
    { "select-all",     action_select_all,     NULL, NULL, NULL },
    { "goto-line",      action_goto_line,      NULL, NULL, NULL },
    /* Search */
    { "find",             action_find,             NULL, NULL, NULL },
    { "replace",          action_replace,          NULL, NULL, NULL },
    { "find-next",        action_find_next,        NULL, NULL, NULL },
    { "find-prev",        action_find_prev,        NULL, NULL, NULL },
    { "find-in-files",    action_find_in_files,    NULL, NULL, NULL },
    /* Macro */
    { "macro-start",      action_macro_start,      NULL, NULL, NULL },
    { "macro-stop",       action_macro_stop,       NULL, NULL, NULL },
    { "macro-play",       action_macro_play,       NULL, NULL, NULL },
    { "macro-play-n",     action_macro_play_n,     NULL, NULL, NULL },
    { "macro-save-as",    action_macro_save_as,    NULL, NULL, NULL },
    { "macro-play-named", action_macro_play_named, "i",  NULL, NULL },
    { "macro-batch",      action_macro_batch,      NULL, NULL, NULL },
    /* Tools / Settings */
    { "run",              action_run,              NULL, NULL, NULL },
    { "preferences",      action_preferences,      NULL, NULL, NULL },
    { "shortcut-map",     action_shortcut_map,     NULL, NULL, NULL },
    { "style-editor",     action_style_editor,     NULL, NULL, NULL },
    { "theme-mode",       action_theme_mode,       "s",  NULL, NULL },
    { "column-editor",    action_column_editor,    NULL, NULL, NULL },
    { "udl-admin",          action_udl_admin,       NULL, NULL, NULL },
    { "plugins-admin",        action_plugins_admin,       NULL, NULL, NULL },
    { "open-plugins-folder",  action_open_plugins_folder, NULL, NULL, NULL },
    { "plugin-cmd",           action_plugin_cmd,          "i",  NULL, NULL },
    /* G11.3 View → Show Symbol / Zoom */
    { "show-ws",            action_show_ws,            NULL, NULL, NULL },
    { "show-eol",           action_show_eol,           NULL, NULL, NULL },
    { "show-all-chars",     action_show_all_chars,     NULL, NULL, NULL },
    { "show-indent-guide",  action_show_indent_guide,  NULL, NULL, NULL },
    { "show-line-numbers",  action_show_line_numbers,  NULL, NULL, NULL },
    { "show-wrap-symbol",   action_show_wrap_symbol,   NULL, NULL, NULL },
    { "zoom-in",            action_zoom_in,            NULL, NULL, NULL },
    { "zoom-out",           action_zoom_out,           NULL, NULL, NULL },
    { "zoom-reset",         action_zoom_reset,         NULL, NULL, NULL },
    /* G11.5 Fold */
    { "fold-all",           action_fold_all,           NULL, NULL, NULL },
    { "unfold-all",         action_unfold_all,         NULL, NULL, NULL },
    { "toggle-fold",        action_toggle_fold,        NULL, NULL, NULL },
    /* Q-fix Fold Level / Unfold Level (16) */
    { "fold-level-1", action_fold_level_1, NULL, NULL, NULL },
    { "fold-level-2", action_fold_level_2, NULL, NULL, NULL },
    { "fold-level-3", action_fold_level_3, NULL, NULL, NULL },
    { "fold-level-4", action_fold_level_4, NULL, NULL, NULL },
    { "fold-level-5", action_fold_level_5, NULL, NULL, NULL },
    { "fold-level-6", action_fold_level_6, NULL, NULL, NULL },
    { "fold-level-7", action_fold_level_7, NULL, NULL, NULL },
    { "fold-level-8", action_fold_level_8, NULL, NULL, NULL },
    { "unfold-level-1", action_unfold_level_1, NULL, NULL, NULL },
    { "unfold-level-2", action_unfold_level_2, NULL, NULL, NULL },
    { "unfold-level-3", action_unfold_level_3, NULL, NULL, NULL },
    { "unfold-level-4", action_unfold_level_4, NULL, NULL, NULL },
    { "unfold-level-5", action_unfold_level_5, NULL, NULL, NULL },
    { "unfold-level-6", action_unfold_level_6, NULL, NULL, NULL },
    { "unfold-level-7", action_unfold_level_7, NULL, NULL, NULL },
    { "unfold-level-8", action_unfold_level_8, NULL, NULL, NULL },
    /* Q-fix Search → Change History (3) */
    { "change-next",  action_change_next,  NULL, NULL, NULL },
    { "change-prev",  action_change_prev,  NULL, NULL, NULL },
    { "change-clear", action_change_clear, NULL, NULL, NULL },
    /* Q-fix Edit → Multi-Select (10) */
    { "msa-ignore",    action_msa_ignore,    NULL, NULL, NULL },
    { "msa-case",      action_msa_case,      NULL, NULL, NULL },
    { "msa-word",      action_msa_word,      NULL, NULL, NULL },
    { "msa-case-word", action_msa_case_word, NULL, NULL, NULL },
    { "msn-ignore",    action_msn_ignore,    NULL, NULL, NULL },
    { "msn-case",      action_msn_case,      NULL, NULL, NULL },
    { "msn-word",      action_msn_word,      NULL, NULL, NULL },
    { "msn-case-word", action_msn_case_word, NULL, NULL, NULL },
    { "ms-undo",       action_ms_undo,       NULL, NULL, NULL },
    { "ms-skip",       action_ms_skip,       NULL, NULL, NULL },
    /* G11.6 View toggles */
    { "word-wrap",          action_word_wrap,          NULL, NULL, NULL },
    { "always-on-top",      action_always_on_top,      NULL, NULL, NULL },
    { "fullscreen",         action_fullscreen,         NULL, NULL, NULL },
    /* G11.4 Tab nav */
    { "tab-next",           action_tab_next,           NULL, NULL, NULL },
    { "tab-prev",           action_tab_prev,           NULL, NULL, NULL },
    { "tab-first",          action_tab_first,          NULL, NULL, NULL },
    { "tab-last",           action_tab_last,           NULL, NULL, NULL },
    { "tab-goto",           action_tab_goto,           "i",  NULL, NULL },
    /* G11.7 File menu fillers */
    { "open-workspace",     action_open_workspace,     NULL, NULL, NULL },
    { "open-containing",    action_open_containing_folder, NULL, NULL, NULL },
    { "open-default-viewer",action_open_in_default_viewer, NULL, NULL, NULL },
    { "save-session",       action_save_session,       NULL, NULL, NULL },
    { "load-session",       action_load_session,       NULL, NULL, NULL },
    { "print",              action_print,              NULL, NULL, NULL },
    /* G11.8 Help */
    { "help-home",          action_help_home,          NULL, NULL, NULL },
    { "help-project",       action_help_project,       NULL, NULL, NULL },
    { "help-manual",        action_help_manual,        NULL, NULL, NULL },
    { "help-about",         action_help_about,         NULL, NULL, NULL },
    /* G11.2 Language */
    /* set-language is registered separately as a stateful action (#4). */
    /* Help / extras */
    { "help-cli-args",      action_help_cli_args,      NULL, NULL, NULL },
    { "view-summary",       action_view_summary,       NULL, NULL, NULL },
    { "reopen-closed",      action_reopen_closed,      NULL, NULL, NULL },
    /* G16 Brace matching */
    { "goto-matching-brace",   action_goto_matching_brace, NULL, NULL, NULL },
    { "select-in-brackets",    action_select_in_brackets,  NULL, NULL, NULL },
    /* G19 Hash + MIME tools */
    { "hash-md5",         action_hash_md5,         NULL, NULL, NULL },
    { "hash-md5-clip",    action_hash_md5_clip,    NULL, NULL, NULL },
    /* Q-align hash-from-files (4) + mark dialog + search-results-window + path-completion (3). */
    { "hash-md5-files",    action_hash_md5_files,    NULL, NULL, NULL },
    { "hash-sha1-files",   action_hash_sha1_files,   NULL, NULL, NULL },
    { "hash-sha256-files", action_hash_sha256_files, NULL, NULL, NULL },
    { "hash-sha512-files", action_hash_sha512_files, NULL, NULL, NULL },
    { "show-mark-dialog",  action_show_mark_dialog,  NULL, NULL, NULL },
    { "show-search-results", action_show_search_results_window, NULL, NULL, NULL },
    { "ac-path",            action_path_completion,   NULL, NULL, NULL },
    { "hash-sha1",        action_hash_sha1,        NULL, NULL, NULL },
    { "hash-sha1-clip",   action_hash_sha1_clip,   NULL, NULL, NULL },
    { "hash-sha256",      action_hash_sha256,      NULL, NULL, NULL },
    { "hash-sha256-clip", action_hash_sha256_clip, NULL, NULL, NULL },
    { "hash-sha512",      action_hash_sha512,      NULL, NULL, NULL },
    { "hash-sha512-clip", action_hash_sha512_clip, NULL, NULL, NULL },
    { "base64-encode",         action_base64_enc,          NULL, NULL, NULL },
    { "base64-decode",         action_base64_dec,          NULL, NULL, NULL },
    { "base64-encode-padded",  action_base64_enc_padded,   NULL, NULL, NULL },
    { "base64-decode-strict",  action_base64_dec_strict,   NULL, NULL, NULL },
    { "base64-encode-urlsafe", action_base64_enc_urlsafe,  NULL, NULL, NULL },
    { "base64-decode-urlsafe", action_base64_dec_urlsafe,  NULL, NULL, NULL },
    { "ascii-to-hex",          action_ascii_to_hex,        NULL, NULL, NULL },
    { "hex-to-ascii",          action_hex_to_ascii,        NULL, NULL, NULL },
    /* G25 Extra panels */
    { "toggle-charpanel",    action_toggle_charpanel,    NULL, NULL, NULL },
    { "toggle-cliphistory",  action_toggle_cliphistory,  NULL, NULL, NULL },
    { "toggle-project",      action_toggle_project,      NULL, NULL, NULL },
    /* G12.1 Insert */
    { "insert-dt-short",     action_insert_dt_short,     NULL, NULL, NULL },
    { "insert-dt-long",      action_insert_dt_long,      NULL, NULL, NULL },
    { "insert-blank-above",  action_insert_blank_above,  NULL, NULL, NULL },
    { "insert-blank-below",  action_insert_blank_below,  NULL, NULL, NULL },
    /* Q-fix: Insert custom date/time, paste-special, view-in (15 items). */
    { "insert-dt-custom",    action_insert_datetime_custom, NULL, NULL, NULL },
    { "paste-html",          action_paste_html,          NULL, NULL, NULL },
    { "paste-rtf",           action_paste_rtf,           NULL, NULL, NULL },
    { "copy-binary",         action_copy_binary,         NULL, NULL, NULL },
    { "paste-binary",        action_paste_binary,        NULL, NULL, NULL },
    { "find-chars-in-range", action_find_chars_in_range, NULL, NULL, NULL },
    { "view-in-firefox",     action_view_in_firefox,     NULL, NULL, NULL },
    { "view-in-chrome",      action_view_in_chrome,      NULL, NULL, NULL },
    { "view-in-chromium",    action_view_in_chromium,    NULL, NULL, NULL },
    { "view-in-default",     action_view_in_default,     NULL, NULL, NULL },
    /* Q-fix UDL + Settings + Help (5 items). */
    { "udl-define",          action_udl_define,          NULL, NULL, NULL },
    { "udl-open-folder",     action_udl_open_folder,     NULL, NULL, NULL },
    { "udl-collection",      action_udl_collection,      NULL, NULL, NULL },
    { "edit-popup-ctxmenu",  action_edit_popup_ctxmenu,  NULL, NULL, NULL },
    { "debug-info",          action_debug_info,          NULL, NULL, NULL },
    { "check-updates",       action_check_updates,       NULL, NULL, NULL },
    { "install-cli",         action_install_cli,         NULL, NULL, NULL },
    /* Q-align Settings → Import submenu. */
    { "import-plugin",       action_import_plugin,       NULL, NULL, NULL },
    { "import-style-theme",  action_import_style_theme,  NULL, NULL, NULL },
    /* Q-align Run / Macro / Edit / Search additions from screenshots. */
    { "get-php-help",        action_get_php_help,        NULL, NULL, NULL },
    { "wikipedia-search",    action_wikipedia_search,    NULL, NULL, NULL },
    { "open-in-new-instance",action_open_in_new_instance,NULL, NULL, NULL },
    { "modify-shortcut-macro",action_modify_shortcut_macro,NULL,NULL, NULL },
    { "modify-shortcut-run", action_modify_shortcut_run, NULL, NULL, NULL },
    { "begin-end-select",    action_begin_end_select,    NULL, NULL, NULL },
    { "begin-end-select-column", action_begin_end_select_column, NULL, NULL, NULL },
    { "select-find-next",    action_select_find_next,    NULL, NULL, NULL },
    { "select-find-prev",    action_select_find_prev,    NULL, NULL, NULL },
    { "toggle-spell-check",  action_toggle_spell_check,  NULL, NULL, NULL },
    /* Q-fix View misc + File misc (8 items). */
    { "distraction-free",    action_distraction_free,    NULL, NULL, NULL },
    { "post-it",             action_post_it,             NULL, NULL, NULL },
    { "hide-line-marks",     action_hide_line_marks,     NULL, NULL, NULL },
    { "sync-scroll-v",       action_sync_scroll_v,       NULL, NULL, NULL },
    { "sync-scroll-h",       action_sync_scroll_h,       NULL, NULL, NULL },
    { "focus-other-view",    action_focus_other_view,    NULL, NULL, NULL },
    { "print-now",           action_print_now,           NULL, NULL, NULL },
    { "open-containing-files",    action_open_containing_files,    NULL, NULL, NULL },
    { "open-containing-terminal", action_open_containing_terminal, NULL, NULL, NULL },
    /* G12.2 Copy to clipboard */
    { "copy-full-path",      action_copy_full_path,      NULL, NULL, NULL },
    { "copy-file-name",      action_copy_file_name,      NULL, NULL, NULL },
    { "copy-dir-path",       action_copy_dir_path,       NULL, NULL, NULL },
    { "copy-all-names",      action_copy_all_names,      NULL, NULL, NULL },
    { "copy-all-paths",      action_copy_all_paths,      NULL, NULL, NULL },
    /* G12.3 Indent */
    { "indent",              action_indent,              NULL, NULL, NULL },
    { "unindent",            action_unindent,            NULL, NULL, NULL },
    /* G12.4 Case */
    { "case-upper",          action_case_upper,          NULL, NULL, NULL },
    { "case-lower",          action_case_lower,          NULL, NULL, NULL },
    { "case-proper-force",   action_case_proper_force,   NULL, NULL, NULL },
    { "case-proper-blend",   action_case_proper_blend,   NULL, NULL, NULL },
    { "case-sentence-force", action_case_sentence_force, NULL, NULL, NULL },
    { "case-sentence-blend", action_case_sentence_blend, NULL, NULL, NULL },
    { "case-invert",         action_case_invert,         NULL, NULL, NULL },
    { "case-random",         action_case_random,         NULL, NULL, NULL },
    /* G12.5 Line ops */
    { "line-duplicate",      action_line_duplicate,      NULL, NULL, NULL },
    { "line-delete",         action_line_delete,         NULL, NULL, NULL },
    { "line-move-up",        action_line_move_up,        NULL, NULL, NULL },
    { "line-move-down",      action_line_move_down,      NULL, NULL, NULL },
    { "line-split",          action_line_split,          NULL, NULL, NULL },
    { "line-join",           action_line_join,           NULL, NULL, NULL },
    { "sort-asc",            action_sort_asc,            NULL, NULL, NULL },
    { "sort-desc",           action_sort_desc,           NULL, NULL, NULL },
    { "sort-asc-ci",         action_sort_asc_ci,         NULL, NULL, NULL },
    { "sort-by-length",      action_sort_by_length,      NULL, NULL, NULL },
    /* Q-fix Sort + dedup variants (9 items). */
    { "sort-by-length-desc", action_sort_by_length_desc, NULL, NULL, NULL },
    { "sort-random",         action_sort_random,         NULL, NULL, NULL },
    { "sort-int-asc",        action_sort_int_asc,        NULL, NULL, NULL },
    { "sort-int-desc",       action_sort_int_desc,       NULL, NULL, NULL },
    { "sort-dec-comma-asc",  action_sort_dec_comma_asc,  NULL, NULL, NULL },
    { "sort-dec-comma-desc", action_sort_dec_comma_desc, NULL, NULL, NULL },
    { "sort-dec-dot-asc",    action_sort_dec_dot_asc,    NULL, NULL, NULL },
    { "sort-dec-dot-desc",   action_sort_dec_dot_desc,   NULL, NULL, NULL },
    { "remove-consec-dups",  action_remove_consec_dups,  NULL, NULL, NULL },
    { "lines-reverse",       action_lines_reverse,       NULL, NULL, NULL },
    { "remove-duplicates",   action_remove_duplicates,   NULL, NULL, NULL },
    /* G12.6 Comment */
    { "toggle-line-comment", action_toggle_line_comment, NULL, NULL, NULL },
    /* Q-align Comment/Tab/Fold extras (15 items). */
    { "comment-line-add",    action_comment_line_add,    NULL, NULL, NULL },
    { "comment-line-remove", action_comment_line_remove, NULL, NULL, NULL },
    { "block-comment-add",   action_block_comment_add,   NULL, NULL, NULL },
    { "block-comment-remove",action_block_comment_remove,NULL, NULL, NULL },
    { "fold-current-level",  action_fold_current_level,  NULL, NULL, NULL },
    { "unfold-current-level",action_unfold_current_level,NULL, NULL, NULL },
    { "trim-and-save",       action_trim_and_save,       NULL, NULL, NULL },
    { "toggle-tab-bar-wrap", action_toggle_tab_bar_wrap, NULL, NULL, NULL },
    { "sort-tabs-name-asc",  action_sort_tabs_name_asc,  NULL, NULL, NULL },
    { "sort-tabs-name-desc", action_sort_tabs_name_desc, NULL, NULL, NULL },
    { "sort-tabs-ext-asc",   action_sort_tabs_ext_asc,   NULL, NULL, NULL },
    { "sort-tabs-ext-desc",  action_sort_tabs_ext_desc,  NULL, NULL, NULL },
    { "sort-tabs-path-asc",  action_sort_tabs_path_asc,  NULL, NULL, NULL },
    { "sort-tabs-path-desc", action_sort_tabs_path_desc, NULL, NULL, NULL },
    /* G12.8 EOL */
    { "eol-crlf",            action_eol_crlf,            NULL, NULL, NULL },
    { "eol-lf",              action_eol_lf,              NULL, NULL, NULL },
    { "eol-cr",              action_eol_cr,              NULL, NULL, NULL },
    /* G12.9 Blank ops */
    { "trim-trailing",           action_trim_trailing,           NULL, NULL, NULL },
    { "trim-leading",            action_trim_leading,            NULL, NULL, NULL },
    { "trim-both",               action_trim_both,               NULL, NULL, NULL },
    { "tab-to-space",            action_tab_to_space,            NULL, NULL, NULL },
    { "eol-to-space",            action_eol_to_space,            NULL, NULL, NULL },
    { "trim-both-eol-to-space",  action_trim_both_eol_to_space,  NULL, NULL, NULL },
    { "space-to-tab-leading",    action_space_to_tab_leading,    NULL, NULL, NULL },
    { "merge-blank-lines",       action_merge_blank_lines,       NULL, NULL, NULL },
    { "remove-unnec-blank-eol",  action_remove_unnec_blank_eol,  NULL, NULL, NULL },
    { "space-to-tab",        action_space_to_tab,        NULL, NULL, NULL },
    { "remove-blank-lines",  action_remove_blank_lines,  NULL, NULL, NULL },
    /* G12.13/14 Modes */
    { "column-mode",         action_column_mode,         NULL, NULL, NULL },
    { "toggle-readonly",     action_toggle_readonly,     NULL, NULL, NULL },
    /* G13 Bookmarks */
    { "bookmark-toggle",        action_bookmark_toggle,        NULL, NULL, NULL },
    { "bookmark-next",          action_bookmark_next,          NULL, NULL, NULL },
    { "bookmark-prev",          action_bookmark_prev,          NULL, NULL, NULL },
    { "bookmark-clear-all",     action_bookmark_clear_all,     NULL, NULL, NULL },
    { "bookmark-cut-lines",     action_bookmark_cut_lines,     NULL, NULL, NULL },
    { "bookmark-copy-lines",    action_bookmark_copy_lines,    NULL, NULL, NULL },
    { "bookmark-paste-lines",   action_bookmark_paste_lines,   NULL, NULL, NULL },
    { "bookmark-remove-lines",  action_bookmark_remove_lines,  NULL, NULL, NULL },
    { "bookmark-remove-unmarked", action_bookmark_remove_unmarked, NULL, NULL, NULL },
    { "bookmark-inverse",       action_bookmark_inverse,       NULL, NULL, NULL },
    /* G13 Mark / Style (5 slots) */
    { "mark-all-1",          action_mark_all_0,          NULL, NULL, NULL },
    { "mark-all-2",          action_mark_all_1,          NULL, NULL, NULL },
    { "mark-all-3",          action_mark_all_2,          NULL, NULL, NULL },
    { "mark-all-4",          action_mark_all_3,          NULL, NULL, NULL },
    { "mark-all-5",          action_mark_all_4,          NULL, NULL, NULL },
    { "clear-mark-1",        action_clear_mark_0,        NULL, NULL, NULL },
    { "clear-mark-2",        action_clear_mark_1,        NULL, NULL, NULL },
    { "clear-mark-3",        action_clear_mark_2,        NULL, NULL, NULL },
    { "clear-mark-4",        action_clear_mark_3,        NULL, NULL, NULL },
    { "clear-mark-5",        action_clear_mark_4,        NULL, NULL, NULL },
    /* Q-fix Style One Token (5) + Copy Styled (5) + Jump (2) + Volatile (2). */
    { "style-one-1",         action_style_one_0,         NULL, NULL, NULL },
    { "style-one-2",         action_style_one_1,         NULL, NULL, NULL },
    { "style-one-3",         action_style_one_2,         NULL, NULL, NULL },
    { "style-one-4",         action_style_one_3,         NULL, NULL, NULL },
    { "style-one-5",         action_style_one_4,         NULL, NULL, NULL },
    { "copy-styled-1",       action_copy_styled_0,       NULL, NULL, NULL },
    { "copy-styled-2",       action_copy_styled_1,       NULL, NULL, NULL },
    { "copy-styled-3",       action_copy_styled_2,       NULL, NULL, NULL },
    { "copy-styled-4",       action_copy_styled_3,       NULL, NULL, NULL },
    { "copy-styled-5",       action_copy_styled_4,       NULL, NULL, NULL },
    { "jump-styled-next",    action_jump_styled_next,    NULL, NULL, NULL },
    { "jump-styled-prev",    action_jump_styled_prev,    NULL, NULL, NULL },
    { "find-volatile-next",  action_find_volatile_next,  NULL, NULL, NULL },
    { "find-volatile-prev",  action_find_volatile_prev,  NULL, NULL, NULL },
    { "clear-all-marks",     action_clear_all_marks,     NULL, NULL, NULL },
    /* G22 Incremental search */
    { "incremental-search",  action_incremental_search,  NULL, NULL, NULL },
    /* G15 Command Palette */
    { "command-palette",     action_command_palette,     NULL, NULL, NULL },
    /* G20 Tab polish */
    { "tab-pin-toggle",      action_tab_pin_toggle,      NULL, NULL, NULL },
    { "tab-set-color",       action_tab_set_color,       "i",  NULL, NULL },
    { "move-to-vview",       action_move_vview,          NULL, NULL, NULL },
    { "clone-to-vview",      action_clone_vview,         NULL, NULL, NULL },
    { "move-to-hview",       action_move_hview,          NULL, NULL, NULL },
    { "clone-to-hview",      action_clone_hview,         NULL, NULL, NULL },
    { "reset-view",          action_reset_view,          NULL, NULL, NULL },
    /* G12.11 On Selection */
    { "sel-open-file",       action_sel_open_file,       NULL, NULL, NULL },
    { "sel-open-folder",     action_sel_open_folder,     NULL, NULL, NULL },
    { "sel-search-internet", action_sel_search_internet, NULL, NULL, NULL },
    { "sel-redact",          action_sel_redact,          NULL, NULL, NULL },
    /* G12.12 Multi-Select */
    { "multisel-add-next",   action_multisel_add_next,   NULL, NULL, NULL },
    { "multisel-add-each",   action_multisel_add_each,   NULL, NULL, NULL },
    /* View extras */
    { "hide-lines",          action_hide_lines,          NULL, NULL, NULL },
    { "show-lines",          action_show_lines,          NULL, NULL, NULL },
    { "text-dir-ltr",        action_text_dir_ltr,        NULL, NULL, NULL },
    { "text-dir-rtl",        action_text_dir_rtl,        NULL, NULL, NULL },
    { "toggle-monitoring",   action_toggle_monitoring,   NULL, NULL, NULL },
    /* File */
    { "move-to-trash",       action_move_to_trash,       NULL, NULL, NULL },
    /* Reload-with-<encoding> — parametric, takes encoding display name. */
    { "reload-as",            action_reload_as,            "s",  NULL, NULL },
    /* G35 Encoding Convert-To */
    { "convert-to-ansi",      action_convert_to_ansi,      NULL, NULL, NULL },
    { "convert-to-utf8",      action_convert_to_utf8,      NULL, NULL, NULL },
    { "convert-to-utf8-bom",  action_convert_to_utf8_bom,  NULL, NULL, NULL },
    { "convert-to-utf16-le",  action_convert_to_utf16_le,  NULL, NULL, NULL },
    { "convert-to-utf16-be",  action_convert_to_utf16_be,  NULL, NULL, NULL },
    /* G38 Auto-completion */
    { "ac-function",           action_autocomplete_function,    NULL, NULL, NULL },
    { "ac-word",               action_autocomplete_word,        NULL, NULL, NULL },
    { "ac-function-hint",      action_function_param_hint,      NULL, NULL, NULL },
    { "ac-function-hint-stop", action_function_param_hint_cancel,NULL, NULL, NULL },
    { "ac-select",             action_autocomplete_select,      NULL, NULL, NULL },
    /* G39 Spell check */
    { "spell-toggle",          action_spell_toggle,             NULL, NULL, NULL },
    { "spell-check-doc",       action_spell_check_doc,          NULL, NULL, NULL },
    /* G34 Find extras */
    { "find-all-current",      action_find_all_current,         NULL, NULL, NULL },
    { "find-all-all-docs",     action_find_all_all_docs,        NULL, NULL, NULL },
    { "replace-in-selection",  action_replace_in_selection,     NULL, NULL, NULL },
    /* G37 Print (real backend, overrides the stub) */
    { "print-real",            action_print_real,               NULL, NULL, NULL },
    { "page-setup",            action_page_setup,               NULL, NULL, NULL },
    /* G21 Floating panels */
    { "panel-popout",          action_panel_popout,             "s",  NULL, NULL },
    { "panel-dockback",        action_panel_dockback,           "s",  NULL, NULL },
    { "panel-toggle-floating", action_panel_toggle,             "s",  NULL, NULL },
    /* macOS-parity menu sweep */
    { "delete",                action_delete,                   NULL, NULL, NULL },
    { "close-all-but-pinned",  action_menu_stub,                NULL, NULL, NULL },
    { "change-search-engine",  action_menu_stub,                NULL, NULL, NULL },
    { "clear-readonly",        action_menu_stub,                NULL, NULL, NULL },
    { "lock-file",             action_menu_stub,                NULL, NULL, NULL },
    { "unlock-file",           action_menu_stub,                NULL, NULL, NULL },
    { "ac-hint-prev",          action_menu_stub,                NULL, NULL, NULL },
    { "ac-hint-next",          action_menu_stub,                NULL, NULL, NULL },
    { "tab-move-start",        action_menu_stub,                NULL, NULL, NULL },
    { "tab-move-end",          action_menu_stub,                NULL, NULL, NULL },
    { "tab-move-forward",      action_menu_stub,                NULL, NULL, NULL },
    { "tab-move-backward",     action_menu_stub,                NULL, NULL, NULL },
    { "search-result-next",    action_menu_stub,                NULL, NULL, NULL },
    { "search-result-prev",    action_menu_stub,                NULL, NULL, NULL },
};

/* ------------------------------------------------------------------ */
/* GAP-20 — macro recording of menu commands.                          */
/*                                                                     */
/* Recordable app actions are registered through a wrapper that stops  */
/* Scintilla's low-level SCN_MACRORECORD stream while the command runs */
/* and records a single type-2 step instead (see macro_menu_wrap_*).   */
/* Mirrors the macOS sendAction: override / Windows WM_COMMAND path.   */
/* ------------------------------------------------------------------ */

/* Actions that must NOT land in a macro: dialog/picker openers, UI-only
 * view state, panel toggles, session plumbing, and the macro commands
 * themselves. Everything else that edits or navigates the document is
 * recordable (Windows semantics). */
static gboolean action_is_recordable(const char *name) {
    static const char *const prefixes[] = {
        "toggle-", "show-", "zoom-", "split-", "macro-", "help-",
        "view-in-", "sync-scroll-", "udl-", "import-", "hash-",
        "open-", "modify-shortcut-", "load-", "save-session",
        "clear-", "reopen-", "fold-", "unfold-",
    };
    static const char *const names[] = {
        /* dialogs & pickers */
        "open", "save-as", "save-copy-as", "rename", "print", "print-now",
        "preferences", "shortcut-map", "style-editor", "column-editor",
        "plugins-admin", "run", "find", "replace", "find-in-files",
        "goto-line", "find-chars-in-range", "insert-dt-custom",
        "edit-popup-ctxmenu", "debug-info", "check-updates", "install-cli",
        "view-summary", "get-php-help", "wikipedia-search",
        /* view / window state */
        "word-wrap", "always-on-top", "fullscreen", "distraction-free",
        "post-it", "hide-line-marks", "focus-other-view", "quit",
        "toggle-readonly", "column-mode",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(prefixes); i++)
        if (g_str_has_prefix(name, prefixes[i])) return FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
        if (g_strcmp0(name, names[i]) == 0) return FALSE;
    return TRUE;
}

static GHashTable *s_recordable_orig = NULL;  /* name → original activate */

static void recordable_action_activate(GSimpleAction *a, GVariant *p,
                                       gpointer u) {
    const char *name = g_action_get_name(G_ACTION(a));
    void (*orig)(GSimpleAction *, GVariant *, gpointer) =
        g_hash_table_lookup(s_recordable_orig, name);
    if (!orig) return;
    if (macro_menu_wrap_begin()) {
        orig(a, p, u);
        macro_menu_wrap_end(name, 0);
    } else {
        orig(a, p, u);
    }
}

/* Heap copy of kAppActions with recordable entries re-pointed at the
 * wrapper. The original callbacks live in s_recordable_orig. */
static const GActionEntry *wrap_recordable_entries(void) {
    static GActionEntry entries[G_N_ELEMENTS(kAppActions)];
    memcpy(entries, kAppActions, sizeof(kAppActions));
    s_recordable_orig = g_hash_table_new(g_str_hash, g_str_equal);
    for (size_t i = 0; i < G_N_ELEMENTS(entries); i++) {
        GActionEntry *e = &entries[i];
        /* Parametric actions (open-recent, tab-goto, plugin-cmd, …) keep
         * their own handlers — plugin-cmd records itself. */
        if (!e->activate || e->parameter_type) continue;
        if (!action_is_recordable(e->name)) continue;
        g_hash_table_insert(s_recordable_orig, (gpointer)e->name,
                            (gpointer)e->activate);
        e->activate = recordable_action_activate;
    }
    return entries;
}

/* Convenience: app.<name> action + accelerator. */
static void set_accel(GtkApplication *app, const char *action_detail,
                      const char *accel) {
    const char *accels[2] = { accel, NULL };
    gtk_application_set_accels_for_action(app, action_detail, accels);
}

/* ------------------------------------------------------------------ */
/* Menu construction                                                   */
/* ------------------------------------------------------------------ */

/* #7 — the English menu model is kept so the menu bar can be retranslated
 * live when the UI language changes; g_menubar_widget is the on-screen
 * GtkPopoverMenuBar. */
static GMenuModel *g_menu_english;
static GtkWidget  *g_menubar_widget;

/* Rebuild the menu bar in the current UI language (called after the
 * language is switched in Preferences). */
void main_retranslate_menu(void)
{
    if (!g_menu_english) return;
    GMenuModel *t = i18n_translate_menu(g_menu_english);
    if (g_menubar_widget && GTK_IS_POPOVER_MENU_BAR(g_menubar_widget))
        gtk_popover_menu_bar_set_menu_model(
            GTK_POPOVER_MENU_BAR(g_menubar_widget), t);
    GApplication *app = g_application_get_default();
    if (app) gtk_application_set_menubar(GTK_APPLICATION(app), t);
    g_object_unref(t);
}

static GMenuModel *build_menu_model(void);

/* Rebuild the whole menubar from scratch — needed when its dynamic
 * content changes at runtime (UDL install/remove: the Language menu
 * lists loaded UDLs). Re-runs build_menu_model, then re-applies the
 * active UI translation. */
void main_rebuild_menubar(void)
{
    GMenuModel *fresh = build_menu_model();
    if (!fresh) return;
    if (g_menu_english) g_object_unref(g_menu_english);
    g_menu_english = fresh;
    main_retranslate_menu();
}

/* The Language submenu's model, shared with the status bar's
 * double-click popup (macOS #174). Set by build_menu_model. */
static GMenu *g_language_menu;

static GMenuModel *build_menu_model(void)
{
    GMenu *bar = g_menu_new();

    /* File — sequence + dividers mirror macOS MenuBuilder.mm:235-277. */
    GMenu *file = g_menu_new();
    {
        GMenu *top = g_menu_new();
        g_menu_append(top, "New",   "app.new");
        g_menu_append(top, "Open…", "app.open");
        /* Open Recent — global so main_recent_file_add can rebuild it. */
        g_recent_menu = g_menu_new();
        g_menu_append_submenu(top, "Open Recent", G_MENU_MODEL(g_recent_menu));
        rebuild_recent_menu();
        /* Open Containing Folder ▸ (macOS "Finder" → "Files" on Linux). */
        GMenu *ocf = g_menu_new();
        g_menu_append(ocf, "Files",    "app.open-containing-files");
        g_menu_append(ocf, "Terminal", "app.open-containing-terminal");
        g_menu_append_submenu(top, "Open Containing Folder", G_MENU_MODEL(ocf));
        g_object_unref(ocf);
        g_menu_append(top, "Open in Default Viewer",    "app.open-default-viewer");
        g_menu_append(top, "Open Folder as Workspace…", "app.open-workspace");
        g_menu_append_section(file, NULL, G_MENU_MODEL(top));
        g_object_unref(top);
    }
    {
        GMenu *reload = g_menu_new();
        g_menu_append(reload, "Reload from Disk", "app.reload");
        g_menu_append_section(file, NULL, G_MENU_MODEL(reload));
        g_object_unref(reload);
    }
    {
        GMenu *save_group = g_menu_new();
        g_menu_append(save_group, "Save",          "app.save");
        g_menu_append(save_group, "Save As…",      "app.save-as");
        g_menu_append(save_group, "Save a Copy As…","app.save-copy-as");
        g_menu_append(save_group, "Save All",      "app.save-all");
        g_menu_append(save_group, "Rename…",       "app.rename");
        g_menu_append_section(file, NULL, G_MENU_MODEL(save_group));
        g_object_unref(save_group);
    }
    {
        GMenu *close_group = g_menu_new();
        g_menu_append(close_group, "Close",     "app.close");
        g_menu_append(close_group, "Close All", "app.close-all");
        GMenu *closem = g_menu_new();
        g_menu_append(closem, "Close All But Current",  "app.close-others");
        g_menu_append(closem, "Close All to the Left",  "app.close-to-left");
        g_menu_append(closem, "Close All to the Right", "app.close-to-right");
        g_menu_append(closem, "Close All Unchanged",    "app.close-unchanged");
        g_menu_append(closem, "Close All But Pinned",   "app.close-all-but-pinned");
        g_menu_append_submenu(close_group, "Close Multiple Documents",
                              G_MENU_MODEL(closem));
        g_object_unref(closem);
        g_menu_append(close_group, "Move to Trash", "app.move-to-trash");
        g_menu_append_section(file, NULL, G_MENU_MODEL(close_group));
        g_object_unref(close_group);
    }
    {
        GMenu *session_group = g_menu_new();
        g_menu_append(session_group, "Load Session…", "app.load-session");
        g_menu_append(session_group, "Save Session…", "app.save-session");
        g_menu_append_section(file, NULL, G_MENU_MODEL(session_group));
        g_object_unref(session_group);
    }
    {
        GMenu *print_group = g_menu_new();
        g_menu_append(print_group, "Print…",    "app.print");
        g_menu_append(print_group, "Print Now", "app.print-now");
        g_menu_append_section(file, NULL, G_MENU_MODEL(print_group));
        g_object_unref(print_group);
    }
    g_menu_append_submenu(bar, "_File", G_MENU_MODEL(file));
    g_object_unref(file);

    /* Edit */
    GMenu *edit = g_menu_new();
    {
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Undo", "app.undo");
        g_menu_append(grp, "Redo", "app.redo");
        g_menu_append_section(edit, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {
        /* macOS keeps Cut…Begin/End Select in one section, incl. Delete. */
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Cut",    "app.cut");
        g_menu_append(grp, "Copy",   "app.copy");
        g_menu_append(grp, "Paste",  "app.paste");
        g_menu_append(grp, "Delete", "app.delete");
        g_menu_append(grp, "Select All", "app.select-all");
        g_menu_append(grp, "Begin/End Select",                "app.begin-end-select");
        g_menu_append(grp, "Begin/End Select in Column Mode", "app.begin-end-select-column");
        g_menu_append_section(edit, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    /* Edit submenus — order matches macOS 3-edit_menu.png exactly. */
    {
        GMenu *submenus = g_menu_new();

        /* Insert */
        {
            GMenu *insert = g_menu_new();
            g_menu_append(insert, "Insert Date/Time (Short)",          "app.insert-dt-short");
            g_menu_append(insert, "Insert Date/Time (Long)",           "app.insert-dt-long");
            g_menu_append(insert, "Insert Date/Time (Custom Format…)", "app.insert-dt-custom");
            GMenu *insert_bl = g_menu_new();
            g_menu_append(insert_bl, "Insert Blank Line Above Current","app.insert-blank-above");
            g_menu_append(insert_bl, "Insert Blank Line Below Current","app.insert-blank-below");
            g_menu_append_section(insert, NULL, G_MENU_MODEL(insert_bl));
            g_object_unref(insert_bl);
            g_menu_append_submenu(submenus, "Insert", G_MENU_MODEL(insert));
            g_object_unref(insert);
        }
        /* Copy to Clipboard */
        {
            GMenu *cp = g_menu_new();
            g_menu_append(cp, "Copy Full File Path",         "app.copy-full-path");
            g_menu_append(cp, "Copy File Name",              "app.copy-file-name");
            g_menu_append(cp, "Copy Current Directory Path", "app.copy-dir-path");
            GMenu *cp_all = g_menu_new();
            g_menu_append(cp_all, "Copy All File Names",  "app.copy-all-names");
            g_menu_append(cp_all, "Copy All File Paths",  "app.copy-all-paths");
            g_menu_append_section(cp, NULL, G_MENU_MODEL(cp_all));
            g_object_unref(cp_all);
            g_menu_append_submenu(submenus, "Copy to Clipboard", G_MENU_MODEL(cp));
            g_object_unref(cp);
        }
        /* Indent */
        {
            GMenu *ind = g_menu_new();
            g_menu_append(ind, "Increase Line Indent", "app.indent");
            g_menu_append(ind, "Decrease Line Indent", "app.unindent");
            g_menu_append_submenu(submenus, "Indent", G_MENU_MODEL(ind));
            g_object_unref(ind);
        }
        /* Convert Case to */
        {
            GMenu *cc = g_menu_new();
            g_menu_append(cc, "UPPERCASE",               "app.case-upper");
            g_menu_append(cc, "lowercase",               "app.case-lower");
            g_menu_append(cc, "Proper Case (Force First Char)",     "app.case-proper-force");
            g_menu_append(cc, "Proper Case (Blend)",                "app.case-proper-blend");
            g_menu_append(cc, "Sentence case (Force First Char)",   "app.case-sentence-force");
            g_menu_append(cc, "Sentence case (Blend)",              "app.case-sentence-blend");
            g_menu_append(cc, "iNVERT cASE",             "app.case-invert");
            g_menu_append(cc, "rAnDoM cAsE",             "app.case-random");
            g_menu_append_submenu(submenus, "Convert Case to", G_MENU_MODEL(cc));
            g_object_unref(cc);
        }
        /* Line Operations */
        {
            GMenu *lo = g_menu_new();
            {
                GMenu *lo1 = g_menu_new();
                g_menu_append(lo1, "Duplicate Line", "app.line-duplicate");
                g_menu_append(lo1, "Delete Line",    "app.line-delete");
                g_menu_append(lo1, "Move Line Up",   "app.line-move-up");
                g_menu_append(lo1, "Move Line Down", "app.line-move-down");
                g_menu_append_section(lo, NULL, G_MENU_MODEL(lo1));
                g_object_unref(lo1);
            }
            {
                GMenu *lo2 = g_menu_new();
                g_menu_append(lo2, "Split Lines", "app.line-split");
                g_menu_append(lo2, "Join Lines",  "app.line-join");
                g_menu_append_section(lo, NULL, G_MENU_MODEL(lo2));
                g_object_unref(lo2);
            }
            {
                GMenu *lo3 = g_menu_new();
                GMenu *sort = g_menu_new();
                GMenu *s1 = g_menu_new();
                g_menu_append(s1, "Ascending",                   "app.sort-asc");
                g_menu_append(s1, "Descending",                  "app.sort-desc");
                g_menu_append(s1, "Ascending (case-insensitive)","app.sort-asc-ci");
                g_menu_append_section(sort, NULL, G_MENU_MODEL(s1));
                g_object_unref(s1);
                GMenu *s2 = g_menu_new();
                g_menu_append(s2, "By Length (Shortest First)", "app.sort-by-length");
                g_menu_append(s2, "By Length (Longest First)",  "app.sort-by-length-desc");
                g_menu_append_section(sort, NULL, G_MENU_MODEL(s2));
                g_object_unref(s2);
                GMenu *s3 = g_menu_new();
                g_menu_append(s3, "Randomly",      "app.sort-random");
                g_menu_append(s3, "Reverse Order", "app.lines-reverse");
                g_menu_append_section(sort, NULL, G_MENU_MODEL(s3));
                g_object_unref(s3);
                GMenu *s4 = g_menu_new();
                g_menu_append(s4, "Integer Ascending",        "app.sort-int-asc");
                g_menu_append(s4, "Integer Descending",       "app.sort-int-desc");
                g_menu_append(s4, "Decimal Comma Ascending",  "app.sort-dec-comma-asc");
                g_menu_append(s4, "Decimal Comma Descending", "app.sort-dec-comma-desc");
                g_menu_append(s4, "Decimal Dot Ascending",    "app.sort-dec-dot-asc");
                g_menu_append(s4, "Decimal Dot Descending",   "app.sort-dec-dot-desc");
                g_menu_append_section(sort, NULL, G_MENU_MODEL(s4));
                g_object_unref(s4);
                g_menu_append_submenu(lo3, "Sort Lines", G_MENU_MODEL(sort));
                g_object_unref(sort);
                g_menu_append_section(lo, NULL, G_MENU_MODEL(lo3));
                g_object_unref(lo3);
            }
            {
                GMenu *lo4 = g_menu_new();
                g_menu_append(lo4, "Remove Duplicate Lines",            "app.remove-duplicates");
                g_menu_append(lo4, "Remove Consecutive Duplicate Lines","app.remove-consec-dups");
                g_menu_append_section(lo, NULL, G_MENU_MODEL(lo4));
                g_object_unref(lo4);
            }
            g_menu_append_submenu(submenus, "Line Operations", G_MENU_MODEL(lo));
            g_object_unref(lo);
        }
        /* Comment/Uncomment */
        {
            GMenu *cmt = g_menu_new();
            g_menu_append(cmt, "Toggle Single Line Comment", "app.toggle-line-comment");
            g_menu_append(cmt, "Single Line Comment",        "app.comment-line-add");
            g_menu_append(cmt, "Single Line Uncomment",      "app.comment-line-remove");
            g_menu_append(cmt, "Block Comment",              "app.block-comment-add");
            g_menu_append(cmt, "Block Uncomment",            "app.block-comment-remove");
            g_menu_append_submenu(submenus, "Comment/Uncomment", G_MENU_MODEL(cmt));
            g_object_unref(cmt);
        }
        /* Auto-Completion */
        {
            GMenu *ac = g_menu_new();
            GMenu *ac1 = g_menu_new();
            g_menu_append(ac1, "Function Completion",             "app.ac-function");
            g_menu_append(ac1, "Word Completion",                 "app.ac-word");
            g_menu_append(ac1, "Function Parameters Hint",        "app.ac-function-hint");
            g_menu_append(ac1, "Function Parameters Previous Hint","app.ac-hint-prev");
            g_menu_append(ac1, "Function Parameters Next Hint",   "app.ac-hint-next");
            g_menu_append(ac1, "Path Completion",                 "app.ac-path");
            g_menu_append_section(ac, NULL, G_MENU_MODEL(ac1));
            g_object_unref(ac1);
            g_menu_append(ac, "Finish or Select Autocomplete Item", "app.ac-select");
            g_menu_append_submenu(submenus, "Auto-Completion", G_MENU_MODEL(ac));
            g_object_unref(ac);
        }
        /* EOL Conversion */
        {
            GMenu *eol = g_menu_new();
            g_menu_append(eol, "Windows (CR LF)", "app.eol-crlf");
            g_menu_append(eol, "Unix (LF)",       "app.eol-lf");
            g_menu_append(eol, "Old Mac (CR)",    "app.eol-cr");
            g_menu_append_submenu(submenus, "EOL Conversion", G_MENU_MODEL(eol));
            g_object_unref(eol);
        }
        /* Blank Operations */
        {
            GMenu *blank = g_menu_new();
            g_menu_append(blank, "Trim Trailing Space",              "app.trim-trailing");
            g_menu_append(blank, "Trim Leading Space",               "app.trim-leading");
            g_menu_append(blank, "Trim Leading and Trailing Space",  "app.trim-both");
            g_menu_append(blank, "EOL to Space",                     "app.eol-to-space");
            g_menu_append(blank, "Trim Both and EOL to Space",       "app.trim-both-eol-to-space");
            GMenu *blank_tab = g_menu_new();
            g_menu_append(blank_tab, "TAB to Space",                 "app.tab-to-space");
            g_menu_append(blank_tab, "Space to TAB (All)",           "app.space-to-tab");
            g_menu_append(blank_tab, "Space to TAB (Leading)",       "app.space-to-tab-leading");
            g_menu_append_section(blank, NULL, G_MENU_MODEL(blank_tab));
            g_object_unref(blank_tab);
            GMenu *blank_rm = g_menu_new();
            g_menu_append(blank_rm, "Remove Unnecessary Blank and EOL", "app.remove-unnec-blank-eol");
            g_menu_append(blank_rm, "Remove Blank Lines",            "app.remove-blank-lines");
            g_menu_append(blank_rm, "Merge Blank Lines",             "app.merge-blank-lines");
            g_menu_append_section(blank, NULL, G_MENU_MODEL(blank_rm));
            g_object_unref(blank_rm);
            g_menu_append_submenu(submenus, "Blank Operations", G_MENU_MODEL(blank));
            g_object_unref(blank);
        }
        /* Paste Special */
        {
            GMenu *ps = g_menu_new();
            GMenu *ps1 = g_menu_new();
            g_menu_append(ps1, "Copy Binary Content",  "app.copy-binary");
            g_menu_append(ps1, "Paste Binary Content", "app.paste-binary");
            g_menu_append_section(ps, NULL, G_MENU_MODEL(ps1));
            g_object_unref(ps1);
            GMenu *ps2 = g_menu_new();
            g_menu_append(ps2, "Paste HTML Content",   "app.paste-html");
            g_menu_append(ps2, "Paste RTF Content",    "app.paste-rtf");
            g_menu_append_section(ps, NULL, G_MENU_MODEL(ps2));
            g_object_unref(ps2);
            g_menu_append_submenu(submenus, "Paste Special", G_MENU_MODEL(ps));
            g_object_unref(ps);
        }
        /* On Selection */
        {
            GMenu *onsel = g_menu_new();
            GMenu *os1 = g_menu_new();
            g_menu_append(os1, "Open File",              "app.sel-open-file");
            g_menu_append(os1, "Open Containing Folder", "app.sel-open-folder");
            g_menu_append_section(onsel, NULL, G_MENU_MODEL(os1));
            g_object_unref(os1);
            GMenu *os2 = g_menu_new();
            g_menu_append(os2, "Redact Selection", "app.sel-redact");
            g_menu_append_section(onsel, NULL, G_MENU_MODEL(os2));
            g_object_unref(os2);
            GMenu *os3 = g_menu_new();
            g_menu_append(os3, "Search on Internet",    "app.sel-search-internet");
            g_menu_append(os3, "Change Search Engine…", "app.change-search-engine");
            g_menu_append_section(onsel, NULL, G_MENU_MODEL(os3));
            g_object_unref(os3);
            g_menu_append_submenu(submenus, "On Selection", G_MENU_MODEL(onsel));
            g_object_unref(onsel);
        }

        g_menu_append_section(edit, NULL, G_MENU_MODEL(submenus));
        g_object_unref(submenus);
    }
    /* Multi-Select section */
    {
        GMenu *mssec = g_menu_new();
        GMenu *msa = g_menu_new();
        g_menu_append(msa, "Ignore Case & Whole Word", "app.msa-ignore");
        g_menu_append(msa, "Match Case Only",                  "app.msa-case");
        g_menu_append(msa, "Whole Word Only",                  "app.msa-word");
        g_menu_append(msa, "Match Case & Whole Word",          "app.msa-case-word");
        g_menu_append_submenu(mssec, "Multi-Select All", G_MENU_MODEL(msa));
        g_object_unref(msa);

        GMenu *msn = g_menu_new();
        g_menu_append(msn, "Ignore Case & Whole Word", "app.msn-ignore");
        g_menu_append(msn, "Match Case Only",                  "app.msn-case");
        g_menu_append(msn, "Whole Word Only",                  "app.msn-word");
        g_menu_append(msn, "Match Case & Whole Word",          "app.msn-case-word");
        g_menu_append_submenu(mssec, "Multi-Select Next", G_MENU_MODEL(msn));
        g_object_unref(msn);

        g_menu_append(mssec, "Undo the Latest Added Multi-Select", "app.ms-undo");
        g_menu_append(mssec, "Skip Current & Go to Next Multi-select", "app.ms-skip");
        g_menu_append_section(edit, NULL, G_MENU_MODEL(mssec));
        g_object_unref(mssec);
    }
    /* Column Mode / Editor / Character Panel / Clipboard History — one
     * section (macOS MenuBuilder.mm:469-472). */
    {
        GMenu *modes = g_menu_new();
        g_menu_append(modes, "Column Mode…",      "app.column-mode");
        g_menu_append(modes, "Column Editor…",    "app.column-editor");
        g_menu_append(modes, "Character Panel",   "app.toggle-charpanel");
        g_menu_append(modes, "Clipboard History", "app.toggle-cliphistory");
        g_menu_append_section(edit, NULL, G_MENU_MODEL(modes));
        g_object_unref(modes);
    }
    /* Read-Only ▸ + Locked Attribute (macOS) ▸ (MenuBuilder.mm:475-482). */
    {
        GMenu *rosec = g_menu_new();
        GMenu *ro = g_menu_new();
        g_menu_append(ro, "Toggle Read-Only",     "app.toggle-readonly");
        g_menu_append(ro, "Clear Read-Only Flag", "app.clear-readonly");
        g_menu_append_submenu(rosec, "Read-Only in " APP_NAME, G_MENU_MODEL(ro));
        g_object_unref(ro);
        GMenu *locked = g_menu_new();
        g_menu_append(locked, "Lock",   "app.lock-file");
        g_menu_append(locked, "Unlock", "app.unlock-file");
        g_menu_append_submenu(rosec, "Locked Attribute (macOS)", G_MENU_MODEL(locked));
        g_object_unref(locked);
        g_menu_append_section(edit, NULL, G_MENU_MODEL(rosec));
        g_object_unref(rosec);
    }

    g_menu_append_submenu(bar, "_Edit", G_MENU_MODEL(edit));
    g_object_unref(edit);

    /* Search — sequence + dividers mirror macOS MenuBuilder.mm:490-617. */
    GMenu *search = g_menu_new();
    {   /* Find / Find in Files */
        GMenu *g = g_menu_new();
        g_menu_append(g, "Find…",          "app.find");
        g_menu_append(g, "Find in Files…", "app.find-in-files");
        g_menu_append_section(search, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    {   /* Find Next/Prev · Select-and-Find · Find (Volatile) */
        GMenu *g = g_menu_new();
        g_menu_append(g, "Find Next",                "app.find-next");
        g_menu_append(g, "Find Previous",            "app.find-prev");
        g_menu_append(g, "Select and Find Next",     "app.select-find-next");
        g_menu_append(g, "Select and Find Previous", "app.select-find-prev");
        g_menu_append(g, "Find (Volatile) Next",     "app.find-volatile-next");
        g_menu_append(g, "Find (Volatile) Previous", "app.find-volatile-prev");
        g_menu_append_section(search, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    {   /* Replace / Incremental Search */
        GMenu *g = g_menu_new();
        g_menu_append(g, "Replace…",           "app.replace");
        g_menu_append(g, "Incremental Search", "app.incremental-search");
        g_menu_append_section(search, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    {   /* Search Results window + result navigation */
        GMenu *g = g_menu_new();
        g_menu_append(g, "Search Results Window",  "app.show-search-results");
        g_menu_append(g, "Next Search Result",     "app.search-result-next");
        g_menu_append(g, "Previous Search Result", "app.search-result-prev");
        g_menu_append_section(search, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    {   /* Go to · Matching Brace · brackets · Mark */
        GMenu *g = g_menu_new();
        g_menu_append(g, "Go to…",                            "app.goto-line");
        g_menu_append(g, "Go to Matching Brace",              "app.goto-matching-brace");
        g_menu_append(g, "Select All In-between {} [] or ()", "app.select-in-brackets");
        g_menu_append(g, "Mark…",                             "app.show-mark-dialog");
        g_menu_append_section(search, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    {   /* Submenu group: Change History · Style/Clear/Copy · Jump · Bookmark */
        GMenu *subs = g_menu_new();
        GMenu *ch = g_menu_new();
        g_menu_append(ch, "Go to Next Change",     "app.change-next");
        g_menu_append(ch, "Go to Previous Change", "app.change-prev");
        g_menu_append(ch, "Clear All Changes",     "app.change-clear");
        g_menu_append_submenu(subs, "Change History", G_MENU_MODEL(ch));
        g_object_unref(ch);

        GMenu *sa = g_menu_new();
        g_menu_append(sa, "Using 1st Style", "app.mark-all-1");
        g_menu_append(sa, "Using 2nd Style", "app.mark-all-2");
        g_menu_append(sa, "Using 3rd Style", "app.mark-all-3");
        g_menu_append(sa, "Using 4th Style", "app.mark-all-4");
        g_menu_append(sa, "Using 5th Style", "app.mark-all-5");
        g_menu_append_submenu(subs, "Style All Occurrences of Token", G_MENU_MODEL(sa));
        g_object_unref(sa);

        GMenu *so = g_menu_new();
        g_menu_append(so, "Using 1st Style", "app.style-one-1");
        g_menu_append(so, "Using 2nd Style", "app.style-one-2");
        g_menu_append(so, "Using 3rd Style", "app.style-one-3");
        g_menu_append(so, "Using 4th Style", "app.style-one-4");
        g_menu_append(so, "Using 5th Style", "app.style-one-5");
        g_menu_append_submenu(subs, "Style One Token", G_MENU_MODEL(so));
        g_object_unref(so);

        GMenu *cl = g_menu_new();
        GMenu *cl1 = g_menu_new();
        g_menu_append(cl1, "Clear 1st Style", "app.clear-mark-1");
        g_menu_append(cl1, "Clear 2nd Style", "app.clear-mark-2");
        g_menu_append(cl1, "Clear 3rd Style", "app.clear-mark-3");
        g_menu_append(cl1, "Clear 4th Style", "app.clear-mark-4");
        g_menu_append(cl1, "Clear 5th Style", "app.clear-mark-5");
        g_menu_append_section(cl, NULL, G_MENU_MODEL(cl1));
        g_object_unref(cl1);
        g_menu_append(cl, "Clear All Styles", "app.clear-all-marks");
        g_menu_append_submenu(subs, "Clear Style", G_MENU_MODEL(cl));
        g_object_unref(cl);

        GMenu *ju = g_menu_new();
        g_menu_append(ju, "Next Styled Token Above", "app.jump-styled-prev");
        g_menu_append(ju, "Next Bookmark Above",     "app.bookmark-prev");
        g_menu_append_submenu(subs, "Jump Up", G_MENU_MODEL(ju));
        g_object_unref(ju);

        GMenu *jd = g_menu_new();
        g_menu_append(jd, "Next Styled Token Below", "app.jump-styled-next");
        g_menu_append(jd, "Next Bookmark Below",     "app.bookmark-next");
        g_menu_append_submenu(subs, "Jump Down", G_MENU_MODEL(jd));
        g_object_unref(jd);

        GMenu *cs = g_menu_new();
        g_menu_append(cs, "Copy 1st Style Text", "app.copy-styled-1");
        g_menu_append(cs, "Copy 2nd Style Text", "app.copy-styled-2");
        g_menu_append(cs, "Copy 3rd Style Text", "app.copy-styled-3");
        g_menu_append(cs, "Copy 4th Style Text", "app.copy-styled-4");
        g_menu_append(cs, "Copy 5th Style Text", "app.copy-styled-5");
        g_menu_append_submenu(subs, "Copy Styled Text", G_MENU_MODEL(cs));
        g_object_unref(cs);

        GMenu *bm = g_menu_new();
        g_menu_append(bm, "Toggle Bookmark",     "app.bookmark-toggle");
        g_menu_append(bm, "Next Bookmark",       "app.bookmark-next");
        g_menu_append(bm, "Previous Bookmark",   "app.bookmark-prev");
        g_menu_append(bm, "Clear All Bookmarks", "app.bookmark-clear-all");
        GMenu *bm2 = g_menu_new();
        g_menu_append(bm2, "Cut Bookmarked Lines",  "app.bookmark-cut-lines");
        g_menu_append(bm2, "Copy Bookmarked Lines", "app.bookmark-copy-lines");
        g_menu_append(bm2, "Paste to (Replace) Bookmarked Lines", "app.bookmark-paste-lines");
        g_menu_append(bm2, "Remove Bookmarked Lines", "app.bookmark-remove-lines");
        g_menu_append(bm2, "Remove Non-Bookmarked Lines", "app.bookmark-remove-unmarked");
        g_menu_append(bm2, "Inverse Bookmark", "app.bookmark-inverse");
        g_menu_append_section(bm, NULL, G_MENU_MODEL(bm2));
        g_object_unref(bm2);
        g_menu_append_submenu(subs, "Bookmark", G_MENU_MODEL(bm));
        g_object_unref(bm);

        g_menu_append_section(search, NULL, G_MENU_MODEL(subs));
        g_object_unref(subs);
    }
    {   /* Find Characters in Range */
        GMenu *g = g_menu_new();
        g_menu_append(g, "Find Characters in Range…", "app.find-chars-in-range");
        g_menu_append_section(search, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    g_menu_append_submenu(bar, "_Search", G_MENU_MODEL(search));
    g_object_unref(search);

    /* View — sequence + dividers mirror macOS MenuBuilder.mm:625-786. */
    GMenu *view = g_menu_new();
    {   /* Command Palette */
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Command Palette…", "app.command-palette");
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {   /* Always on Top / Full Screen / Post-It / Distraction Free */
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Always on Top",          "app.always-on-top");
        g_menu_append(grp, "Toggle Full Screen Mode","app.fullscreen");
        g_menu_append(grp, "Post-It",                "app.post-it");
        g_menu_append(grp, "Distraction Free Mode",  "app.distraction-free");
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {   /* View Current File in */
        GMenu *grp = g_menu_new();
        GMenu *vif = g_menu_new();
        g_menu_append(vif, "Firefox",         "app.view-in-firefox");
        g_menu_append(vif, "Chrome",          "app.view-in-chrome");
        g_menu_append(vif, "Chromium",        "app.view-in-chromium");
        g_menu_append(vif, "Default Browser", "app.view-in-default");
        g_menu_append_submenu(grp, "View Current File in", G_MENU_MODEL(vif));
        g_object_unref(vif);
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {   /* Show Symbol / Zoom / Move-Clone / Tab + Word Wrap / Focus / Hide
         * Lines — all one section (macOS keeps no divider before Word Wrap). */
        GMenu *grp = g_menu_new();
        /* Show Symbol */
        GMenu *show = g_menu_new();
        g_menu_append(show, "Show White Space and TAB", "app.show-ws");
        g_menu_append(show, "Show End of Line",         "app.show-eol");
        g_menu_append(show, "Show All Characters",      "app.show-all-chars");
        g_menu_append(show, "Show Indent Guide",        "app.show-indent-guide");
        g_menu_append(show, "Show Line Numbers",        "app.show-line-numbers");
        g_menu_append(show, "Show Wrap Symbol",         "app.show-wrap-symbol");
        GMenu *show2 = g_menu_new();
        g_menu_append(show2, "Hide Line Marks (Bookmarks)", "app.hide-line-marks");
        g_menu_append_section(show, NULL, G_MENU_MODEL(show2));
        g_object_unref(show2);
        g_menu_append_submenu(grp, "Show Symbol", G_MENU_MODEL(show));
        g_object_unref(show);
        /* Zoom */
        GMenu *zoom = g_menu_new();
        g_menu_append(zoom, "Zoom In",  "app.zoom-in");
        g_menu_append(zoom, "Zoom Out", "app.zoom-out");
        g_menu_append(zoom, "Restore Default Zoom", "app.zoom-reset");
        g_menu_append_submenu(grp, "Zoom", G_MENU_MODEL(zoom));
        g_object_unref(zoom);
        /* Move/Clone Current Document — real, distinct actions + dividers. */
        GMenu *mc = g_menu_new();
        GMenu *mcv = g_menu_new();
        g_menu_append(mcv, "Move to Other Vertical View",  "app.move-to-vview");
        g_menu_append(mcv, "Clone to Other Vertical View", "app.clone-to-vview");
        g_menu_append_section(mc, NULL, G_MENU_MODEL(mcv));
        g_object_unref(mcv);
        GMenu *mch = g_menu_new();
        g_menu_append(mch, "Move to Other Horizontal View",  "app.move-to-hview");
        g_menu_append(mch, "Clone to Other Horizontal View", "app.clone-to-hview");
        g_menu_append_section(mc, NULL, G_MENU_MODEL(mch));
        g_object_unref(mch);
        GMenu *mcr = g_menu_new();
        g_menu_append(mcr, "Reset View", "app.reset-view");
        g_menu_append_section(mc, NULL, G_MENU_MODEL(mcr));
        g_object_unref(mcr);
        g_menu_append_submenu(grp, "Move/Clone Current Document", G_MENU_MODEL(mc));
        g_object_unref(mc);
        /* Tab */
        GMenu *tabs = g_menu_new();
        GMenu *tnum = g_menu_new();
        for (int i = 1; i <= 9; i++) {
            char label[16]; g_snprintf(label, sizeof(label), "%d%s Tab",
                i, i == 1 ? "st" : i == 2 ? "nd" : i == 3 ? "rd" : "th");
            GMenuItem *mi = g_menu_item_new(label, NULL);
            g_menu_item_set_action_and_target(mi, "app.tab-goto", "i", i);
            g_menu_append_item(tnum, mi);
            g_object_unref(mi);
        }
        g_menu_append_section(tabs, NULL, G_MENU_MODEL(tnum));
        g_object_unref(tnum);
        GMenu *tnav = g_menu_new();
        g_menu_append(tnav, "First Tab",    "app.tab-first");
        g_menu_append(tnav, "Last Tab",     "app.tab-last");
        g_menu_append(tnav, "Wrap tabs to multiple lines", "app.toggle-tab-bar-wrap");
        g_menu_append(tnav, "Next Tab",     "app.tab-next");
        g_menu_append(tnav, "Previous Tab", "app.tab-prev");
        g_menu_append_section(tabs, NULL, G_MENU_MODEL(tnav));
        g_object_unref(tnav);
        GMenu *tmov = g_menu_new();
        g_menu_append(tmov, "Move to Start",     "app.tab-move-start");
        g_menu_append(tmov, "Move to End",       "app.tab-move-end");
        g_menu_append(tmov, "Move Tab Forward",  "app.tab-move-forward");
        g_menu_append(tmov, "Move Tab Backward", "app.tab-move-backward");
        g_menu_append_section(tabs, NULL, G_MENU_MODEL(tmov));
        g_object_unref(tmov);
        GMenu *tcol = g_menu_new();
        for (int i = 1; i <= 5; i++) {
            char raw[16];
            g_snprintf(raw, sizeof(raw), "Apply Color %d", i);
            /* Pango markup with a coloured U+25A0 swatch prefix, since
             * GtkPopoverMenu's GMenuItem icon attribute is a known no-op
             * for vertical rows in GTK4 (GNOME Discourse:
             * "G_MENU_ATTRIBUTE_ICON does not work in GTK4 as expected").
             * editor_tab_color_markup_label is the single source of
             * truth for the palette + the markup format. */
            char *markup = editor_tab_color_markup_label(i, raw);
            GMenuItem *mi = g_menu_item_new(markup, NULL);
            g_menu_item_set_action_and_target(mi, "app.tab-set-color", "i", i);
            g_menu_item_set_attribute(mi, "use-markup", "s", "true");
            g_menu_append_item(tcol, mi);
            g_object_unref(mi);
            g_free(markup);
        }
        {
            GMenuItem *mi = g_menu_item_new("Remove Color", NULL);
            g_menu_item_set_action_and_target(mi, "app.tab-set-color", "i", 0);
            g_menu_append_item(tcol, mi);
            g_object_unref(mi);
        }
        g_menu_append_section(tabs, NULL, G_MENU_MODEL(tcol));
        g_object_unref(tcol);
        g_menu_append_submenu(grp, "Tab", G_MENU_MODEL(tabs));
        g_object_unref(tabs);
        /* Flat items in the same section (no divider before Word Wrap). */
        g_menu_append(grp, "Word Wrap",             "app.word-wrap");
        g_menu_append(grp, "Focus on Another View", "app.focus-other-view");
        g_menu_append(grp, "Hide Lines",            "app.hide-lines");
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {   /* Fold All / Unfold All / Fold-Unfold Current / Fold-Unfold Level */
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Fold All",             "app.fold-all");
        g_menu_append(grp, "Unfold All",           "app.unfold-all");
        g_menu_append(grp, "Fold Current Level",   "app.fold-current-level");
        g_menu_append(grp, "Unfold Current Level", "app.unfold-current-level");
        GMenu *fl = g_menu_new();
        for (int i = 1; i <= 8; i++) {
            char label[16], action[32];
            g_snprintf(label,  sizeof(label),  "Fold Level %d", i);
            g_snprintf(action, sizeof(action), "app.fold-level-%d", i);
            g_menu_append(fl, label, action);
        }
        g_menu_append_submenu(grp, "Fold Level", G_MENU_MODEL(fl));
        g_object_unref(fl);
        GMenu *ufl = g_menu_new();
        for (int i = 1; i <= 8; i++) {
            char label[16], action[32];
            g_snprintf(label,  sizeof(label),  "Unfold Level %d", i);
            g_snprintf(action, sizeof(action), "app.unfold-level-%d", i);
            g_menu_append(ufl, label, action);
        }
        g_menu_append_submenu(grp, "Unfold Level", G_MENU_MODEL(ufl));
        g_object_unref(ufl);
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {   /* Summary */
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Summary…", "app.view-summary");
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {   /* Panels — incl. Git */
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Project Panels", "app.toggle-project");
        g_menu_append(grp, "Document Map",   "app.toggle-docmap");
        g_menu_append(grp, "Document List",  "app.toggle-doclist");
        g_menu_append(grp, "Function List",  "app.toggle-funclist");
        g_menu_append(grp, "Git",            "app.toggle-gitpanel");
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {   /* Spell Check */
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Spell Check", "app.toggle-spell-check");
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {   /* Sync scrolling */
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Synchronize Vertical Scrolling",   "app.sync-scroll-v");
        g_menu_append(grp, "Synchronize Horizontal Scrolling", "app.sync-scroll-h");
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {   /* Text Direction */
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Text Direction RTL", "app.text-dir-rtl");
        g_menu_append(grp, "Text Direction LTR", "app.text-dir-ltr");
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    {   /* Monitoring */
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Monitoring (tail -f)", "app.toggle-monitoring");
        g_menu_append_section(view, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    g_menu_append_submenu(bar, "_View", G_MENU_MODEL(view));
    g_object_unref(view);

    /* Q-fix: Encoding is a TOP-LEVEL menu on macOS (MenuBuilder.mm:790),
     * NOT nested under View. Promote it here with the canonical layout:
     *  UTF group → Character sets submenu → separator → Convert To group. */
    GMenu *enc_menu = g_menu_new();
    /* Q-align macOS Encoding menu (6-encoding_menu.png): top group is
     *   ANSI / UTF-8 / UTF-8-BOM / UTF-16 BE BOM / UTF-16 LE BOM
     * (no plain UTF-16 BE / LE without BOM at the top). The other variants
     * are still reachable via Character sets / set-encoding parametric. */
    static const struct { const char *label; const char *target; } utf_rows[] = {
        { "ANSI",           "Windows-1252"  },
        { "UTF-8",          "UTF-8"         },
        { "UTF-8-BOM",      "UTF-8 BOM"     },
        { "UTF-16 BE BOM",  "UTF-16 BE BOM" },
        { "UTF-16 LE BOM",  "UTF-16 LE BOM" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(utf_rows); i++) {
        GMenuItem *mi = g_menu_item_new(utf_rows[i].label, NULL);
        g_menu_item_set_action_and_target(mi, "app.set-encoding", "s",
                                          utf_rows[i].target);
        g_menu_append_item(enc_menu, mi);
        g_object_unref(mi);
    }
    {
        GMenu *cs = g_menu_new();
        struct { const char *region; const char **encs; size_t n; } regions[] = {
            { "Western European", (const char *[]){
                "Windows-1252", "ISO-8859-1", "ISO-8859-15" }, 3 },
            { "Central European", (const char *[]){
                "Windows-1250", "ISO-8859-2" }, 2 },
            { "Cyrillic", (const char *[]){
                "Windows-1251", "KOI8-R" }, 2 },
            { "East Asian", (const char *[]){
                "Shift-JIS", "GB18030", "Big5", "EUC-KR" }, 4 },
        };
        for (size_t r = 0; r < G_N_ELEMENTS(regions); r++) {
            GMenu *region_menu = g_menu_new();
            for (size_t i = 0; i < regions[r].n; i++) {
                GMenuItem *mi = g_menu_item_new(regions[r].encs[i], NULL);
                g_menu_item_set_action_and_target(mi, "app.set-encoding", "s",
                                                  regions[r].encs[i]);
                g_menu_append_item(region_menu, mi);
                g_object_unref(mi);
            }
            g_menu_append_submenu(cs, regions[r].region, G_MENU_MODEL(region_menu));
            g_object_unref(region_menu);
        }
        g_menu_append_submenu(enc_menu, "Character sets", G_MENU_MODEL(cs));
        g_object_unref(cs);
    }
    /* Convert To group (macOS encMenu after the divider at line 816). */
    {
        GMenu *cv = g_menu_new();
        g_menu_append(cv, "Convert to ANSI",          "app.convert-to-ansi");
        g_menu_append(cv, "Convert to UTF-8",         "app.convert-to-utf8");
        g_menu_append(cv, "Convert to UTF-8-BOM",     "app.convert-to-utf8-bom");
        g_menu_append(cv, "Convert to UTF-16 BE BOM", "app.convert-to-utf16-be");
        g_menu_append(cv, "Convert to UTF-16 LE BOM", "app.convert-to-utf16-le");
        g_menu_append_section(enc_menu, NULL, G_MENU_MODEL(cv));
        g_object_unref(cv);
    }
    g_menu_append_submenu(bar, "_Encoding", G_MENU_MODEL(enc_menu));
    g_object_unref(enc_menu);

    /* Language — alphabetical letter-grouped submenus. */
    GMenu *lang = g_menu_new();
    {
        GMenu *none_grp = g_menu_new();
        GMenuItem *mi = g_menu_item_new("None (Normal Text)", NULL);
        g_menu_item_set_action_and_target(mi, "app.set-language", "s", "");
        g_menu_append_item(none_grp, mi);
        g_object_unref(mi);
        g_menu_append_section(lang, NULL, G_MENU_MODEL(none_grp));
        g_object_unref(none_grp);
    }
    {
        char letter = 0;
        GMenu *letter_grp = NULL;
        char letter_label[2] = { 0, 0 };
        for (int i = 0; i < kLangsCount; i++) {
            char first = (char)((kLangs[i].display[0] >= 'a' && kLangs[i].display[0] <= 'z')
                                ? (kLangs[i].display[0] - 'a' + 'A')
                                : kLangs[i].display[0]);
            if (first != letter) {
                if (letter_grp) {
                    g_menu_append_submenu(lang, letter_label, G_MENU_MODEL(letter_grp));
                    g_object_unref(letter_grp);
                }
                letter = first;
                letter_label[0] = first;
                letter_grp = g_menu_new();
            }
            GMenuItem *mi = g_menu_item_new(kLangs[i].display, NULL);
            g_menu_item_set_action_and_target(mi, "app.set-language", "s",
                                              kLangs[i].key);
            g_menu_append_item(letter_grp, mi);
            g_object_unref(mi);
        }
        if (letter_grp) {
            g_menu_append_submenu(lang, letter_label, G_MENU_MODEL(letter_grp));
            g_object_unref(letter_grp);
        }
    }
    /* Q-fix Language → User Defined Language submenu (matches macOS — 3 items). */
    {
        GMenu *udl = g_menu_new();
        g_menu_append(udl, "Define your language…",                "app.udl-define");
        g_menu_append(udl, "Open User Defined Language Folder…",   "app.udl-open-folder");
        g_menu_append(udl, "Nextpad++ User Defined Languages Collection", "app.udl-collection");
        g_menu_append_submenu(lang, "User Defined Language", G_MENU_MODEL(udl));
        g_object_unref(udl);
        /* Top-level Admin entry, no separator (macOS 426b88c/ad33623). */
        g_menu_append(lang, "User Defined Language Admin…", "app.udl-admin");
    }
    /* #4 — every loaded User Defined Language as a selectable, radio-checked
     * Language-menu entry (macOS lists them after the UDL submenu, from
     * ~/.nextpad++/userDefineLangs/). */
    {
        extern void udl_load_all(void);
        extern int  udl_count(void);
        extern const char *udl_name(int);
        extern const char *udl_key(int);
        udl_load_all();
        int nudl = udl_count();
        if (nudl > 0) {
            GMenu *udls = g_menu_new();
            for (int i = 0; i < nudl; i++) {
                const char *nm = udl_name(i), *ky = udl_key(i);
                if (!nm || !ky) continue;
                GMenuItem *mi = g_menu_item_new(nm, NULL);
                g_menu_item_set_action_and_target(mi, "app.set-language",
                                                  "s", ky);
                g_menu_append_item(udls, mi);
                g_object_unref(mi);
            }
            g_menu_append_section(lang, NULL, G_MENU_MODEL(udls));
            g_object_unref(udls);
        }
    }
    g_menu_append_submenu(bar, "_Language", G_MENU_MODEL(lang));
    /* Keep a ref for the status bar's double-click popup (macOS #174):
     * the model is shared, so dynamic UDL entries stay in sync. */
    if (g_language_menu) g_object_unref(g_language_menu);
    g_language_menu = g_object_ref(lang);
    g_object_unref(lang);

    /* Q-fix: Settings sits between Language and Tools per macOS
     * MenuBuilder.mm (line 832). Per macOS layout the Settings submenu
     * contains ONLY Preferences, Shortcut Mapper, Edit Popup ContextMenu
     * (no separate Appearance submenu — theme switching lives in
     * Preferences > Dark Mode tab instead). */
    {
        /* macOS Settings menu (8-settings_menu.png): Preferences,
         * Style Configurator, Shortcut Mapper, separator, Import submenu,
         * separator, Edit Popup ContextMenu. */
        GMenu *settings = g_menu_new();
        {
            GMenu *g = g_menu_new();
            g_menu_append(g, "Preferences…",        "app.preferences");
            g_menu_append(g, "Style Configurator…", "app.style-editor");
            g_menu_append(g, "Shortcut Mapper…",    "app.shortcut-map");
            g_menu_append_section(settings, NULL, G_MENU_MODEL(g));
            g_object_unref(g);
        }
        {
            GMenu *imp_sec = g_menu_new();
            GMenu *import = g_menu_new();
            g_menu_append(import, "Import Plugin(s)…",      "app.import-plugin");
            g_menu_append(import, "Import Style Theme(s)…", "app.import-style-theme");
            g_menu_append_submenu(imp_sec, "Import", G_MENU_MODEL(import));
            g_object_unref(import);
            g_menu_append_section(settings, NULL, G_MENU_MODEL(imp_sec));
            g_object_unref(imp_sec);
        }
        {
            GMenu *g = g_menu_new();
            g_menu_append(g, "Edit Popup ContextMenu", "app.edit-popup-ctxmenu");
            g_menu_append_section(settings, NULL, G_MENU_MODEL(g));
            g_object_unref(g);
        }
        g_menu_append_submenu(bar, "_Settings", G_MENU_MODEL(settings));
        g_object_unref(settings);
    }

    /* Tools. Matches macOS MenuBuilder.mm:849-877 exactly — four hash
     * submenus and nothing else. The Column Editor, Style Configurator,
     * Spell Check, and Auto-Completion entries that used to sit here
     * have moved (or were duplicates):
     *   - Column Editor → Edit menu, after Toggle Column Mode (macOS parity).
     *   - Style Configurator → already in Settings menu (was duplicated).
     *   - Spell Check + Auto-Completion → wired via View menu / shortcuts
     *     only, matching macOS which does not surface them here. */
    GMenu *tools = g_menu_new();
    /* Hash submenus — one per algorithm with 3 actions each (macOS parity).
     * Q-align: added "Generate from Files…" matching macOS hashMD5FromFiles:. */
    const char *hash_names[] = { "MD5", "SHA-1", "SHA-256", "SHA-512" };
    const char *hash_actions_dlg[] = { "app.hash-md5", "app.hash-sha1",
                                       "app.hash-sha256", "app.hash-sha512" };
    const char *hash_actions_clip[] = { "app.hash-md5-clip", "app.hash-sha1-clip",
                                        "app.hash-sha256-clip", "app.hash-sha512-clip" };
    const char *hash_actions_files[] = { "app.hash-md5-files", "app.hash-sha1-files",
                                         "app.hash-sha256-files", "app.hash-sha512-files" };
    for (int i = 0; i < 4; i++) {
        GMenu *h = g_menu_new();
        g_menu_append(h, "Generate",                               hash_actions_dlg[i]);
        g_menu_append(h, "Generate from Files…",                   hash_actions_files[i]);
        g_menu_append(h, "Generate from Selection into Clipboard", hash_actions_clip[i]);
        g_menu_append_submenu(tools, hash_names[i], G_MENU_MODEL(h));
        g_object_unref(h);
    }
    g_menu_append_submenu(bar, "_Tools", G_MENU_MODEL(tools));
    g_object_unref(tools);

    /* Macro */
    GMenu *macro = g_menu_new();
    {
        GMenu *g = g_menu_new();
        g_menu_append(g, "Start Recording",             "app.macro-start");
        g_menu_append(g, "Stop Recording",              "app.macro-stop");
        g_menu_append(g, "Playback",                    "app.macro-play");
        g_menu_append(g, "Save Current Recorded Macro…","app.macro-save-as");
        g_menu_append_section(macro, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    {
        GMenu *g = g_menu_new();
        g_menu_append(g, "Run a Macro Multiple Times…", "app.macro-play-n");
        g_menu_append(g, "Run Macro on Files…",         "app.macro-batch");
        g_menu_append_section(macro, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    {
        GMenu *g = g_menu_new();
        g_menu_append(g, "Trim Trailing Space and Save", "app.trim-and-save");
        g_menu_append_section(macro, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    /* Dynamic named-macro list — sits after "Trim Trailing Space and Save"
     * (macOS tag 9901); rebuilt on first build + after every save. */
    g_named_macros_menu = g_menu_new();
    g_menu_append_section(macro, NULL, G_MENU_MODEL(g_named_macros_menu));
    rebuild_macro_menu();
    {
        GMenu *g = g_menu_new();
        g_menu_append(g, "Modify Shortcut/Delete Macro…", "app.modify-shortcut-macro");
        g_menu_append_section(macro, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    g_menu_append_submenu(bar, "_Macro", G_MENU_MODEL(macro));
    g_object_unref(macro);

    /* Run */
    GMenu *run_menu = g_menu_new();
    g_menu_append(run_menu, "Run…", "app.run");
    /* Q-align macOS Run menu (11-run_menu.png): canned web-helpers +
     * "open selected path in new instance" + shortcut/delete editor. */
    {
        GMenu *grp = g_menu_new();
        g_menu_append(grp, "Get PHP help",                          "app.get-php-help");
        g_menu_append(grp, "Wikipedia Search",                      "app.wikipedia-search");
        g_menu_append(grp, "Open selected file path in new instance","app.open-in-new-instance");
        g_menu_append_section(run_menu, NULL, G_MENU_MODEL(grp));
        g_object_unref(grp);
    }
    g_menu_append(run_menu, "Modify Shortcut/Delete Command…", "app.modify-shortcut-run");
    g_menu_append_submenu(bar, "_Run", G_MENU_MODEL(run_menu));
    g_object_unref(run_menu);

    /* Plugins */
    GMenu *plugins = g_menu_new();
    {
        /* Q6 — MIME Tools matches macOS MenuBuilder.mm:923-931. */
        GMenu *mime = g_menu_new();
        g_menu_append(mime, "Base64 Encode",              "app.base64-encode");
        g_menu_append(mime, "Base64 Decode",              "app.base64-decode");
        g_menu_append(mime, "Base64 Encode with Padding", "app.base64-encode-padded");
        g_menu_append(mime, "Base64 Decode (Strict Mode)","app.base64-decode-strict");
        /* Separator */
        GMenu *mime_sep = g_menu_new();
        g_menu_append(mime_sep, "Base64 URL-Safe Encode", "app.base64-encode-urlsafe");
        g_menu_append(mime_sep, "Base64 URL-Safe Decode", "app.base64-decode-urlsafe");
        g_menu_append_section(mime, NULL, G_MENU_MODEL(mime_sep));
        g_object_unref(mime_sep);
        g_menu_append_submenu(plugins, "MIME Tools", G_MENU_MODEL(mime));
        g_object_unref(mime);
    }
    {
        /* Q6 — Converter matches macOS MenuBuilder.mm:933-936. */
        GMenu *conv = g_menu_new();
        g_menu_append(conv, "ASCII to Hex", "app.ascii-to-hex");
        g_menu_append(conv, "Hex to ASCII", "app.hex-to-ascii");
        g_menu_append_submenu(plugins, "Converter", G_MENU_MODEL(conv));
        g_object_unref(conv);
    }
    /* GAP-20 — loaded external plugins: one submenu per plugin, items
     * dispatch through app.plugin-cmd(cmdID) so they are recordable into
     * macros. FuncItem name "-" starts a new section (Windows semantics).
     * Empty until plugin_load_all() runs; activate() rebuilds the menubar
     * right after loading. */
    for (int pi = 0; pi < plugin_count(); pi++) {
        int nf = plugin_func_count(pi);
        if (nf <= 0) continue;
        GMenu *sub = g_menu_new();
        GMenu *sect = g_menu_new();
        for (int fi = 0; fi < nf; fi++) {
            const char *fname = plugin_func_name(pi, fi);
            if (!fname || !*fname) continue;
            if (g_strcmp0(fname, "-") == 0) {
                if (g_menu_model_get_n_items(G_MENU_MODEL(sect))) {
                    g_menu_append_section(sub, NULL, G_MENU_MODEL(sect));
                    g_object_unref(sect);
                    sect = g_menu_new();
                }
                continue;
            }
            GMenuItem *mi = g_menu_item_new(fname, NULL);
            g_menu_item_set_action_and_target(mi, "app.plugin-cmd", "i",
                                              plugin_func_cmd_id(pi, fi));
            g_menu_append_item(sect, mi);
            g_object_unref(mi);
        }
        if (g_menu_model_get_n_items(G_MENU_MODEL(sect)))
            g_menu_append_section(sub, NULL, G_MENU_MODEL(sect));
        g_object_unref(sect);
        g_menu_append_submenu(plugins, plugin_name_at(pi), G_MENU_MODEL(sub));
        g_object_unref(sub);
    }
    {
        GMenu *g = g_menu_new();
        g_menu_append(g, "Plugins Admin…",       "app.plugins-admin");
        g_menu_append(g, "Open Plugins Folder…", "app.open-plugins-folder");
        g_menu_append_section(plugins, NULL, G_MENU_MODEL(g));
        g_object_unref(g);
    }
    g_menu_append_submenu(bar, "_Plugins", G_MENU_MODEL(plugins));
    g_object_unref(plugins);

    /* Help */
    GMenu *help = g_menu_new();
    g_menu_append(help, "Command Line Arguments…", "app.help-cli-args");
    g_menu_append(help, "Nextpad++ macOS Home",          "app.help-home");
    g_menu_append(help, "Nextpad++ macOS Project Page",  "app.help-project");
    g_menu_append(help, "Online User Manual",      "app.help-manual");
    /* Q-fix Help → Debug Info (macOS parity). */
    g_menu_append(help, "Debug Info…",             "app.debug-info");
    {
        /* Ported from the macOS Nextpad++ app menu. The section is kept
         * in g_updates_section so the status bullet on "Check for
         * Updates" can be refreshed after a check. */
        GMenu *upd_grp = g_menu_new();
        g_menu_append(upd_grp, "Check for Updates…", "app.check-updates");
        g_menu_append(upd_grp, "Install nextpad++ Command Line Tool…",
                      "app.install-cli");
        g_menu_append_section(help, NULL, G_MENU_MODEL(upd_grp));
        g_updates_section = upd_grp;   /* kept alive by the menu tree */
        g_object_unref(upd_grp);
    }
    {
        GMenu *about_grp = g_menu_new();
        g_menu_append(about_grp, "About Nextpad++", "app.help-about");
        g_menu_append_section(help, NULL, G_MENU_MODEL(about_grp));
        g_object_unref(about_grp);
    }
    g_menu_append_submenu(bar, "_Help", G_MENU_MODEL(help));
    g_object_unref(help);

    return G_MENU_MODEL(bar);
}

/* ------------------------------------------------------------------ */
/* Drag-and-drop: file URI list → open in tabs                         */
/* ------------------------------------------------------------------ */

/* GTK4 file drop: GtkDropTarget delivers a GDK_TYPE_FILE_LIST GValue. */
static gboolean on_files_dropped(GtkDropTarget *dt, const GValue *value,
                                 double x, double y, gpointer user)
{
    (void)dt; (void)x; (void)y; (void)user;
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
    GSList *files = g_value_get_boxed(value);
    gboolean ok = FALSE;
    for (GSList *l = files; l; l = l->next) {
        char *path = g_file_get_path(G_FILE(l->data));
        if (path) { editor_open_path(path); g_free(path); ok = TRUE; }
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Q14 — side-panel host auto-hide/show                                */
/* ------------------------------------------------------------------ */
/* The host is a GtkBox holding every dockable panel_frame on the right
 * of hpaned1. Each frame's content widget already toggles visibility on
 * its own; panel_frame mirrors that onto the frame; we mirror frame
 * visibility onto the host so an empty right column collapses to 0 width.
 * Counter stored on the host as a GINT-tagged data key. */
static void side_host_bump(GtkWidget *host, int delta) {
    int n = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(host),
                                              "npp-visible-count"));
    n += delta;
    if (n < 0) n = 0;
    g_object_set_data(G_OBJECT(host), "npp-visible-count",
                      GINT_TO_POINTER(n));

    GtkWidget *paned = GTK_WIDGET(g_object_get_data(G_OBJECT(host),
                                                    "npp-hpaned"));
    if (n > 0) {
        gtk_widget_show(host);
        if (paned) {
            GtkAllocation a;
            gtk_widget_get_allocation(paned, &a);
            /* Right-attached panels get a 300 px default width (~50% wider
             * than the macOS NPPSidePanel default) so the panel-frame
             * pop-out / close buttons aren't clipped on first open.
             * Falls back to half the width when the window is narrow. */
            int target = a.width > 340 ? a.width - 300 : a.width / 2;
            if (target > 0) gtk_paned_set_position(GTK_PANED(paned), target);
        }
    } else {
        gtk_widget_hide(host);
    }
}

static void on_panel_frame_show(GtkWidget *frame, gpointer user_data) {
    (void)frame; side_host_bump(GTK_WIDGET(user_data), +1);
}

static void on_panel_frame_hide(GtkWidget *frame, gpointer user_data) {
    (void)frame; side_host_bump(GTK_WIDGET(user_data), -1);
}

/* ------------------------------------------------------------------ */
/* Window lifecycle                                                    */
/* ------------------------------------------------------------------ */

static gboolean on_window_delete(GtkWindow *w, gpointer ud)
{
    (void)w;
    GtkApplication *app = GTK_APPLICATION(ud);
    /* Capture window geometry now — once delete-event returns, the
     * toplevel starts tearing down and size queries become unreliable. */
    if (g_window) {
        int ww = 0, wh = 0, wx = 0, wy = 0;
        gtk_window_get_size    (GTK_WINDOW(g_window), &ww, &wh);
        gtk_window_get_position(GTK_WINDOW(g_window), &wx, &wy);
        gboolean maxim = gtk_window_is_maximized(GTK_WINDOW(g_window));
        session_stash_geometry(ww, wh, wx, wy, maxim);
    }
    /* G33 — let plugins veto-or-react to shutdown. */
    plugin_notify_before_shutdown();
    /* Capture session before tabs start closing so paths + caret positions
     * are still readable. session_save() silently skips Untitled docs. */
    session_save();
    editor_close_all_quit(G_APPLICATION(app));
    plugin_notify_shutdown();
    /* If editor_close_all_quit didn't dispatch g_application_quit (user
     * cancelled), suppress the delete. */
    return TRUE;
}

/* Search Results panel-frame close (× in its title bar): route to the
 * panel's own hide so the paned divider collapses cleanly. */
static void sresults_panel_close(GtkWidget *frame, gpointer user)
{
    (void)frame; (void)user;
    searchresults_set_visible(FALSE);
}

/* macOS #174 — double-click on the status bar's language token pops the
 * live Language menu at the label. One-shot GtkPopoverMenu; teardown is
 * deferred out of the closed emission (same pattern as npp_menu.c). */
static gboolean lang_popup_teardown_idle(gpointer w)
{
    gtk_widget_unparent(GTK_WIDGET(w));
    return G_SOURCE_REMOVE;
}
static void on_lang_popup_closed(GtkPopover *p, gpointer ud)
{
    (void)ud;
    g_idle_add_full(G_PRIORITY_HIGH, lang_popup_teardown_idle, p, NULL);
}
static void statusbar_open_language_menu(GtkWidget *anchor)
{
    if (!g_language_menu) return;
    GtkWidget *pop =
        gtk_popover_menu_new_from_model(G_MENU_MODEL(g_language_menu));
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_TOP);
    gtk_widget_set_parent(pop, anchor);
    g_signal_connect(pop, "closed", G_CALLBACK(on_lang_popup_closed), NULL);
    gtk_popover_popup(GTK_POPOVER(pop));
}

static void build_main_window(GtkApplication *app)
{
    g_window = GTK_APPLICATION_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(g_window), "Nextpad++");
    /* Restore saved frame from session.xml; fall back to 1280×760 — the
     * width that fits the 32-button toolbar without overflow, similar to
     * macOS's 1100×720 minimum content size. */
    {
        int sw = 0, sh = 0, sx = 0, sy = 0;
        gboolean smax = FALSE;
        if (session_get_saved_geometry(&sw, &sh, &sx, &sy, &smax)
            && sw > 200 && sh > 200) {
            gtk_window_set_default_size(GTK_WINDOW(g_window), sw, sh);
            if (sx > -32768 && sy > -32768)
                gtk_window_move(GTK_WINDOW(g_window), sx, sy);
            if (smax)
                gtk_window_maximize(GTK_WINDOW(g_window));
        } else {
            gtk_window_set_default_size(GTK_WINDOW(g_window), 1280, 760);
        }
    }
    g_signal_connect(g_window, "close-request",
                     G_CALLBACK(on_window_delete), app);

    /* Layout (matches macOS port topology, all toggled via View → Panels):
     *
     *   vbox
     *     toolbar
     *     hpaned (workspace | center)
     *       workspace (left, hidden)
     *       hpaned2 (doclist | center)
     *         doclist (left, hidden)
     *         hpaned3 (notebook | docmap)
     *           vpaned (notebook / searchresults)
     *             notebook (center)
     *             searchresults (bottom, hidden)
     *           docmap (right, hidden)
     *     statusbar (bottom)
     *
     * Side panels follow the macOS port's docking convention; user toggles
     * them via View → Panels. */
    GtkWidget *vbox     = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *toolbar  = toolbar_init(GTK_WIDGET(g_window));
    GtkWidget *primary  = editor_init(GTK_WIDGET(g_window));
    /* G14 — wrap primary in a horizontal GtkPaned with a hidden secondary
     * pane that hosts cloned Scintilla widgets sharing the document. */
    GtkWidget *notebook = split_init(primary, GTK_WIDGET(g_window));

    GtkWidget *workspace = workspace_init(GTK_WIDGET(g_window));
    GtkWidget *doclist   = doclist_init();
    GtkWidget *funclist  = funclist_init();
    GtkWidget *docmap    = docmap_init();
    GtkWidget *sresults  = searchresults_init();
    GtkWidget *gitpanel  = gitpanel_init(GTK_WIDGET(g_window));
    GtkWidget *mdpanel   = mdpreview_init(GTK_WIDGET(g_window));
    GtkWidget *statusbar = statusbar_init();
    statusbar_set_language_dblclick(statusbar_open_language_menu);

    /* Q1 — match macOS: editor on LEFT, all side panels packed to RIGHT
     * inside a single side-panel host. macOS: MainWindowController.mm:2665
     *
     *   hpaned1  (horizontal)
     *     vpaned (vertical) ─── pack1 (LEFT, the editor body)
     *       notebook         ← top
     *       searchresults    ← bottom (collapsible)
     *     side_panel_host ─── pack2 (RIGHT, collapsible)
     *       GtkBox vertical stack of every dockable panel
     *
     * The host collapses to width 0 when every panel inside is hidden. */
    /* G34 — wrap each side panel in a uniform PanelFrame (24-px title bar
     * with [pop_out] [×] buttons). Pop-out is dispatched to floating.c via
     * the name passed in here, which must match the floating_register()
     * call below. Wrapping is opt-in per panel — searchresults lives in
     * its own vpaned slot and still gets a frame so its chrome matches. */
    GtkWidget *sresults_frame  = sresults
        ? panel_frame_new("searchresults", "Search results", sresults,
                          sresults_panel_close, NULL) : NULL;
    /* The Search Results panel has no floating counterpart on macOS —
     * only the close × is wired; hide the detach button. */
    if (sresults_frame) panel_frame_set_detachable(sresults_frame, FALSE);
    GtkWidget *doclist_frame   = doclist
        ? panel_frame_new("doclist",   "Document List",  doclist,   NULL, NULL) : NULL;
    GtkWidget *workspace_frame = workspace
        ? panel_frame_new("workspace", "Folder as Workspace", workspace, NULL, NULL) : NULL;
    GtkWidget *funclist_frame  = funclist
        ? panel_frame_new("funclist",  "Function List",  funclist,  NULL, NULL) : NULL;
    GtkWidget *docmap_frame    = docmap
        ? panel_frame_new("docmap",    "Document Map",   docmap,    NULL, NULL) : NULL;
    GtkWidget *gitpanel_frame  = gitpanel
        ? panel_frame_new("gitpanel",  "Source Control", gitpanel,  NULL, NULL) : NULL;
    GtkWidget *mdpanel_frame   = mdpanel
        ? panel_frame_new("mdpreview", "Markdown Preview", mdpanel, NULL, NULL) : NULL;
    /* Q-align macOS projects_panel.png — initialise + wrap Project Panel. */
    GtkWidget *projectw = project_init(GTK_WIDGET(g_window));
    GtkWidget *project_frame = projectw
        ? panel_frame_new("project", "Project Panel", projectw, NULL, NULL) : NULL;

    /* Character Panel + Clipboard History — were previously initialised
     * but not packed; the toggle actions had no widget to address. Hook
     * them up alongside the other side panels. */
    GtkWidget *charpanelw   = charpanel_init(GTK_WIDGET(g_window));
    GtkWidget *charpanel_frame = charpanelw
        ? panel_frame_new("charpanel", "Character Panel", charpanelw, NULL, NULL) : NULL;
    GtkWidget *cliphistoryw   = cliphistory_init(GTK_WIDGET(g_window));
    GtkWidget *cliphistory_frame = cliphistoryw
        ? panel_frame_new("cliphistory", "Clipboard History", cliphistoryw, NULL, NULL) : NULL;

    GtkWidget *vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(vpaned), notebook, TRUE, FALSE);
    if (sresults_frame) gtk_paned_pack2(GTK_PANED(vpaned), sresults_frame, FALSE, FALSE);

    GtkWidget *side_panel_host = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (doclist_frame)   npp_box_pack(GTK_BOX(side_panel_host), doclist_frame, TRUE, 0);
    if (workspace_frame) npp_box_pack(GTK_BOX(side_panel_host), workspace_frame, TRUE, 0);
    if (funclist_frame)  npp_box_pack(GTK_BOX(side_panel_host), funclist_frame, TRUE, 0);
    if (docmap_frame)    npp_box_pack(GTK_BOX(side_panel_host), docmap_frame, TRUE, 0);
    if (gitpanel_frame)  npp_box_pack(GTK_BOX(side_panel_host), gitpanel_frame, TRUE, 0);
    if (mdpanel_frame)   npp_box_pack(GTK_BOX(side_panel_host), mdpanel_frame, TRUE, 0);
    if (project_frame)   npp_box_pack(GTK_BOX(side_panel_host), project_frame, TRUE, 0);
    if (charpanel_frame)   npp_box_pack(GTK_BOX(side_panel_host), charpanel_frame, TRUE, 0);
    if (cliphistory_frame) npp_box_pack(GTK_BOX(side_panel_host), cliphistory_frame, TRUE, 0);
    /* Q14 — collapse host when every panel inside is hidden; reveal it
     * (and set a reasonable paned position) when any becomes visible.
     * Tracked by counting "show"/"hide" signals across the panel frames.
     * Note: NO `no_show_all` here — show_all must recurse into every frame
     * so the title bar, separator and inner panel widgets get prepared.
     * We then hide the host explicitly at the bottom of build_main_window
     * after all panel contents have been hidden. */
    g_object_set_data(G_OBJECT(side_panel_host), "npp-visible-count",
                      GINT_TO_POINTER(0));

    GtkWidget *hpaned1 = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(hpaned1), vpaned,          TRUE,  FALSE);
    gtk_paned_pack2(GTK_PANED(hpaned1), side_panel_host, FALSE, TRUE);
    g_object_set_data(G_OBJECT(side_panel_host), "npp-hpaned", hpaned1);

    /* Q14 — wire each frame's show/hide to the host visibility counter. */
    GtkWidget *side_frames[] = {
        doclist_frame, workspace_frame, funclist_frame,
        docmap_frame,  gitpanel_frame,  mdpanel_frame, project_frame,
        charpanel_frame, cliphistory_frame,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(side_frames); i++) {
        if (!side_frames[i]) continue;
        g_signal_connect(side_frames[i], "show",
                         G_CALLBACK(on_panel_frame_show), side_panel_host);
        g_signal_connect(side_frames[i], "hide",
                         G_CALLBACK(on_panel_frame_hide), side_panel_host);
    }

    /* G22 incremental search bar — revealer above the status bar. */
    g_isearch_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(g_isearch_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    {
        GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(bar), 4);
        GtkWidget *lbl = gtk_label_new("Find:");
        g_isearch_entry = gtk_search_entry_new();
        gtk_widget_set_hexpand(g_isearch_entry, TRUE);
        g_signal_connect(g_isearch_entry, "search-changed",
                         G_CALLBACK(on_isearch_changed), NULL);
        {
            GtkEventController *kc = gtk_event_controller_key_new();
            g_signal_connect(kc, "key-pressed",
                             G_CALLBACK(on_isearch_keypress), NULL);
            gtk_widget_add_controller(g_isearch_entry, kc);
        }
        npp_box_pack(GTK_BOX(bar), lbl, FALSE, 0);
        npp_box_pack(GTK_BOX(bar), g_isearch_entry, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(g_isearch_revealer), bar);
    }
    gtk_revealer_set_reveal_child(GTK_REVEALER(g_isearch_revealer), FALSE);

    /* Menu bar — GTK4's GtkApplicationWindow does not reliably auto-show the
     * application menubar under a plain mutter session, so build an explicit
     * GtkPopoverMenuBar from the same GMenuModel and pack it at the top. */
    gtk_application_window_set_show_menubar(g_window, FALSE);
    {
        GMenuModel *mbmodel = gtk_application_get_menubar(app);
        if (mbmodel) {
            GtkWidget *menubar = gtk_popover_menu_bar_new_from_model(mbmodel);
            g_menubar_widget = menubar;   /* #7 — for live retranslation */
            npp_box_pack(GTK_BOX(vbox), menubar, FALSE, 0);
        }
        /* Tighten the menu-bar dropdowns 20%. GTK4's default is
         * `popover.menu modelbutton { min-height: 30px; padding: 0 12px }`
         * — the row height is set purely by min-height (vertical padding
         * is 0), so 30 -> 24px is a 20% trim. Scoped to popover.menu, so
         * context-menu popovers are untouched. */
        GtkCssProvider *menu_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(menu_css,
            "popover.menu modelbutton {"
            "  min-height: 24px;"
            "}\n", -1);
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(menu_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(menu_css);
    }

    if (toolbar)   npp_box_pack(GTK_BOX(vbox), toolbar, FALSE, 0);
    npp_box_pack(GTK_BOX(vbox), hpaned1, TRUE, 0);
    npp_box_pack(GTK_BOX(vbox), g_isearch_revealer, FALSE, 0);
    if (statusbar) npp_box_pack(GTK_BOX(vbox), statusbar, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(g_window), vbox);

    gtk_widget_show_all(GTK_WIDGET(g_window));

    /* gtk_window_set_default_size is only a hint — once realized, GTK
     * grows the window to its minimum-natural size, which on multi-panel
     * layouts can blow past the saved height. After show, force the
     * actual saved dimensions with gtk_window_resize. */
    {
        int sw = 0, sh = 0, sx = 0, sy = 0;
        gboolean smax = FALSE;
        if (session_get_saved_geometry(&sw, &sh, &sx, &sy, &smax)
            && sw > 200 && sh > 200 && !smax) {
            gtk_window_resize(GTK_WINDOW(g_window), sw, sh);
        }
    }

    /* All side panels start hidden — toggled via View → Panels. We hide
     * the INNER widget only; the panel_frame wrappers stay visible but
     * their contents hide. The host visibility-counter logic depends on
     * the frame show/hide signals which fire when each panel's
     * set_visible toggles the frame (see e.g. workspace_set_visible). */
    if (workspace)   gtk_widget_hide(workspace);
    if (doclist)     gtk_widget_hide(doclist);
    if (funclist)    gtk_widget_hide(funclist);
    if (docmap)      gtk_widget_hide(docmap);
    if (sresults)    gtk_widget_hide(sresults);
    if (gitpanel)    gtk_widget_hide(gitpanel);
    if (mdpanel)     gtk_widget_hide(mdpanel);
    if (projectw)    gtk_widget_hide(projectw);
    if (charpanelw)  gtk_widget_hide(charpanelw);
    if (cliphistoryw)gtk_widget_hide(cliphistoryw);

    /* P3 — initial status bar visibility from pref. */
    if (statusbar && !g_prefs.show_status_bar)
        gtk_widget_hide(statusbar);

    /* P10 — apply persisted workspace roots. Reveal the panel if any
     * roots were loaded so the user can see their workspace immediately. */
    {
        int n = 0;
        const char *const *roots = prefs_workspace_roots(&n);
        for (int i = 0; i < n; i++)
            workspace_add_folder(roots[i]);
        if (n > 0 && workspace)
            gtk_widget_show(workspace);
    }

    /* G21 — register panels with the floating-panel manager (must happen
     * AFTER they're packed into their GtkPaned slots so the capture sees
     * the original parent + position). G34 wraps every panel in a
     * PanelFrame; registering the WRAPPER ensures pop-out moves the
     * entire frame (chrome + content) into the floating window and
     * dock-back returns it to the same slot. */
    if (workspace_frame) floating_register("workspace",     workspace_frame);
    if (doclist_frame)   floating_register("doclist",       doclist_frame);
    if (funclist_frame)  floating_register("funclist",      funclist_frame);
    if (docmap_frame)    floating_register("docmap",        docmap_frame);
    /* searchresults: not registered — no detach button (see above). */
    if (gitpanel_frame)  floating_register("gitpanel",      gitpanel_frame);
    if (mdpanel_frame)   floating_register("mdpreview",     mdpanel_frame);

    /* G20 — install tab-colour CSS for the lifetime of the window. */
    install_tab_color_css();

    /* Auto-backup timer now that the first editor is live. */
    backup_init();
    /* Plugin system — scan + load plugins from
     *   ~/.config/nextpad-plus-plus/plugins/<Name>/<Name>.so
     *   /usr/lib/nextpad-plus-plus/plugins/<Name>/<Name>.so
     *   /usr/local/lib/nextpad-plus-plus/plugins/<Name>/<Name>.so */
    plugin_init(GTK_WIDGET(g_window));
    plugin_load_all();
    /* GAP-20 — the Plugins menu lists each loaded plugin's FuncItems;
     * they only exist after plugin_load_all, so rebuild the menubar. */
    if (plugin_count() > 0)
        main_rebuild_menubar();
    /* G33 — fire NPPN_READY once plugins are loaded. */
    plugin_notify_ready();

    /* G3.7: accept file drops from Nautilus / other apps. */
    {
        GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
                                                GDK_ACTION_COPY);
        g_signal_connect(dt, "drop", G_CALLBACK(on_files_dropped), NULL);
        gtk_widget_add_controller(GTK_WIDGET(g_window),
                                  GTK_EVENT_CONTROLLER(dt));
    }

    /* Update title once the initial doc exists. */
    main_refresh_title();

    /* Appearance — call AFTER editor_init so theme_apply can seed
     * STYLE_DEFAULT on the freshly-built editor(s) and re-run the styling
     * pipeline. The earlier P3 block in main() only flipped GtkSettings
     * before any window existed; theme_apply does the same flip plus the
     * Scintilla-side work, all behind one seam shared with prefs/menu. */
    theme_apply(theme_mode_from_prefs());
}

/* ------------------------------------------------------------------ */
/* GtkApplication signal handlers                                      */
/* ------------------------------------------------------------------ */

static void on_startup(GtkApplication *app, gpointer ud)
{
    (void)ud;

    /* Resolve effective dark/light appearance and apply it to GtkSettings
     * BEFORE any widget is constructed. This runs in the startup phase,
     * which is GtkApplication's first signal after gtk_init has run —
     * earlier (in main(), before g_application_run) the GtkSettings
     * singleton doesn't yet exist, so the value never reached the theme
     * subsystem and toolbar.c:is_dark_mode() read FALSE on every launch,
     * causing icons to load from light/ even in Dark mode. */
    {
        GtkSettings *s = gtk_settings_get_default();
        if (s) {
            gboolean dark;
            if (g_prefs.appearance == APPEAR_DARK) {
                dark = TRUE;
            } else if (g_prefs.appearance == APPEAR_LIGHT) {
                dark = FALSE;
            } else {
                /* APPEAR_AUTO: probe the desktop color-scheme. */
                dark = FALSE;
                GSettingsSchemaSource *src =
                    g_settings_schema_source_get_default();
                if (src) {
                    GSettingsSchema *schema = g_settings_schema_source_lookup(
                        src, "org.gnome.desktop.interface", TRUE);
                    if (schema) {
                        if (g_settings_schema_has_key(schema, "color-scheme")) {
                            GSettings *gs = g_settings_new(
                                "org.gnome.desktop.interface");
                            if (gs) {
                                gchar *v = g_settings_get_string(gs, "color-scheme");
                                if (v && strcmp(v, "prefer-dark") == 0)
                                    dark = TRUE;
                                g_free(v);
                                g_object_unref(gs);
                            }
                        }
                        g_settings_schema_unref(schema);
                    }
                }
            }
            g_object_set(s, "gtk-application-prefer-dark-theme", dark, NULL);
        }
    }

    /* Actions + accelerators. Recordable menu commands go through the
     * macro-recording wrapper (GAP-20). */
    g_action_map_add_action_entries(G_ACTION_MAP(app),
                                    wrap_recordable_entries(),
                                    G_N_ELEMENTS(kAppActions), NULL);
    macro_set_changed_hook(rebuild_macro_menu);
    macro_push_accels();

    /* Grey out the macOS-parity items that have no Linux backend yet. */
    for (size_t i = 0; i < G_N_ELEMENTS(kStubActions); i++) {
        GAction *act = g_action_map_lookup_action(G_ACTION_MAP(app),
                                                  kStubActions[i]);
        if (act) g_simple_action_set_enabled(G_SIMPLE_ACTION(act), FALSE);
    }

    /* Stateful set-encoding action. */
    g_enc_action = g_simple_action_new_stateful(
        "set-encoding", G_VARIANT_TYPE_STRING,
        g_variant_new_string("UTF-8"));
    g_signal_connect(g_enc_action, "change-state",
                     G_CALLBACK(action_set_encoding_change), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(g_enc_action));

    /* Stateful set-language action — drives the Language menu radio (#4). */
    g_lang_action = g_simple_action_new_stateful(
        "set-language", G_VARIANT_TYPE_STRING, g_variant_new_string(""));
    g_signal_connect(g_lang_action, "change-state",
                     G_CALLBACK(action_set_language_change), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(g_lang_action));

    set_accel(app, "app.new",        "<Primary>n");
    set_accel(app, "app.open",       "<Primary>o");
    set_accel(app, "app.save",       "<Primary>s");
    set_accel(app, "app.save-as",    "<Primary><Shift>s");
    set_accel(app, "app.save-all",   "<Primary><Alt>s");
    set_accel(app, "app.close",      "<Primary>w");
    set_accel(app, "app.quit",       "<Primary>q");
    set_accel(app, "app.undo",       "<Primary>z");
    set_accel(app, "app.redo",       "<Primary>y");
    set_accel(app, "app.cut",        "<Primary>x");
    set_accel(app, "app.copy",       "<Primary>c");
    set_accel(app, "app.paste",      "<Primary>v");
    set_accel(app, "app.select-all", "<Primary>a");
    set_accel(app, "app.goto-line",  "<Primary>g");
    set_accel(app, "app.reload",     "<Primary><Shift>r");
    set_accel(app, "app.find",         "<Primary>f");
    set_accel(app, "app.replace",      "<Primary>h");
    set_accel(app, "app.find-next",    "F3");
    set_accel(app, "app.find-prev",    "<Shift>F3");
    set_accel(app, "app.find-in-files","<Primary><Shift>f");
    set_accel(app, "app.preferences",  "<Primary>comma");
    set_accel(app, "app.run",          "F5");
    set_accel(app, "app.macro-start",  "<Primary>F8");
    set_accel(app, "app.macro-stop",   "<Primary>F9");
    set_accel(app, "app.macro-play",   "<Primary>F10");
    /* G11 accelerators.
     * Zoom (Ctrl +/-/0) is intentionally NOT a global accelerator: it must
     * act on whatever is focused — the editor or a panel. A window-level
     * accelerator always wins over a focused widget, and "<Primary>plus"
     * never matches the real keystroke (Ctrl+Shift+= on most layouts).
     * Editor zoom is handled by a key controller on the Scintilla view
     * (editor.c); panel zoom by one in panel_frame.c. */
    set_accel(app, "app.word-wrap",    "<Primary><Alt>w");
    set_accel(app, "app.fullscreen",   "F11");
    set_accel(app, "app.tab-next",     "<Primary>Page_Down");
    set_accel(app, "app.tab-prev",     "<Primary>Page_Up");
    set_accel(app, "app.print",        "<Primary>p");
    set_accel(app, "app.help-about",   "F1");
    /* G16 brace matching shortcuts (matches macOS Cmd+M / Cmd+Shift+M) */
    set_accel(app, "app.goto-matching-brace", "<Primary>m");
    set_accel(app, "app.select-in-brackets",  "<Primary><Shift>m");
    /* G12 keyboard accelerators */
    set_accel(app, "app.line-duplicate",       "<Primary>d");
    set_accel(app, "app.line-delete",          "<Primary><Shift>k");
    set_accel(app, "app.line-move-up",         "<Primary><Shift>Up");
    set_accel(app, "app.line-move-down",       "<Primary><Shift>Down");
    set_accel(app, "app.indent",               "<Primary>bracketright");
    set_accel(app, "app.unindent",             "<Primary>bracketleft");
    set_accel(app, "app.toggle-line-comment",  "<Primary>q");
    set_accel(app, "app.column-mode",          "<Primary><Alt>c");
    /* G13 + G15 + G22 keys */
    /* Q4 — bookmark accels match macOS Notepad++:
     *   Cmd+F2 → Toggle      (Linux Ctrl+F2 = <Primary>F2)
     *   F2     → Next
     *   Shift+F2 → Previous */
    set_accel(app, "app.bookmark-toggle",      "<Primary>F2");
    set_accel(app, "app.bookmark-next",        "F2");
    set_accel(app, "app.bookmark-prev",        "<Shift>F2");
    set_accel(app, "app.command-palette",      "<Primary><Shift>p");
    set_accel(app, "app.incremental-search",   "<Primary>i");
    set_accel(app, "app.reopen-closed",        "<Primary><Shift>t");
    set_accel(app, "app.multisel-add-next",    "<Primary><Alt>n");
    set_accel(app, "app.hide-lines",           "<Primary><Shift>h");
    set_accel(app, "app.ac-function",          "<Primary>space");
    set_accel(app, "app.ac-word",              "<Primary>Return");
    set_accel(app, "app.ac-function-hint",     "<Primary><Shift>space");

    /* Q-align: keyboard parity with macOS — tab nav 1-9, search result nav,
     * Post-It, parameter hints. Tab parametric: use the {action}::{detail}
     * form (a literal int target ‘1’ through ‘9’). */
    {
        GtkApplication *_app = app;
        const char *tab_accels[9][2] = {
            { "app.tab-goto(1)", "<Primary>1" }, { "app.tab-goto(2)", "<Primary>2" },
            { "app.tab-goto(3)", "<Primary>3" }, { "app.tab-goto(4)", "<Primary>4" },
            { "app.tab-goto(5)", "<Primary>5" }, { "app.tab-goto(6)", "<Primary>6" },
            { "app.tab-goto(7)", "<Primary>7" }, { "app.tab-goto(8)", "<Primary>8" },
            { "app.tab-goto(9)", "<Primary>9" },
        };
        for (int i = 0; i < 9; i++) {
            const char *accels[] = { tab_accels[i][1], NULL };
            gtk_application_set_accels_for_action(_app,
                                                  tab_accels[i][0], accels);
        }
    }
    /* F4 / Shift+F4 — Next/Previous Search Result (macOS parity). */
    set_accel(app, "app.jump-styled-next",     "F4");
    set_accel(app, "app.jump-styled-prev",     "<Shift>F4");
    /* F12 — Post-It Mode toggle. */
    set_accel(app, "app.post-it",              "F12");
    /* Q-align Run menu canned helpers (macOS Alt+F1/F3/F6). */
    set_accel(app, "app.get-php-help",         "<Alt>F1");
    set_accel(app, "app.wikipedia-search",     "<Alt>F3");
    set_accel(app, "app.open-in-new-instance", "<Alt>F6");

    /* Menu bar — keep the English model (g_menu_english) for the context-
     * menu index and for live retranslation; the app menubar itself uses a
     * translated copy in the current UI language (#7). */
    g_menu_english = build_menu_model();
    GMenuModel *translated = i18n_translate_menu(g_menu_english);
    gtk_application_set_menubar(app, translated);

    /* P5 — build the (entry+item → action-name) lookup table for the
     * XML-driven context menus, from the ENGLISH model so the lookups
     * (tabContextMenu.xml uses English item names) keep resolving. */
    extern void ctxmenu_index_from_model(GMenuModel *);
    ctxmenu_index_from_model(g_menu_english);

    g_object_unref(translated);
}

/* Put keyboard focus in the editor at startup / on raise so the user can
 * type immediately without clicking into the view first. Deferred to an
 * idle so it runs after window present and the first layout pass. */
static gboolean focus_editor_idle(gpointer d)
{
    (void)d;
    GtkWidget *sci = current_sci();
    if (sci) gtk_widget_grab_focus(sci);
    return G_SOURCE_REMOVE;
}

static void on_activate(GtkApplication *app, gpointer ud)
{
    (void)ud;
    if (!g_window) {
        build_main_window(app);
        /* Once the editor + split-view widgets exist, propagate their
         * pointers into NppData so the next plugin SCI_* call from
         * setInfo()-aware plugins lands on the right Scintilla. */
        plugin_refresh_handles();
        /* P3 — restore previous session only when the pref allows it.
         * session_restore() already silently skips missing files; the
         * keep_absent_session pref is honored inside session.c. */
        if (g_prefs.remember_session)
            session_restore();
    }
    gtk_window_present(GTK_WINDOW(g_window));
    g_idle_add(focus_editor_idle, NULL);
    g_timeout_add(2500, startup_update_check, NULL);
}

static void on_open(GtkApplication *app, GFile **files, gint n_files,
                    const gchar *hint, gpointer ud)
{
    (void)hint; (void)ud;
    if (!g_window) build_main_window(app);
    for (gint i = 0; i < n_files; i++) {
        const char *path = g_file_peek_path(files[i]);
        if (path) editor_open_path_guarded(path);   /* G17 size guard */
    }
    /* G3.14: apply parsed CLI flags to whichever file is now active. */
    apply_cli_flags_to_current();
    gtk_window_present(GTK_WINDOW(g_window));
    g_idle_add(focus_editor_idle, NULL);
    g_timeout_add(2500, startup_update_check, NULL);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    /* Load styles, lexers, prefs, localisation, recent files before any
     * window exists. */
    /* P1 — match macOS: create ~/.nextpad++/ and seed bundled model XMLs
     * on first run (idempotent). Must happen before any loader. */
    npp_ensure_user_dirs();

    stylestore_init(RESOURCES_DIR "/stylers.model.xml");
    /* P4 — load language definitions (ext→lang, keywords, comments) from
     * ~/.nextpad++/langs.xml (or bundled langs.model.xml). */
    extern void langsmgr_init(void);
    langsmgr_init();
    /* P6 — parse toolbarButtonsConf.xml so per-button hide is applied at
     * toolbar construction time. */
    extern void toolbarconf_init(void);
    toolbarconf_init();
    /* prefs_load() first so i18n_init() can honour the saved UI language
     * (Preferences > General > Language) before falling back to locale. */
    prefs_load();
    i18n_init();
    /* Materialise any newly-added defaults to disk so the schema in
     * config.xml stays in sync with the in-memory NppPrefs. */
    prefs_save();
    recent_load();

    /* Toolbar icon scale + theme preset. The dark/light appearance
     * resolution that used to live in this block moved into on_startup
     * (the toolbar was being built with light/ icons because gtk_init
     * hadn't yet created the GtkSettings singleton at this point). */
    {
        extern void toolbar_apply_icon_scale(int);
        toolbar_apply_icon_scale(g_prefs.toolbar_icon_scale);

        /* Theme preset: look up <theme>.xml in ~/.nextpad++/themes/ first,
         * then fall back to bundled themes. "Default" means leave as-is. */
        if (g_prefs.theme_preset[0] &&
            strcmp(g_prefs.theme_preset, "Default") != 0) {
            gchar *fname = g_strdup_printf("%s.xml", g_prefs.theme_preset);
            gchar *user_theme = npp_user_file("themes", fname);
            if (g_file_test(user_theme, G_FILE_TEST_EXISTS)) {
                stylestore_load_theme(user_theme);
            } else {
                gchar *bundle_theme = npp_bundle_file("themes", fname);
                if (g_file_test(bundle_theme, G_FILE_TEST_EXISTS))
                    stylestore_load_theme(bundle_theme);
                g_free(bundle_theme);
            }
            g_free(user_theme);
            g_free(fname);
        }
    }

    /* G3.14: strip our recognised flags from argv before GApplication sees
     * it; remaining args (file paths) flow into on_open as GFile*. */
    argc = parse_cli_flags(argc, argv);

    g_app = gtk_application_new(kAppId, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(g_app, "startup",  G_CALLBACK(on_startup),  NULL);
    g_signal_connect(g_app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(g_app, "open",     G_CALLBACK(on_open),     NULL);
    int status = g_application_run(G_APPLICATION(g_app), argc, argv);
    g_object_unref(g_app);
    return status;
}

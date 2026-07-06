#ifndef MACRO_H
#define MACRO_H

#include <gtk/gtk.h>
#include "sci_c.h"

/* ------------------------------------------------------------------
 * Macro step — Windows shortcuts.xml <Action> format (GAP-18/19).
 *   type 0  mtUseLParameter  Scintilla message, numeric lParam
 *   type 1  mtUseSParameter  Scintilla message, string lParam (sparam)
 *   type 2  mtMenuCommand    menu command: Windows IDM_* in wp, or a
 *                            selector/action name in sparam
 *                            ("pluginMenuAction:" + wp = plugin cmdID)
 *   type 3  mtSavedSnR       Find/Replace pseudo-message sequence
 * ------------------------------------------------------------------ */
typedef struct {
    int          type;
    unsigned int msg;
    gint64       wp;
    gint64       lp;
    char        *sparam;   /* owned; NULL when unused */
} MacroStep;

typedef struct {
    char     *name;
    char     *folder;      /* FolderName attribute (round-tripped) or NULL */
    gboolean  ctrl, alt, shift, super_mod;
    guint     key;         /* Windows-VK code; 0 = unbound */
    GArray   *steps;       /* of MacroStep */
} NamedMacro;

/* Start/stop Scintilla macro recording. */
void macro_start_recording(GtkWidget *sci);
void macro_stop_recording(GtkWidget *sci);

/* Called from SCN_MACRORECORD: store one recorded step. */
void macro_on_record(unsigned int msg, uptr_t wp, sptr_t lp);

/* Menu-command recording (GAP-20). Wrap a recordable menu action:
 *   if (macro_menu_wrap_begin()) { run(); macro_menu_wrap_end(name, 0); }
 * begin returns FALSE (and records nothing) unless recording is active.
 * plugin_cmd_id > 0 records the macOS/Windows-interoperable
 * "pluginMenuAction:" form instead of the action name. */
gboolean macro_menu_wrap_begin(void);
void     macro_menu_wrap_end(const char *linux_action, int plugin_cmd_id);

/* Play back the current recorded macro once. */
void macro_playback(GtkWidget *sci);

/* Replay an arbitrary step list (GAP-18: all four action types). */
void macro_play_steps(GtkWidget *sci, const MacroStep *steps, int n);

/* "Run a Macro Multiple Times…" dialog — N times or until EOF (GAP-22). */
void macro_run_multiple_dialog(GtkWidget *sci, GtkWindow *parent);

/* Until-EOF loop (Windows line-delta termination), sans dialog/progress —
 * returns the number of iterations run. Testable. */
int macro_run_until_eof(GtkWidget *sci, const MacroStep *steps, int n);

/* State queries */
gboolean macro_is_recording(void);
gboolean macro_has_macro(void);
gboolean macro_is_playing(void);

/* Current recorded macro (for the batch runner / run-multiple dialog). */
const MacroStep *macro_current_steps(int *n);

/* Named macro management */
void macro_save_as_dialog(GtkWidget *sci, GtkWindow *parent);
void macro_manage_dialog(GtkWidget *sci, GtkWindow *parent);
void macro_trim_and_save(GtkWidget *sci);

int               macro_named_count(void);
const char       *macro_named_at(int i);
const NamedMacro *macro_named_get(int i);
void              macro_play_named(int i);
void              macro_named_delete(int i);
void              macro_named_set_shortcut(int i, gboolean ctrl, gboolean alt,
                                           gboolean shift, gboolean super_mod,
                                           guint key);

/* Persist named macros into the <Macros> section of shortcuts.xml,
 * preserving every other section of the file (GAP-19). */
void macro_save_to_shortcuts_xml(void);

/* Emit the <Macros>…</Macros> section body (with real Action children) —
 * used by shortcutmap.c when it rewrites the whole shortcuts.xml. */
void macro_emit_macros_section(GString *out);

/* Push accelerators for bound named macros onto app.macro-play-named(i). */
void macro_push_accels(void);

/* main.c registers a hook to refresh the Macro menu after save/delete. */
void macro_set_changed_hook(void (*fn)(void));

/* Pure text splice: replace (or insert) the <Macros> section inside a
 * shortcuts.xml document. xml may be NULL/empty → minimal skeleton.
 * Comment-aware. Returns newly-allocated full file text. Testable. */
char *macro_xml_splice_macros(const char *xml, const char *section);

#endif /* MACRO_H */

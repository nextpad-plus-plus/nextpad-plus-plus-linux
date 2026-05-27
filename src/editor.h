#ifndef EDITOR_H
#define EDITOR_H

#include <gtk/gtk.h>
#include "sci_c.h"

typedef struct {
    GtkWidget    *sci;
    char         *filepath;           /* NULL = unsaved */
    char         *encoding;           /* e.g. "UTF-8", "ISO-8859-1" — does NOT
                                       * include BOM marker; see has_bom field */
    gboolean      has_bom;            /* file carries a UTF byte-order mark on
                                       * disk (matches macOS hasBOM in
                                       * session.plist tab dict). */
    gboolean      user_readonly;      /* user-toggled Read-Only flag (Edit menu).
                                       * Separate from file-system read-only —
                                       * macOS calls this userReadOnly. */
    char         *language;           /* current language name (e.g. "cpp"),
                                       * separate from filepath-derived
                                       * heuristic so Language menu override
                                       * survives session round-trip. */
    char         *backup_filepath;    /* path of the snapshot under
                                       * ~/.nextpad++/backup/ for crash
                                       * recovery — matches macOS
                                       * backupFilePath in session.plist. */
    gboolean      modified;
    int           new_index;          /* "new N" label when filepath==NULL */
    gboolean      word_wrap;          /* per-tab word wrap state */
    GFileMonitor *file_monitor;       /* watches filepath for external changes */
    gboolean      ignore_next_change; /* suppress the event caused by our own save */
    gboolean      monitoring;         /* tail-f auto-reload on every external change */
    int           color_tag;          /* 0 = none, 1..5 = Red/Orange/Yellow/Green/Blue.
                                       * Matches macOS DocumentListPanel color column
                                       * used to visually flag related docs. */
    gboolean      pinned;             /* user-pinned tab — sticks at the top of the
                                       * doc list and survives Close All but This. */
} NppDoc;

/* Initialise — call once, returns the GtkNotebook to embed in the window */
GtkWidget *editor_init(GtkWidget *window);

/* Document access */
NppDoc    *editor_current_doc(void);
NppDoc    *editor_doc_at(int page);
int        editor_page_count(void);
int        editor_current_page(void);
GtkWidget *editor_get_notebook(void);

/* File operations (dialogs shown when path is NULL / as appropriate) */
void       editor_new_doc(void);
gboolean   editor_open_dialog(void);               /* shows GTK open dialog */
gboolean   editor_open_path(const char *path);     /* open a specific file   */
gboolean   editor_save(void);                      /* save current doc       */
gboolean   editor_save_at(int page);               /* save specific page     */
gboolean   editor_save_all(void);                  /* save all modified docs  */
gboolean   editor_save_as_dialog(void);            /* shows GTK save dialog  */
gboolean   editor_save_copy_as(void);             /* save copy to new path, keep current */
gboolean   editor_rename(void);                   /* rename current file in-place */
void       editor_reload_current(void);            /* reload current doc from disk */
void       editor_reload_as(const char *encoding); /* re-read with forced encoding */
gboolean   editor_close_page(int page);            /* -1 = current           */
gboolean   editor_close_sci(GtkWidget *sci);        /* close one exact tab     */
gboolean   editor_close_all_but_current(void);
void       editor_close_all_quit(GApplication *app);

/* Split views (#3) — move/clone the focused editor to the secondary
 * vertical (right) or horizontal (bottom) view; Reset View collapses
 * both secondaries back into the primary. */
void       editor_move_to_view(gboolean vertical);
void       editor_clone_to_view(gboolean vertical);
void       editor_reset_view(void);
gboolean   editor_split_active(void);

/* Synchronised scrolling between the primary and secondary split view. */
void       editor_set_sync_scroll(gboolean vertical, gboolean enable);

/* Reload every tab's save-state / pin / close icons after a light/dark
 * appearance switch. */
void       editor_refresh_tab_chrome(void);

/* Tab pinning — NppDoc.pinned is the single source of truth, shared with
 * the Document List. A pinned tab hides its × and blocks close. */
gboolean   editor_tab_pinned(GtkWidget *sci);
void       editor_set_tab_pinned(GtkWidget *sci, gboolean pinned);

/* Tab colour — NppDoc.color_tag (0 = none, 1..5), shared with the
 * Document List. Draws a 3px top stripe on the tab (macOS NppTabBar). */
gint       editor_tab_color(GtkWidget *sci);
void       editor_set_tab_color(GtkWidget *sci, int slot);
void       editor_apply_tab_color(GtkWidget *sci);

/* Set img's contents to the same toolbar floppy icon the tab strip uses
 * (resources/icons/{light|dark}/toolbar/regular/save_off{,_red}.png).
 * `modified` toggles between the saved and red-modified variants.
 * `pixel_size` lets the caller pick the rendered size (tabs use 11 px;
 * the doc-list panel uses 14 px to match macOS NSTableView rows). */
void       editor_apply_save_status_icon(GtkWidget *img, gboolean modified,
                                         int pixel_size);

/* Hex string ("#RRGGBB") for tab colour slot 1..5; NULL for 0/out-of-range.
 * Single source of truth for the palette — main.c's tab-stripe CSS and
 * the colour-swatch menu labels both read from here. */
const char *editor_tab_color_hex(int slot);

/* Pango markup label for a "Apply Color N"-style menu item:
 *   <span color='#FCE386'>■</span>  Apply Color 1
 * Caller frees the returned string with g_free. Slots outside 1..5
 * return a plain g_strdup(label) with no swatch glyph. The label
 * argument is XML-escaped, so user-translated strings are safe. */
char       *editor_tab_color_markup_label(int slot, const char *label);

/* Edit operations on current document */
void editor_undo(void);
void editor_redo(void);
void editor_cut(void);
void editor_copy(void);
void editor_paste(void);
void editor_select_all(void);
void editor_goto_line_dialog(void);

/* Re-apply current theme styles to all open editors. */
void editor_reapply_styles(void);

/* Re-apply g_prefs to all open editors (call after changing preferences). */
void editor_apply_prefs(void);

/* Implemented in main.c, called from editor.c / toolbar.c */
void main_toggle_bookmark_at_line(GtkWidget *sci, int line);
void main_recent_file_add(const char *path);
void main_sync_encoding_menu(const char *enc);
void main_apply_view_symbols(GtkWidget *sci); /* apply margin widths to one sci */
void main_doclist_refresh(void);              /* rebuild Document List panel */
void main_do_print(void);                     /* show system print dialog */

/* Open a file (or switch to it if already open) then jump to a 1-based line. */
void   editor_open_and_goto(const char *path, int line);

/* Incremental search bar (Ctrl+I) */
void editor_incr_search_show(void);
void editor_incr_search_close(void);

/* Convenience send to current doc */
sptr_t editor_send(unsigned int msg, uptr_t wp, sptr_t lp);

#endif /* EDITOR_H */

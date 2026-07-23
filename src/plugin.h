#pragma once
#include <gtk/gtk.h>

/* ------------------------------------------------------------------
 * Types shared between host and plugin .so files.
 * Plugin authors should include this header when writing a plugin.
 * ------------------------------------------------------------------ */

/** One menu entry contributed by a plugin. */
typedef struct {
    char   itemName[64];   /* display label (UTF-8); "-" = separator  */
    void (*pFunc)(void);   /* called on menu-item activation           */
    int    cmdID;          /* unique ID assigned by host at load time  */
    int    init2Check;     /* non-zero → item starts with a checkmark  */
} FuncItem;

/** Callback type plugins use to query the host. */
typedef long (*NppHostMsg)(unsigned int msg, unsigned long wParam, long lParam);

/** Passed to plugins that export setInfo(NppData). */
typedef struct {
    GtkWidget  *nppHandle;             /* main application window   */
    GtkWidget  *scintillaMainHandle;   /* primary Scintilla widget  */
    GtkWidget  *scintillaSecondHandle; /* secondary sci (or NULL)   */
    NppHostMsg  hostMsg;               /* send a message to host    */
} NppData;

/* ------------------------------------------------------------------
 * NPPM host-message IDs — canonical Notepad++ layout
 *
 * Renumbered 2026-05-14 to match notepad-plus-plus-macos/src/
 * NppPluginInterfaceMac.h verbatim. The pre-renumber layout used
 * ad-hoc offsets that produced collisions (e.g. SAVECURRENTFILE+38
 * collided with the locally-invented GETFULLCURRENTPATH+38).
 *
 * Two bases:
 *   NPPM_BASE         = WM_USER + 1000  → most messages (NPPMSG family)
 *   NPPM_RUNCMD_BASE  = WM_USER + 3000  → path/word/line queries
 *
 * Plugins binding against this header must be rebuilt — IDs CHANGED.
 * That's acceptable per project decision: plugins are re-compiled
 * per platform anyway (ABI differs from Windows .dll).
 * ------------------------------------------------------------------ */
#define NPPM_BASE                  (0x0400 + 1000)
#define NPPM_RUNCMD_BASE           (0x0400 + 3000)

/* ── NPPMSG range (canonical offsets) ─────────────────────────────────── */
#define NPPM_GETCURRENTSCINTILLA              (NPPM_BASE + 4)
#define NPPM_GETCURRENTLANGTYPE               (NPPM_BASE + 5)
#define NPPM_SETCURRENTLANGTYPE               (NPPM_BASE + 6)
#define NPPM_GETNBOPENFILES                   (NPPM_BASE + 7)
#define NPPM_GETOPENFILENAMES                 (NPPM_BASE + 8)
#define NPPM_MODELESSDIALOG                   (NPPM_BASE + 12)
#define NPPM_GETNBSESSIONFILES                (NPPM_BASE + 13)
#define NPPM_GETSESSIONFILES                  (NPPM_BASE + 14)
#define NPPM_SAVESESSION                      (NPPM_BASE + 15)
#define NPPM_SAVECURRENTSESSION               (NPPM_BASE + 16)
#define NPPM_GETOPENFILENAMESPRIMARY          (NPPM_BASE + 17)
#define NPPM_GETOPENFILENAMESSECOND           (NPPM_BASE + 18)
#define NPPM_CREATESCINTILLAHANDLE            (NPPM_BASE + 20)
#define NPPM_DESTROYSCINTILLAHANDLE           (NPPM_BASE + 21)
#define NPPM_GETNBUSERLANG                    (NPPM_BASE + 22)
#define NPPM_GETCURRENTDOCINDEX               (NPPM_BASE + 23)
#define NPPM_SETSTATUSBAR                     (NPPM_BASE + 24)
#define NPPM_GETMENUHANDLE                    (NPPM_BASE + 25)
#define NPPM_ENCODESCI                        (NPPM_BASE + 26)
#define NPPM_DECODESCI                        (NPPM_BASE + 27)
#define NPPM_ACTIVATEDOC                      (NPPM_BASE + 28)
#define NPPM_LAUNCHFINDINFILESDLG             (NPPM_BASE + 29)
#define NPPM_DMMSHOW                          (NPPM_BASE + 30)
#define NPPM_DMMHIDE                          (NPPM_BASE + 31)
#define NPPM_DMMUPDATEDISPINFO                (NPPM_BASE + 32)
#define NPPM_DMMREGASDCKDLG                   (NPPM_BASE + 33)
#define NPPM_LOADSESSION                      (NPPM_BASE + 34)
#define NPPM_DMMVIEWOTHERTAB                  (NPPM_BASE + 35)
#define NPPM_RELOADFILE                       (NPPM_BASE + 36)
#define NPPM_SWITCHTOFILE                     (NPPM_BASE + 37)
#define NPPM_SAVECURRENTFILE                  (NPPM_BASE + 38)
#define NPPM_SAVEALLFILES                     (NPPM_BASE + 39)
#define NPPM_SETMENUITEMCHECK                 (NPPM_BASE + 40)
#define NPPM_ADDTOOLBARICON                   (NPPM_BASE + 41)
#define NPPM_GETWINDOWSVERSION                (NPPM_BASE + 42)
#define NPPM_DMMGETPLUGINHWNDBYNAME           (NPPM_BASE + 43)
#define NPPM_MAKECURRENTBUFFERDIRTY           (NPPM_BASE + 44)
#define NPPM_GETPLUGINSCONFIGDIR              (NPPM_BASE + 46)
#define NPPM_MSGTOPLUGIN                      (NPPM_BASE + 47)
#define NPPM_MENUCOMMAND                      (NPPM_BASE + 48)
#define NPPM_TRIGGERTABBARCONTEXTMENU         (NPPM_BASE + 49)
#define NPPM_GETNPPVERSION                    (NPPM_BASE + 50)
#define NPPM_HIDETABBAR                       (NPPM_BASE + 51)
#define NPPM_ISTABBARHIDDEN                   (NPPM_BASE + 52)
#define NPPM_GETPOSFROMBUFFERID               (NPPM_BASE + 57)
#define NPPM_GETFULLPATHFROMBUFFERID          (NPPM_BASE + 58)
#define NPPM_GETBUFFERIDFROMPOS               (NPPM_BASE + 59)
#define NPPM_GETCURRENTBUFFERID               (NPPM_BASE + 60)
#define NPPM_RELOADBUFFERID                   (NPPM_BASE + 61)
#define NPPM_GETBUFFERLANGTYPE                (NPPM_BASE + 64)
#define NPPM_SETBUFFERLANGTYPE                (NPPM_BASE + 65)
#define NPPM_GETBUFFERENCODING                (NPPM_BASE + 66)
#define NPPM_SETBUFFERENCODING                (NPPM_BASE + 67)
#define NPPM_GETBUFFERFORMAT                  (NPPM_BASE + 68)
#define NPPM_SETBUFFERFORMAT                  (NPPM_BASE + 69)
#define NPPM_HIDETOOLBAR                      (NPPM_BASE + 70)
#define NPPM_ISTOOLBARHIDDEN                  (NPPM_BASE + 71)
#define NPPM_HIDEMENU                         (NPPM_BASE + 72)
#define NPPM_ISMENUHIDDEN                     (NPPM_BASE + 73)
#define NPPM_HIDESTATUSBAR                    (NPPM_BASE + 74)
#define NPPM_ISSTATUSBARHIDDEN                (NPPM_BASE + 75)
#define NPPM_GETSHORTCUTBYCMDID               (NPPM_BASE + 76)
#define NPPM_DOOPEN                           (NPPM_BASE + 77)
#define NPPM_SAVECURRENTFILEAS                (NPPM_BASE + 78)
#define NPPM_GETCURRENTNATIVELANGENCODING     (NPPM_BASE + 79)
#define NPPM_ALLOCATESUPPORTED                (NPPM_BASE + 80)
#define NPPM_ALLOCATECMDID                    (NPPM_BASE + 81)
#define NPPM_ALLOCATEMARKER                   (NPPM_BASE + 82)
#define NPPM_GETLANGUAGENAME                  (NPPM_BASE + 83)
#define NPPM_GETLANGUAGEDESC                  (NPPM_BASE + 84)
#define NPPM_SHOWDOCLIST                      (NPPM_BASE + 85)
#define NPPM_ISDOCLISTSHOWN                   (NPPM_BASE + 86)
#define NPPM_GETAPPDATAPLUGINSALLOWED         (NPPM_BASE + 87)
#define NPPM_GETCURRENTVIEW                   (NPPM_BASE + 88)
#define NPPM_DOCLISTDISABLEEXTCOLUMN          (NPPM_BASE + 89)
#define NPPM_GETEDITORDEFAULTFOREGROUNDCOLOR  (NPPM_BASE + 90)
#define NPPM_GETEDITORDEFAULTBACKGROUNDCOLOR  (NPPM_BASE + 91)
#define NPPM_SETSMOOTHFONT                    (NPPM_BASE + 92)
#define NPPM_SETEDITORBORDEREDGE              (NPPM_BASE + 93)
#define NPPM_SAVEFILE                         (NPPM_BASE + 94)
#define NPPM_DISABLEAUTOUPDATE                (NPPM_BASE + 95)
#define NPPM_REMOVESHORTCUTBYCMDID            (NPPM_BASE + 96)
#define NPPM_GETPLUGINHOMEPATH                (NPPM_BASE + 97)
#define NPPM_GETSETTINGSONCLOUDPATH           (NPPM_BASE + 98)
#define NPPM_SETLINENUMBERWIDTHMODE           (NPPM_BASE + 99)
#define NPPM_GETLINENUMBERWIDTHMODE           (NPPM_BASE + 100)
#define NPPM_ADDTOOLBARICON_FORDARKMODE       (NPPM_BASE + 101)
#define NPPM_DOCLISTDISABLEPATHCOLUMN         (NPPM_BASE + 102)
#define NPPM_GETEXTERNALLEXERAUTOINDENTMODE   (NPPM_BASE + 103)
#define NPPM_SETEXTERNALLEXERAUTOINDENTMODE   (NPPM_BASE + 104)
#define NPPM_ISAUTOINDENTON                   (NPPM_BASE + 105)
#define NPPM_GETCURRENTMACROSTATUS            (NPPM_BASE + 106)
#define NPPM_ISDARKMODEENABLED                (NPPM_BASE + 107)
#define NPPM_GETDARKMODECOLORS                (NPPM_BASE + 108)
#define NPPM_GETCURRENTCMDLINE                (NPPM_BASE + 109)
#define NPPM_CREATELEXER                      (NPPM_BASE + 110)
#define NPPM_GETBOOKMARKID                    (NPPM_BASE + 111)
#define NPPM_DARKMODESUBCLASSANDTHEME         (NPPM_BASE + 112)
#define NPPM_ALLOCATEINDICATOR                (NPPM_BASE + 113)
#define NPPM_GETTABCOLORID                    (NPPM_BASE + 114)
#define NPPM_SETUNTITLEDNAME                  (NPPM_BASE + 115)
#define NPPM_GETNATIVELANGFILENAME            (NPPM_BASE + 116)
#define NPPM_ADDSCNMODIFIEDFLAGS              (NPPM_BASE + 117)
#define NPPM_GETTOOLBARICONSETCHOICE          (NPPM_BASE + 118)
#define NPPM_GETNPPSETTINGSDIRPATH            (NPPM_BASE + 119)
#define NPPM_SETPLUGINSUBSCRIPTIONS           (NPPM_BASE + 500)
#define NPPM_DMM_REGISTERPANEL                (NPPM_BASE + 501)
#define NPPM_DMM_SHOWPANEL                    (NPPM_BASE + 502)
#define NPPM_DMM_HIDEPANEL                    (NPPM_BASE + 503)
#define NPPM_DMM_UNREGISTERPANEL              (NPPM_BASE + 504)
/* GAP-81 (macOS 7d74dcb/167d794) — OPTIONAL restore metadata for a
 * registered panel, the analogue of Windows tTbData's pszModuleName +
 * dlgID. A panel that declares this and is open when Nextpad++ quits is
 * re-opened at the next launch (subject to the "Remember panel
 * visibility" preference). Never declared → never restored (pre-1.1.0
 * behaviour).
 *   wParam — handle returned from NPPM_DMM_REGISTERPANEL.
 *   lParam — const NppPanelInfo * (read only for the duration of the
 *            call; may live on the caller's stack).
 * Restore ladder at the next launch (after NPPN_READY + 500 ms, so a
 * plugin that restores its own panel in READY wins and the host no-ops):
 *   1. panel already visible → nothing;
 *   2. otherwise the host INVOKES YOUR MENU COMMAND at cmdIndex — even
 *      when the panel is registered-but-hidden, so the open runs through
 *      your code path and your internal state stays consistent. That
 *      command must therefore be a plain open/toggle with no other side
 *      effects (do NOT point it at a command that starts real work);
 *   3. a direct host-side show is the last resort, only when the
 *      declared command can no longer be resolved.
 * The command is re-resolved by NAME first (captured at declare time —
 * survives FuncItem reordering across plugin versions), index fallback.
 * Returns 1 on success, 0 for an invalid handle — and 0 on hosts older
 * than 1.1.0; ignore the result and the plugin stays compatible. */
#define NPPM_DMM_SETPANELINFO                 (NPPM_BASE + 505)

typedef struct NppPanelInfo {
    const char *moduleName;   /* plugin folder / getName() name, e.g. "NotifySpy" */
    int         cmdIndex;     /* index into your FuncItem array of the pure
                               * open/toggle command for this panel (dlgID) */
} NppPanelInfo;

/* GAP-88e — NPPM_MSGTOPLUGIN inter-plugin messaging (Windows canon):
 * wParam = const char * destination module name (getName()), lParam =
 * CommunicationInfo *. Delivered synchronously to the destination's
 * messageProc(NPPM_MSGTOPLUGIN, 0, (long)ci); its return value is
 * relayed back. Returns 0 when the named plugin is not loaded. */
struct CommunicationInfo {
    long        internalMsg;
    const char *srcModuleName;   /* UTF-8 */
    void       *info;
};

/* GAP-88c — NPPM_SETPLUGINSUBSCRIPTIONS (macOS-parity message):
 * wParam = bitmask of NPPPLUGIN_WANTS_* flags, lParam = const char *
 * plugin module name (getName()). Both flags default ON; a plugin that
 * doesn't need SCN_UPDATEUI / SCN_PAINTED opts out to skip the fan-out.
 * Returns 1 on success, 0 if the named plugin is not loaded. */
#define NPPPLUGIN_WANTS_UPDATEUI        (1u << 0)
#define NPPPLUGIN_WANTS_PAINTED         (1u << 1)
#define NPPPLUGIN_DEFAULT_SUBSCRIPTIONS (NPPPLUGIN_WANTS_UPDATEUI | \
                                         NPPPLUGIN_WANTS_PAINTED)

/* ── RUNCOMMAND_USER range (path / word / line queries) ──────────────── */
#define NPPM_GETFULLCURRENTPATH               (NPPM_RUNCMD_BASE + 1)
#define NPPM_GETCURRENTDIRECTORY              (NPPM_RUNCMD_BASE + 2)
#define NPPM_GETFILENAME                      (NPPM_RUNCMD_BASE + 3)
#define NPPM_GETNAMEPART                      (NPPM_RUNCMD_BASE + 4)
#define NPPM_GETEXTPART                       (NPPM_RUNCMD_BASE + 5)
#define NPPM_GETCURRENTWORD                   (NPPM_RUNCMD_BASE + 6)
#define NPPM_GETNPPDIRECTORY                  (NPPM_RUNCMD_BASE + 7)
#define NPPM_GETCURRENTLINE                   (NPPM_RUNCMD_BASE + 8)
#define NPPM_GETCURRENTCOLUMN                 (NPPM_RUNCMD_BASE + 9)
#define NPPM_GETNPPFULLFILEPATH               (NPPM_RUNCMD_BASE + 10)
#define NPPM_GETFILENAMEATCURSOR              (NPPM_RUNCMD_BASE + 11)
#define NPPM_GETCURRENTLINESTR                (NPPM_RUNCMD_BASE + 12)

/* ── Linux-specific extensions (no canonical home) ───────────────────── */
#define NPPM_GETDIRECTORYPATH                 (NPPM_BASE + 600)
#define NPPM_RUNMENUCOMMAND                   (NPPM_BASE + 601)
#define NPPM_SHOWDOCSWITCHER                  (NPPM_BASE + 602)
#define NPPM_ISDOCSWITCHERSHOWN               (NPPM_BASE + 603)
#define NPPM_SAVECURRENTSESSIONAS             (NPPM_BASE + 604)

/* ------------------------------------------------------------------
 * NPPN_* plugin notification codes — sent from host to plugins via
 * the beNotified(SCNotification *) entry point. SCNotification.nmhdr.code
 * carries the NPPN_* value; SCNotification.nmhdr.idFrom carries the buffer
 * id (a NppDoc * cast to uintptr_t on Linux).
 * ------------------------------------------------------------------ */
#define NPPN_FIRST                       1000
#define NPPN_READY                       (NPPN_FIRST + 1)
#define NPPN_TBMODIFICATION              (NPPN_FIRST + 2)
#define NPPN_FILEBEFORECLOSE             (NPPN_FIRST + 3)
#define NPPN_FILEOPENED                  (NPPN_FIRST + 4)
#define NPPN_FILECLOSED                  (NPPN_FIRST + 5)
#define NPPN_FILEBEFOREOPEN              (NPPN_FIRST + 6)
#define NPPN_FILEBEFORESAVE              (NPPN_FIRST + 7)
#define NPPN_FILESAVED                   (NPPN_FIRST + 8)
#define NPPN_SHUTDOWN                    (NPPN_FIRST + 9)
#define NPPN_BUFFERACTIVATED             (NPPN_FIRST + 10)
#define NPPN_LANGCHANGED                 (NPPN_FIRST + 11)
#define NPPN_WORDSTYLESUPDATED           (NPPN_FIRST + 12)
#define NPPN_SHORTCUTREMAPPED            (NPPN_FIRST + 13)
#define NPPN_FILEBEFORELOAD              (NPPN_FIRST + 14)
#define NPPN_FILELOADFAILED              (NPPN_FIRST + 15)
#define NPPN_READONLYCHANGED             (NPPN_FIRST + 16)
#define NPPN_DOCORDERCHANGED             (NPPN_FIRST + 17)
#define NPPN_SNAPSHOTDIRTYFILELOADED     (NPPN_FIRST + 18)
#define NPPN_BEFORESHUTDOWN              (NPPN_FIRST + 19)
#define NPPN_CANCELSHUTDOWN              (NPPN_FIRST + 20)
#define NPPN_FILEBEFORERENAME            (NPPN_FIRST + 21)
#define NPPN_FILERENAMECANCEL            (NPPN_FIRST + 22)
#define NPPN_FILERENAMED                 (NPPN_FIRST + 23)
#define NPPN_FILEBEFOREDELETE            (NPPN_FIRST + 24)
#define NPPN_FILEDELETEFAILED            (NPPN_FIRST + 25)
#define NPPN_FILEDELETED                 (NPPN_FIRST + 26)
#define NPPN_DARKMODECHANGED             (NPPN_FIRST + 27)

/* ------------------------------------------------------------------
 * Public hooks for editor.c / main.c to fire notifications.
 * ------------------------------------------------------------------ */
void plugin_notify_buffer_activated(void *npp_doc_or_sci);
void plugin_notify_file_opened     (void *npp_doc_or_sci);
void plugin_notify_file_saved      (void *npp_doc_or_sci);
void plugin_notify_file_before_close(void *npp_doc_or_sci);
void plugin_notify_file_closed     (void *npp_doc_or_sci);
void plugin_notify_lang_changed    (void *npp_doc_or_sci);
void plugin_notify_readonly_changed(void *npp_doc_or_sci);
void plugin_notify_doc_order_changed(void);
void plugin_notify_ready(void);
void plugin_notify_before_shutdown(void);
void plugin_notify_shutdown(void);

/* Field IDs for NPPM_SETSTATUSBAR */
#define STATUSBAR_DOC_TYPE      0
#define STATUSBAR_DOC_SIZE      1
#define STATUSBAR_CUR_POS       2
#define STATUSBAR_EOF_FORMAT    3
#define STATUSBAR_UNICODE_TYPE  4
#define STATUSBAR_TYPING_MODE   5

/* ------------------------------------------------------------------
 * Five symbols a plugin .so must export
 *
 *   const char *getName(void);
 *   FuncItem   *getFuncsArray(int *nbF);
 *   void        beNotified(SCNotification *pNotify);
 *   long        messageProc(unsigned int msg, unsigned long wParam, long lParam);
 *   int         isUnicode(void);
 *
 * Optional (called before getFuncsArray if present):
 *   void        setInfo(NppData nppData);
 *
 * Plugin directory layout:
 *   <local data dir>/plugins/<PluginName>/<PluginName>.so
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------
 * Host API — called from main.c and editor.c
 * ------------------------------------------------------------------ */
void  plugin_init(GtkWidget *main_window);
void  plugin_load_all(void);
void  plugin_refresh_handles(void);           /* update NppData.scintilla*Handle */
void  plugin_notify_all(void *pNotify);       /* pass SCNotification * cast to void * */
void  plugin_notify_tbmodification(void);     /* GAP-88a — right after NPPN_READY */
long  plugin_host_message(unsigned int msg, unsigned long wParam, long lParam);

/* GAP-81 — plugin-panel persistence glue (used by panelstate.c).
 * plugin_panels_save_tokens: allocated space-separated tokens
 * ("plugin=<esc-module>,<cmdIndex>,<esc-cmdName>,<esc-title>,<popped01>,
 * <W>x<H>,<pin01>" — float geometry rides the token because the floating
 * registry keys plugin panels by load-order-dependent names) for every
 * OPEN panel whose plugin declared NPPM_DMM_SETPANELINFO; "" when none.
 * plugin_panel_restore: the tier ladder for one saved token (call after
 * plugins are loaded). */
char *plugin_panels_save_tokens(void);
void  plugin_panel_restore(const char *module, int cmd_index,
                           const char *cmd_name, const char *title,
                           gboolean popped, int float_w, int float_h,
                           gboolean float_pinned);
int   plugin_count(void);

/* FuncItem enumeration (dynamic Plugins menu) + dispatch by cmdID
 * (NPPM_MENUCOMMAND, macro replay of plugin commands — GAP-20). */
const char *plugin_name_at(int i);
int         plugin_func_count(int i);
const char *plugin_func_name(int i, int j);   /* "-" = separator */
int         plugin_func_cmd_id(int i, int j);
int         plugin_func_init2check(int i, int j);   /* GAP-88b seed */
gboolean    plugin_run_command_by_id(int cmd_id);

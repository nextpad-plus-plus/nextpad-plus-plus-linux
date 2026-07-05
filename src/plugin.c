#include "plugin.h"
#include "paths.h"
#include "statusbar.h"
#include "gtk_compat.h"
#include "branding.h"
#include "editor.h"
#include "split.h"
#include "prefs.h"
#include "session.h"
#include "findinfiles.h"
#include "doclist.h"
#include "udl.h"
#include <limits.h>
#include <dlfcn.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------
 * Plugin symbol typedefs
 * ------------------------------------------------------------------ */
typedef const char *(*GetName_t)(void);
typedef FuncItem   *(*GetFuncsArray_t)(int *);
typedef void        (*BeNotified_t)(void *);
typedef long        (*MessageProc_t)(unsigned int, unsigned long, long);
typedef int         (*IsUnicode_t)(void);
typedef void        (*SetInfo_t)(NppData);

/* ------------------------------------------------------------------
 * Internal plugin record
 * ------------------------------------------------------------------ */
typedef struct {
    void          *dl_handle;
    char           name[128];
    FuncItem      *funcs;
    int            n_funcs;
    BeNotified_t   be_notified;
    MessageProc_t  message_proc;
} LoadedPlugin;

#define MAX_PLUGINS   64
#define CMD_ID_BASE   10000

static LoadedPlugin  s_plugins[MAX_PLUGINS];
static int           s_n_plugins   = 0;
static GtkWidget    *s_window      = NULL;
static NppData       s_npp_data;
static int           s_next_cmd_id = CMD_ID_BASE;

/* forward declaration */
static long host_msg_cb(unsigned int msg, unsigned long wParam, long lParam);

/* ------------------------------------------------------------------
 * Load one plugin from a .so path; silently skip if invalid
 * ------------------------------------------------------------------ */
static void load_plugin(const char *sopath)
{
    if (s_n_plugins >= MAX_PLUGINS) return;

    void *h = dlopen(sopath, RTLD_LAZY | RTLD_LOCAL);
    if (!h) return;

    GetName_t       get_name     = (GetName_t)      dlsym(h, "getName");
    GetFuncsArray_t get_funcs    = (GetFuncsArray_t) dlsym(h, "getFuncsArray");
    BeNotified_t    be_notified  = (BeNotified_t)   dlsym(h, "beNotified");
    MessageProc_t   msg_proc     = (MessageProc_t)  dlsym(h, "messageProc");
    IsUnicode_t     is_unicode   = (IsUnicode_t)    dlsym(h, "isUnicode");

    if (!get_name || !get_funcs || !be_notified || !msg_proc || !is_unicode) {
        dlclose(h);
        return;
    }

    /* Optional: pass NppData before querying func array */
    SetInfo_t set_info = (SetInfo_t) dlsym(h, "setInfo");
    if (set_info) set_info(s_npp_data);

    LoadedPlugin *p = &s_plugins[s_n_plugins];
    p->dl_handle   = h;
    p->be_notified = be_notified;
    p->message_proc = msg_proc;

    const char *raw = get_name();
    snprintf(p->name, sizeof(p->name), "%s", raw ? raw : "Plugin");

    int n = 0;
    FuncItem *funcs = get_funcs(&n);
    p->funcs   = (n > 0 && funcs) ? funcs : NULL;
    p->n_funcs = (n > 0 && funcs) ? n     : 0;

    for (int i = 0; i < p->n_funcs; i++)
        p->funcs[i].cmdID = s_next_cmd_id++;

    s_n_plugins++;
    g_message("plugin: loaded '%s' (%d items) from %s", p->name, p->n_funcs, sopath);
}

/* ------------------------------------------------------------------
 * Scan a directory for plugins laid out as <dir>/<Name>/<Name>.so
 * ------------------------------------------------------------------ */
static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char sopath[2048];
        snprintf(sopath, sizeof(sopath), "%s/%s/%s.so",
                 dir, ent->d_name, ent->d_name);
        load_plugin(sopath);
    }
    closedir(d);
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

void plugin_init(GtkWidget *main_window)
{
    s_window                         = main_window;
    s_npp_data.nppHandle             = main_window;
    s_npp_data.scintillaMainHandle   = NULL;
    s_npp_data.scintillaSecondHandle = NULL;
    s_npp_data.hostMsg               = host_msg_cb;
}

/* Refresh the Sci handles in NppData after the editor and split-view
 * widgets exist. Called from main.c once build_main_window has returned.
 * macOS keeps a similar invariant — the handles are pointers the
 * plugin treats as opaque and passes back via host_msg_cb so the host
 * routes SCI_* messages to the correct view (see plugin_split_view_routing
 * in NppPluginManager.mm:1343-1368). */
void plugin_refresh_handles(void)
{
    NppDoc *cur = editor_current_doc();
    s_npp_data.scintillaMainHandle = cur ? cur->sci : NULL;
    s_npp_data.scintillaSecondHandle = split_secondary_current_sci();
}

void plugin_load_all(void)
{
    /* User plugins */
    gchar *user_dir = npp_user_subdir("plugins");
    g_mkdir_with_parents(user_dir, 0755);
    scan_dir(user_dir);
    g_free(user_dir);

    /* System-wide plugins (optional) */
    scan_dir("/usr/lib/nextpad-plus-plus/plugins");
    scan_dir("/usr/local/lib/nextpad-plus-plus/plugins");
}

void plugin_notify_all(void *pNotify)
{
    for (int i = 0; i < s_n_plugins; i++) {
        if (s_plugins[i].be_notified)
            s_plugins[i].be_notified(pNotify);
    }
}

int plugin_count(void)
{
    return s_n_plugins;
}

/* ------------------------------------------------------------------
 * NPPM host message router
 * ------------------------------------------------------------------ */
static long host_msg_cb(unsigned int msg, unsigned long wParam, long lParam)
{
    return plugin_host_message(msg, wParam, lParam);
}

/* External hooks from sci_c.h */
#include "sci_c.h"
#include "lexer.h"

/* ------------------------------------------------------------------
 * G33 — NPPN_* notification dispatching
 *
 * Each helper builds an SCNotification with nmhdr.code = NPPN_* and
 * nmhdr.idFrom = (uintptr_t)buffer_id (NppDoc *). The notification is
 * sent to every loaded plugin's beNotified() entry. Plugins compare
 * code to their own NPPN_* expectations.
 * ------------------------------------------------------------------ */

static void fire_notification(unsigned int code, void *buffer_id) {
    SCNotification n;
    memset(&n, 0, sizeof(n));
    n.nmhdr.hwndFrom = NULL;
    n.nmhdr.idFrom   = (uptr_t)buffer_id;
    n.nmhdr.code     = code;
    /* Use the generic plugin_notify_all dispatcher already wired. */
    plugin_notify_all(&n);
}

void plugin_notify_buffer_activated(void *buf)  { fire_notification(NPPN_BUFFERACTIVATED, buf); }
void plugin_notify_file_opened(void *buf)        { fire_notification(NPPN_FILEOPENED,     buf); }
void plugin_notify_file_saved(void *buf)         { fire_notification(NPPN_FILESAVED,      buf); }
void plugin_notify_file_before_close(void *buf)  { fire_notification(NPPN_FILEBEFORECLOSE,buf); }
void plugin_notify_file_closed(void *buf)        { fire_notification(NPPN_FILECLOSED,     buf); }
void plugin_notify_lang_changed(void *buf)       { fire_notification(NPPN_LANGCHANGED,    buf); }
void plugin_notify_readonly_changed(void *buf)   { fire_notification(NPPN_READONLYCHANGED,buf); }
void plugin_notify_doc_order_changed(void)       { fire_notification(NPPN_DOCORDERCHANGED, NULL); }
void plugin_notify_ready(void)                   { fire_notification(NPPN_READY,           NULL); }
void plugin_notify_before_shutdown(void)         { fire_notification(NPPN_BEFORESHUTDOWN,  NULL); }
void plugin_notify_shutdown(void)                { fire_notification(NPPN_SHUTDOWN,        NULL); }

/* Track plugin panel registrations — stub list for now; G21 hooks them in. */
typedef struct { void *panel_widget; char title[128]; int visible; } PluginPanel;
static PluginPanel s_plugin_panels[32];
static int         s_plugin_panel_count = 0;

/* Indicator / marker slot allocators — start at the Scintilla-reserved-for-app
 * range and increment. INDIC_CONTAINER (8) is the lowest indicator slot
 * plugins may use; markers 25..31 are reserved for plugins per Scintilla
 * convention. */
static int s_next_indicator = INDIC_CONTAINER + 1;  /* 9 onward */
static int s_next_marker    = 25;
static int s_next_alloc_cmd = 11000;                /* well above CMD_ID_BASE */

long plugin_host_message(unsigned int msg, unsigned long wParam, long lParam)
{
    switch (msg) {

    /* ── Scintilla / view ────────────────────────────────────────────── */
    case NPPM_GETCURRENTSCINTILLA: {
        NppDoc *doc = editor_current_doc();
        /* lParam may be a `int *` to receive the view id (0 or 1); we only
         * have view 0 right now. */
        if (lParam) *((int *)(intptr_t)lParam) = 0;
        (void)wParam;
        return doc ? (long)(intptr_t)doc->sci : 0L;
    }
    case NPPM_GETCURRENTVIEW:
        return 0L;
    case NPPM_GETCURRENTBUFFERID: {
        NppDoc *doc = editor_current_doc();
        return doc ? (long)(intptr_t)doc : 0L;
    }
    case NPPM_GETFULLPATHFROMBUFFERID: {
        NppDoc *doc = (NppDoc *)(intptr_t)wParam;
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return doc && doc->filepath ? (long)strlen(doc->filepath) : 0;
        if (doc && doc->filepath) snprintf(buf, 2048, "%s", doc->filepath);
        else buf[0] = '\0';
        return doc && doc->filepath ? (long)strlen(doc->filepath) : 0;
    }

    /* ── Document set ────────────────────────────────────────────────── */
    case NPPM_GETNBOPENFILES:
        return (long)editor_page_count();
    case NPPM_DOOPEN: {
        const char *path = (const char *)(intptr_t)lParam;
        if (path && *path) return editor_open_path(path) ? 1 : 0;
        return 0;
    }

    /* ── Path queries ────────────────────────────────────────────────── */
    case NPPM_GETFULLCURRENTPATH: {
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0L;
        NppDoc *doc = editor_current_doc();
        if (doc && doc->filepath) snprintf(buf, 2048, "%s", doc->filepath);
        else buf[0] = '\0';
        return 1L;
    }
    case NPPM_GETFILENAME: {
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0L;
        NppDoc *doc = editor_current_doc();
        if (doc && doc->filepath) {
            const char *base = strrchr(doc->filepath, '/');
            snprintf(buf, 256, "%s", base ? base + 1 : doc->filepath);
        } else buf[0] = '\0';
        return 1L;
    }
    case NPPM_GETDIRECTORYPATH: {
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0L;
        NppDoc *doc = editor_current_doc();
        if (doc && doc->filepath) {
            char *dir = g_path_get_dirname(doc->filepath);
            snprintf(buf, 2048, "%s", dir); g_free(dir);
        } else buf[0] = '\0';
        return 1L;
    }
    case NPPM_GETNAMEPART: {
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0L;
        NppDoc *doc = editor_current_doc();
        if (doc && doc->filepath) {
            const char *base = strrchr(doc->filepath, '/');
            const char *name = base ? base + 1 : doc->filepath;
            const char *dot = strrchr(name, '.');
            if (dot) snprintf(buf, 256, "%.*s", (int)(dot - name), name);
            else     snprintf(buf, 256, "%s", name);
        } else buf[0] = '\0';
        return 1L;
    }
    case NPPM_GETEXTPART: {
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0L;
        NppDoc *doc = editor_current_doc();
        if (doc && doc->filepath) {
            const char *dot = strrchr(doc->filepath, '.');
            snprintf(buf, 64, "%s", dot ? dot : "");
        } else buf[0] = '\0';
        return 1L;
    }

    /* ── App-wide paths ──────────────────────────────────────────────── */
    case NPPM_GETNPPDIRECTORY: {
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0L;
        /* System data dir is /usr/share/<APP_DATA_DIR> (NOT APP_CONFIG_DIR which is
         * the dotfile user dir). */
        snprintf(buf, 2048, "/usr/share/%s", APP_DATA_DIR);
        return 1L;
    }
    case NPPM_GETNPPSETTINGSDIRPATH: {
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0L;
        {
            gchar *p = npp_user_dir();
            g_strlcpy(buf, p, 2048);
            g_free(p);
        }
        return 1L;
    }
    case NPPM_GETPLUGINSCONFIGDIR: {
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0L;
        {
            gchar *p = npp_user_subdir("plugins");
            g_strlcpy(buf, p, 2048);
            g_free(p);
        }
        return 1L;
    }
    case NPPM_GETPLUGINHOMEPATH: {
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0L;
        /* Plugins live alongside their .so under .../plugins/<Name>/. We don't
         * track which plugin is calling, so return the parent dir. */
        {
            gchar *p = npp_user_subdir("plugins");
            g_strlcpy(buf, p, 2048);
            g_free(p);
        }
        return 1L;
    }
    case NPPM_GETAPPDATAPLUGINSALLOWED:
        return 1L;

    /* ── Caret position ──────────────────────────────────────────────── */
    case NPPM_GETCURRENTLINE: {
        NppDoc *doc = editor_current_doc();
        if (!doc) return 0;
        sptr_t pos = scintilla_send_message(SCINTILLA(doc->sci), SCI_GETCURRENTPOS, 0, 0);
        return (long)scintilla_send_message(SCINTILLA(doc->sci),
            SCI_LINEFROMPOSITION, pos, 0) + 1;
    }
    case NPPM_GETCURRENTCOLUMN: {
        NppDoc *doc = editor_current_doc();
        if (!doc) return 0;
        sptr_t pos = scintilla_send_message(SCINTILLA(doc->sci), SCI_GETCURRENTPOS, 0, 0);
        return (long)scintilla_send_message(SCINTILLA(doc->sci),
            SCI_GETCOLUMN, pos, 0) + 1;
    }
    case NPPM_GETCURRENTDIRECTORY:
        /* Synonym for GETDIRECTORYPATH (path of current doc's containing dir) */
        return plugin_host_message(NPPM_GETDIRECTORYPATH, wParam, lParam);

    /* ── Language ────────────────────────────────────────────────────── */
    case NPPM_GETCURRENTLANGTYPE: {
        /* Canonical Windows NPP LangType values, ported from macOS
         * NppPluginManager.mm:700-797. A plugin built against
         * Notepad_plus_msgs.h sees the same L_* IDs on every platform. */
        NppDoc *doc = editor_current_doc();
        int *out = (int *)(intptr_t)lParam;
        if (!doc || !doc->sci) { if (out) *out = 0; return 0; }
        const char *lang = g_object_get_data(G_OBJECT(doc->sci), "npp-lang");
        if (!lang) lang = "";
        static const struct { const char *name; int id; } lt[] = {
            { "c",            2  }, { "cpp",          3  }, { "cs",           4  },
            { "objc",         5  }, { "java",         6  }, { "rc",           7  },
            { "html",         8  }, { "xml",          9  }, { "makefile",     10 },
            { "pascal",       11 }, { "batch",        12 }, { "ini",          13 },
            { "asp",          16 }, { "sql",          17 }, { "vb",           18 },
            { "css",          20 }, { "perl",         21 }, { "python",       22 },
            { "lua",          23 }, { "tex",          24 }, { "fortran",      25 },
            { "bash",         26 }, { "actionscript", 27 }, { "nsis",         28 },
            { "tcl",          29 }, { "lisp",         30 }, { "scheme",       31 },
            { "asm",          32 }, { "diff",         33 }, { "props",        34 },
            { "postscript",   35 }, { "ruby",         36 }, { "smalltalk",    37 },
            { "vhdl",         38 }, { "kix",          39 }, { "autoit",       40 },
            { "caml",         41 }, { "ada",          42 }, { "verilog",      43 },
            { "matlab",       44 }, { "haskell",      45 }, { "inno",         46 },
            { "cmake",        48 }, { "yaml",         49 }, { "cobol",        50 },
            { "d",            52 }, { "powershell",   53 }, { "r",            54 },
            { "coffeescript", 56 }, { "json",         57 }, { "javascript",   58 },
            { "javascript.js",58 }, { "fortran77",    59 }, { "baanc",        60 },
            { "swift",        64 }, { "avs",          66 }, { "blitzbasic",   67 },
            { "purebasic",    68 }, { "freebasic",    69 }, { "csound",       70 },
            { "erlang",       71 }, { "escript",      72 }, { "forth",        73 },
            { "latex",        74 }, { "nim",          76 }, { "nncrontab",    77 },
            { "oscript",      78 }, { "registry",     80 }, { "rust",         81 },
            { "spice",        82 }, { "visualprolog", 84 }, { "typescript",   85 },
            { "mssql",        87 }, { "gdscript",     88 }, { "hollywood",    89 },
            { "go",           90 }, { "raku",         91 }, { "toml",         92 },
            { "sas",          93 },
        };
        int found = 0;
        for (size_t i = 0; i < G_N_ELEMENTS(lt); i++) {
            if (g_ascii_strcasecmp(lang, lt[i].name) == 0) { found = lt[i].id; break; }
        }
        if (out) *out = found;
        return found;
    }
    case NPPM_GETLANGUAGENAME: {
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0L;
        int langType = (int)wParam;
        static const struct { int id; const char *display; } lname[] = {
            { 0,  "Normal text"     }, { 1,  "PHP"               },
            { 2,  "C"               }, { 3,  "C++"               },
            { 4,  "C#"              }, { 5,  "Objective-C"       },
            { 6,  "Java"            }, { 7,  "RC"                },
            { 8,  "HTML"            }, { 9,  "XML"               },
            { 10, "Makefile"        }, { 11, "Pascal"            },
            { 12, "Batch"           }, { 13, "ini"               },
            { 16, "ASP"             }, { 17, "SQL"               },
            { 18, "Visual Basic"    }, { 20, "CSS"               },
            { 21, "Perl"            }, { 22, "Python"            },
            { 23, "Lua"             }, { 24, "TeX"               },
            { 25, "Fortran"         }, { 26, "Shell"             },
            { 27, "Flash ActionScript" }, { 28, "NSIS"           },
            { 29, "TCL"             }, { 30, "Lisp"              },
            { 31, "Scheme"          }, { 32, "Assembly"          },
            { 33, "Diff"            }, { 34, "Properties"        },
            { 35, "PostScript"      }, { 36, "Ruby"              },
            { 37, "Smalltalk"       }, { 38, "VHDL"              },
            { 39, "KiXtart"         }, { 40, "AutoIt"            },
            { 41, "CAML"            }, { 42, "Ada"               },
            { 43, "Verilog"         }, { 44, "MATLAB"            },
            { 45, "Haskell"         }, { 46, "Inno Setup"        },
            { 48, "CMake"           }, { 49, "YAML"              },
            { 50, "COBOL"           }, { 52, "D"                 },
            { 53, "PowerShell"      }, { 54, "R"                 },
            { 56, "CoffeeScript"    }, { 57, "JSON"              },
            { 58, "JavaScript"      }, { 59, "Fortran 77"        },
            { 60, "BaanC"           }, { 64, "Swift"             },
            { 66, "AviSynth"        }, { 67, "BlitzBasic"        },
            { 68, "PureBasic"       }, { 69, "FreeBasic"         },
            { 70, "Csound"          }, { 71, "Erlang"            },
            { 72, "ESCRIPT"         }, { 73, "Forth"             },
            { 74, "LaTeX"           }, { 76, "Nim"               },
            { 77, "nnCron"          }, { 78, "OScript"           },
            { 80, "Registry"        }, { 81, "Rust"              },
            { 82, "Spice"           }, { 84, "Visual Prolog"     },
            { 85, "TypeScript"      }, { 87, "MS-SQL"            },
            { 88, "GDScript"        }, { 89, "Hollywood"         },
            { 90, "Go"              }, { 91, "Raku"              },
            { 92, "TOML"            }, { 93, "SAS"               },
        };
        for (size_t i = 0; i < G_N_ELEMENTS(lname); i++) {
            if (lname[i].id == langType) {
                g_strlcpy(buf, lname[i].display, 1024);
                return (long)strlen(buf);
            }
        }
        buf[0] = '\0';
        return 0L;
    }

    /* ── Version / mode ──────────────────────────────────────────────── */
    case NPPM_GETNPPVERSION:
        /* high 16 = major, low 16 = minor — encode 1.0.6 as (1<<16) | 6 */
        return (long)((1 << 16) | 6);
    case NPPM_ISDARKMODEENABLED:
        /* Read prefs (dark mode toggle is in G11 polish; default FALSE). */
        return 0L;

    /* ── Resource allocators ─────────────────────────────────────────── */
    case NPPM_ALLOCATECMDID: {
        /* wParam = number of IDs to allocate; lParam = int * to receive base */
        int *out = (int *)(intptr_t)lParam;
        if (!out) return 0;
        *out = s_next_alloc_cmd;
        s_next_alloc_cmd += (int)wParam;
        return 1;
    }
    case NPPM_ALLOCATEINDICATOR: {
        /* wParam = count; lParam = int * base */
        int *out = (int *)(intptr_t)lParam;
        if (!out) return 0;
        if (s_next_indicator + (int)wParam > 31) return 0; /* range exhausted */
        *out = s_next_indicator;
        s_next_indicator += (int)wParam;
        return 1;
    }
    case NPPM_ALLOCATEMARKER: {
        int *out = (int *)(intptr_t)lParam;
        if (!out) return 0;
        if (s_next_marker + (int)wParam > 31) return 0;
        *out = s_next_marker;
        s_next_marker += (int)wParam;
        return 1;
    }
    case NPPM_GETBOOKMARKID:
        return (long)SC_MARKNUM_BOOKMARK;

    /* ── Dockable plugin panel API (G21 backbone) ────────────────────── */
    case NPPM_DMM_REGISTERPANEL: {
        /* lParam → tTbData struct (NSView*/ /*hClient on macOS). On Linux this is
         * a GtkWidget *. Stub: track it but don't show. G21 wires actual
         * floating/docking. */
        if (s_plugin_panel_count >= 32) return 0;
        PluginPanel *p = &s_plugin_panels[s_plugin_panel_count++];
        p->panel_widget = (void *)(intptr_t)lParam;
        p->visible = 0;
        return (long)s_plugin_panel_count;  /* opaque handle */
    }
    case NPPM_DMM_SHOWPANEL: {
        int idx = (int)wParam - 1;
        if (idx < 0 || idx >= s_plugin_panel_count) return 0;
        s_plugin_panels[idx].visible = 1;
        if (s_plugin_panels[idx].panel_widget)
            gtk_widget_show(GTK_WIDGET(s_plugin_panels[idx].panel_widget));
        return 1;
    }
    case NPPM_DMM_HIDEPANEL: {
        int idx = (int)wParam - 1;
        if (idx < 0 || idx >= s_plugin_panel_count) return 0;
        s_plugin_panels[idx].visible = 0;
        if (s_plugin_panels[idx].panel_widget)
            gtk_widget_hide(GTK_WIDGET(s_plugin_panels[idx].panel_widget));
        return 1;
    }
    case NPPM_DMM_UNREGISTERPANEL: {
        /* No actual deallocation — stub. */
        return 1;
    }

    /* ── Menu / toolbar (stub-only for now) ──────────────────────────── */
    case NPPM_GETMENUHANDLE:
        /* No HMENU equivalent on Linux; return NULL. Plugins should fall
         * back to NPPM_MENUCOMMAND if they relied on raw menu access. */
        return 0;
    case NPPM_ADDTOOLBARICON_FORDARKMODE:
        /* Toolbar icon registration — stub. */
        return 1;
    case NPPM_DARKMODESUBCLASSANDTHEME:
        return 0;

    /* ── G36 additions ───────────────────────────────────────────────── */
    case NPPM_SAVECURRENTSESSION: {
        extern void session_save(void);
        session_save();
        return 1;
    }
    case NPPM_SAVEALLFILES: {
        return editor_save_all() ? 1 : 0;
    }
    case NPPM_SAVEFILE: {
        /* lParam = path; save the buffer whose filepath matches. We only
         * track the active doc easily, so this is a no-op if not active. */
        const char *path = (const char *)(intptr_t)lParam;
        NppDoc *d = editor_current_doc();
        if (d && d->filepath && path && strcmp(d->filepath, path) == 0)
            return editor_save() ? 1 : 0;
        return 0;
    }
    case NPPM_RELOADFILE: {
        const char *path = (const char *)(intptr_t)lParam;
        NppDoc *d = editor_current_doc();
        if (d && d->filepath && path && strcmp(d->filepath, path) == 0) {
            editor_reload_current();
            return 1;
        }
        return 0;
    }
    case NPPM_RELOADBUFFERID: {
        NppDoc *d = (NppDoc *)(intptr_t)wParam;
        if (d && d == editor_current_doc()) {
            editor_reload_current();
            return 1;
        }
        return 0;
    }
    case NPPM_MENUCOMMAND: {
        /* lParam = command ID. Map our plugin command IDs back to FuncItem
         * and invoke. */
        int id = (int)lParam;
        for (int i = 0; i < s_n_plugins; i++) {
            LoadedPlugin *p = &s_plugins[i];
            for (int j = 0; j < p->n_funcs; j++) {
                if (p->funcs[j].cmdID == id && p->funcs[j].pFunc) {
                    p->funcs[j].pFunc();
                    return 1;
                }
            }
        }
        return 0;
    }
    case NPPM_GETCURRENTMACROSTATUS: {
        /* 0 = none, 1 = recording, 2 = playable. */
        extern gboolean macro_is_recording(void);
        extern gboolean macro_has_macro(void);
        if (macro_is_recording()) return 1;
        if (macro_has_macro())    return 2;
        return 0;
    }
    case NPPM_CREATESCINTILLAHANDLE: {
        editor_new_doc();
        NppDoc *d = editor_current_doc();
        return d ? (long)(intptr_t)d->sci : 0;
    }
    case NPPM_DESTROYSCINTILLAHANDLE: {
        /* Plugins shouldn't destroy sci widgets directly on Linux — the
         * tab manager owns lifecycle. Return 1 to claim handled. */
        return 1;
    }
    case NPPM_GETNBSESSIONFILES: {
        return (long)editor_page_count();
    }
    case NPPM_GETSESSIONFILES: {
        char **arr = (char **)(intptr_t)lParam;
        if (!arr) return 0;
        int n = editor_page_count();
        int out = 0;
        for (int i = 0; i < n; i++) {
            NppDoc *d = editor_doc_at(i);
            if (d && d->filepath) arr[out++] = g_strdup(d->filepath);
        }
        return out;
    }
    case NPPM_GETLINENUMBERWIDTHMODE:
        return 0;  /* 0 = dynamic, 1 = constant */
    case NPPM_SETLINENUMBERWIDTHMODE:
        return 1;  /* accept but no-op for now */
    case NPPM_TRIGGERTABBARCONTEXTMENU:
        /* Plugins can request the tab context menu to appear. Stub. */
        return 0;

    /* ── G36 batch 2 ─────────────────────────────────────────────────── */
    case NPPM_GETOPENFILENAMES:
    case NPPM_GETOPENFILENAMESPRIMARY: {
        char **arr = (char **)(intptr_t)wParam;
        int    cap = (int)lParam;
        if (!arr) return (long)editor_page_count();
        int n = editor_page_count();
        int out = 0;
        for (int i = 0; i < n && out < cap; i++) {
            NppDoc *d = editor_doc_at(i);
            if (d && d->filepath) arr[out++] = g_strdup(d->filepath);
        }
        return (long)out;
    }
    case NPPM_GETOPENFILENAMESSECOND:
        /* No second view yet (G14 deferred). */
        return 0;
    case NPPM_SAVECURRENTSESSIONAS: {
        /* lParam = path; we don't support custom paths yet — fall back to default. */
        extern void session_save(void);
        session_save();
        return 1;
    }
    case NPPM_LOADSESSION: {
        extern void session_restore(void);
        session_restore();
        return 1;
    }
    case NPPM_ACTIVATEDOC: {
        /* wParam = view (0/1), lParam = index */
        GtkWidget *n = editor_get_notebook();
        if (n) gtk_notebook_set_current_page(GTK_NOTEBOOK(n), (int)lParam);
        return 1;
    }
    case NPPM_RUNMENUCOMMAND:
        /* Synonym for MENUCOMMAND. */
        return plugin_host_message(NPPM_MENUCOMMAND, wParam, lParam);

    case NPPM_GETBUFFERLANGTYPE:
        return plugin_host_message(NPPM_GETCURRENTLANGTYPE, wParam, lParam);
    case NPPM_SETBUFFERLANGTYPE:
        /* lParam = L_* enum value. Reverse-map and apply via lexer_apply. */
        return 0;  /* stub */
    case NPPM_GETBUFFERENCODING: {
        NppDoc *d = (NppDoc *)(intptr_t)wParam;
        if (!d) d = editor_current_doc();
        /* Return a numeric encoding hint: 0=UTF-8, 1=UTF-8 BOM, 2=UTF-16LE,
         * 3=UTF-16BE, 4=ANSI. Stub mapping. */
        if (d && d->encoding) {
            if (!strcmp(d->encoding, "UTF-8 BOM"))     return 1;
            if (!strcmp(d->encoding, "UTF-16 LE BOM")) return 2;
            if (!strcmp(d->encoding, "UTF-16 BE BOM")) return 3;
            if (!strcmp(d->encoding, "Windows-1252"))  return 4;
        }
        return 0;
    }
    case NPPM_SETBUFFERENCODING:
        return 0;  /* stub */
    case NPPM_GETBUFFERFORMAT: {
        /* EOL mode: 0=CRLF, 1=CR, 2=LF */
        NppDoc *d = (NppDoc *)(intptr_t)wParam;
        if (!d) d = editor_current_doc();
        if (!d) return 2;
        return (long)scintilla_send_message(SCINTILLA(d->sci), SCI_GETEOLMODE, 0, 0);
    }
    case NPPM_SETBUFFERFORMAT: {
        NppDoc *d = (NppDoc *)(intptr_t)wParam;
        if (!d) d = editor_current_doc();
        if (!d) return 0;
        scintilla_send_message(SCINTILLA(d->sci), SCI_SETEOLMODE, (uptr_t)lParam, 0);
        scintilla_send_message(SCINTILLA(d->sci), SCI_CONVERTEOLS, (uptr_t)lParam, 0);
        return 1;
    }

    /* UI visibility queries / setters — all stub-OK */
    case NPPM_HIDETABBAR:        return 1;
    case NPPM_ISTABBARHIDDEN:    return 0;
    case NPPM_HIDETOOLBAR:       return 1;
    case NPPM_ISTOOLBARHIDDEN:   return 0;
    case NPPM_HIDESTATUSBAR:     return 1;
    case NPPM_ISSTATUSBARHIDDEN: return 0;
    /* Plugin-writable status-bar text. macOS b5b73b2 gives plugins one
     * dedicated middle field; every STATUSBAR_* id routes there so the
     * built-in left/right blocks can't be clobbered. */
    case NPPM_SETSTATUSBAR: {
        const char *text = (const char *)(intptr_t)lParam;
        statusbar_set_plugin_text(text);
        return 1;
    }
    case NPPM_HIDEMENU:          return 1;
    case NPPM_ISMENUHIDDEN:      return 0;
    case NPPM_SHOWDOCSWITCHER:   return 1;
    case NPPM_ISDOCSWITCHERSHOWN: return 0;

    case NPPM_GETLANGUAGEDESC: {
        /* wParam = L_* id, lParam = char * buf. Return generic display name. */
        char *buf = (char *)(intptr_t)lParam;
        if (!buf) return 0;
        snprintf(buf, 64, "%s", "Generic Language");
        return 1;
    }
    case NPPM_MAKECURRENTBUFFERDIRTY: {
        NppDoc *d = editor_current_doc();
        if (d && d->sci)
            scintilla_send_message(SCINTILLA(d->sci), SCI_SETSAVEPOINT, 0, 0);
        return 1;
    }
    case NPPM_GETSETTINGSONCLOUDPATH: {
        char *buf = (char *)(intptr_t)lParam;
        if (buf) buf[0] = '\0';  /* No cloud-settings feature on Linux. */
        return 0;
    }
    case NPPM_SETSMOOTHFONT:
    case NPPM_SETEDITORBORDEREDGE:
        return 1;  /* accept, no-op */
    case NPPM_MSGTOPLUGIN: {
        /* lParam = communicationInfo*, wParam = target plugin name.
         * Plugin-to-plugin messaging; we'd need a name → handle map.
         * Stub returns 0 (not delivered). */
        return 0;
    }

    /* ENABLED 2026-05-14 — plugin.h renumbered to canonical Notepad++
     * offsets, so these handlers no longer collide with the existing ones. */
    /* ────────────────────────────────────────────────────────────── */
    /* Q-fix block: 20 macOS-parity NPPM_* handlers (plugin SDK gap).  */
    /* Mirrors NppPluginManager.mm dispatcher entries from the macOS  */
    /* port. Each lands the minimal behavior plugins expect — full    */
    /* fidelity comes incrementally as host features mature.           */
    /* ────────────────────────────────────────────────────────────── */

    /* Word under caret. lParam = char*, wParam = buf size. */
    case NPPM_GETCURRENTWORD: {
        char *out = (char *)(intptr_t)lParam; if (!out) return 0;
        NppDoc *doc = editor_current_doc();
        if (!doc) { out[0] = '\0'; return 0; }
        ScintillaObject *sci = SCINTILLA(doc->sci);
        sptr_t pos = scintilla_send_message(sci, SCI_GETCURRENTPOS, 0, 0);
        sptr_t s   = scintilla_send_message(sci, SCI_WORDSTARTPOSITION, (uptr_t)pos, TRUE);
        sptr_t e   = scintilla_send_message(sci, SCI_WORDENDPOSITION,   (uptr_t)pos, TRUE);
        sptr_t n   = e - s;
        if (n <= 0 || (unsigned long)n + 1 > wParam) { out[0] = '\0'; return 0; }
        struct Sci_TextRangeFull tr = {{s, e}, out};
        scintilla_send_message(sci, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
        return 1;
    }

    /* Current line text (no newline). lParam = char*, wParam = buf size. */
    case NPPM_GETCURRENTLINESTR: {
        char *out = (char *)(intptr_t)lParam; if (!out) return 0;
        NppDoc *doc = editor_current_doc();
        if (!doc) { out[0] = '\0'; return 0; }
        ScintillaObject *sci = SCINTILLA(doc->sci);
        sptr_t pos  = scintilla_send_message(sci, SCI_GETCURRENTPOS, 0, 0);
        sptr_t line = scintilla_send_message(sci, SCI_LINEFROMPOSITION, (uptr_t)pos, 0);
        sptr_t need = scintilla_send_message(sci, SCI_LINELENGTH, (uptr_t)line, 0);
        if ((unsigned long)need + 1 > wParam) { out[0] = '\0'; return 0; }
        scintilla_send_message(sci, SCI_GETLINE, (uptr_t)line, (sptr_t)out);
        out[need] = '\0';
        /* Strip trailing CR/LF for caller convenience. */
        while (need > 0 && (out[need-1] == '\n' || out[need-1] == '\r'))
            out[--need] = '\0';
        return 1;
    }

    /* Current tab index in the notebook. */
    case NPPM_GETCURRENTDOCINDEX: {
        (void)wParam; (void)lParam;
        return (long)editor_current_page();
    }

    /* Path → buffer id (we use the doc pointer as the id). */
    case NPPM_GETPOSFROMBUFFERID: {
        NppDoc *target = (NppDoc *)(intptr_t)wParam; if (!target) return -1;
        GtkWidget *nb = editor_get_notebook();
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb));
        for (int i = 0; i < n; i++)
            if (editor_doc_at(i) == target) return i;
        return -1;
    }

    /* Position → buffer id. wParam = tab index. */
    case NPPM_GETBUFFERIDFROMPOS: {
        NppDoc *d = editor_doc_at((int)wParam);
        return d ? (long)(intptr_t)d : 0;
    }

    /* File path under the caret (selection-as-path). Used by plugins to
     * open the file the user clicked. lParam = char* buf, wParam = size. */
    case NPPM_GETFILENAMEATCURSOR: {
        char *out = (char *)(intptr_t)lParam; if (!out) return 0;
        NppDoc *doc = editor_current_doc();
        if (!doc) { out[0] = '\0'; return 0; }
        ScintillaObject *sci = SCINTILLA(doc->sci);
        sptr_t ss = scintilla_send_message(sci, SCI_GETSELECTIONSTART, 0, 0);
        sptr_t se = scintilla_send_message(sci, SCI_GETSELECTIONEND, 0, 0);
        sptr_t pos = (ss == se)
            ? scintilla_send_message(sci, SCI_GETCURRENTPOS, 0, 0)
            : ss;
        sptr_t s = scintilla_send_message(sci, SCI_WORDSTARTPOSITION, (uptr_t)pos, TRUE);
        sptr_t e = scintilla_send_message(sci, SCI_WORDENDPOSITION,   (uptr_t)pos, TRUE);
        if (ss != se) { s = ss; e = se; }
        sptr_t n = e - s;
        if (n <= 0 || (unsigned long)n + 1 > wParam) { out[0] = '\0'; return 0; }
        struct Sci_TextRangeFull tr = {{s, e}, out};
        scintilla_send_message(sci, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
        return 1;
    }

    /* Path to the running Nextpad++ binary. */
    case NPPM_GETNPPFULLFILEPATH: {
        char *out = (char *)(intptr_t)lParam; if (!out) return 0;
        char buf[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) { out[0] = '\0'; return 0; }
        buf[n] = '\0';
        if ((unsigned long)n + 1 > wParam) { out[0] = '\0'; return 0; }
        memcpy(out, buf, n + 1);
        return 1;
    }

    /* Editor default fg/bg colours, as BGR-int (Scintilla's COLORREF). */
    case NPPM_GETEDITORDEFAULTFOREGROUNDCOLOR: {
        NppDoc *doc = editor_current_doc();
        if (!doc) return 0x000000;
        return (long)scintilla_send_message(SCINTILLA(doc->sci),
            SCI_STYLEGETFORE, STYLE_DEFAULT, 0);
    }
    case NPPM_GETEDITORDEFAULTBACKGROUNDCOLOR: {
        NppDoc *doc = editor_current_doc();
        if (!doc) return 0xFFFFFF;
        return (long)scintilla_send_message(SCINTILLA(doc->sci),
            SCI_STYLEGETBACK, STYLE_DEFAULT, 0);
    }

    /* Dark-mode palette block. lParam = NppDarkMode::Colors* (5 BGR ints).
     * We hand back a sensible default palette derived from theme state. */
    case NPPM_GETDARKMODECOLORS: {
        unsigned *out = (unsigned *)(intptr_t)lParam; if (!out) return 0;
        gboolean dark = FALSE;
        GtkSettings *s = gtk_settings_get_default();
        if (s) g_object_get(s, "gtk-application-prefer-dark-theme", &dark, NULL);
        if (dark) {
            out[0] = 0x1E1E1E; out[1] = 0x2D2D2D; out[2] = 0xD4D4D4;
            out[3] = 0x404040; out[4] = 0x569CD6;
        } else {
            out[0] = 0xFFFFFF; out[1] = 0xF0F0F0; out[2] = 0x000000;
            out[3] = 0xC0C0C0; out[4] = 0x0000FF;
        }
        return 1;
    }

    /* Save the current file. */
    case NPPM_SAVECURRENTFILE: {
        (void)wParam; (void)lParam;
        return (long)editor_save();
    }
    case NPPM_SAVECURRENTFILEAS: {
        /* macOS variant takes wParam = use-copy flag, lParam = char* path.
         * On Linux we only honour the dialog form for now; the parametric
         * path-save variant would need a path-aware helper added to editor.h. */
        (void)wParam; (void)lParam;
        return (long)editor_save_as_dialog();
    }
    case NPPM_SAVESESSION: {
        /* macOS variant takes a session-config struct. We honour the
         * default-location save; arbitrary-path save lives at follow-up. */
        (void)wParam; (void)lParam;
        session_save();
        return 1;
    }

    /* Doc list (the side panel that lists open documents). */
    case NPPM_ISDOCLISTSHOWN: {
        return doclist_is_visible() ? 1 : 0;
    }
    case NPPM_SHOWDOCLIST: {
        doclist_set_visible(wParam ? TRUE : FALSE);
        return 1;
    }
    /* macOS uses SWITCHTOFILE with a file path. editor_open_path is
     * idempotent — if the path is already open, it focuses that tab. */
    case NPPM_SWITCHTOFILE: {
        const char *path = (const char *)(intptr_t)lParam;
        if (!path || !*path) return 0;
        return editor_open_path(path) ? 1 : 0;
    }
    /* Override the displayed name of an Untitled doc — stub:
     * label refresh helper lives in editor.c and isn't yet exported. */
    case NPPM_SETUNTITLEDNAME: {
        (void)wParam; (void)lParam;
        return 0;
    }

    /* Toggle a checkbox on a registered command id. The full
     * implementation requires the plugin loader to expose each FuncItem
     * as a GAction+GMenuItem so we can locate and toggle the check
     * attribute (matches macOS NppPluginManager.mm:991-1019, which
     * walks the NSMenu tree). The loader still calls _pFunc callbacks
     * directly, so no menu item exists today — accept the request as a
     * no-op until the loader insertion lands (tracked separately). */
    case NPPM_SETMENUITEMCHECK:        return 1;
    /* Open the Find-in-Files dialog. */
    case NPPM_LAUNCHFINDINFILESDLG: {
        (void)wParam;
        const char *initial_text = (const char *)(intptr_t)lParam;
        findinfiles_show(NULL, initial_text);
        return 1;
    }

    /* DMM panel verbs that don't take a registered panel ptr — these are
     * the deprecated macOS aliases. They forward to the registered-panel
     * variants we already implement above. */
    case NPPM_DMMSHOW:            return plugin_host_message(NPPM_DMM_SHOWPANEL,   wParam, lParam);
    case NPPM_DMMHIDE:            return plugin_host_message(NPPM_DMM_HIDEPANEL,   wParam, lParam);
    case NPPM_DMMUPDATEDISPINFO:  return 1;  /* refresh request — no-op */
    case NPPM_DMMVIEWOTHERTAB:    return 1;  /* multi-view stub */
    case NPPM_DMMREGASDCKDLG:     return plugin_host_message(NPPM_DMM_REGISTERPANEL, wParam, lParam);
    case NPPM_DMMGETPLUGINHWNDBYNAME: {
        /* Stub: returning 0 means "no panel registered under that name".
         * The panel-by-name lookup helper isn't exposed yet. */
        (void)wParam;
        return 0;
    }

    /* Shortcut introspection. lParam = ShortcutKey*; wParam = cmd id. We
     * surface "no shortcut" for IDs we don't recognise. Plugins that need
     * the actual binding can read it from shortcuts.xml. */
    case NPPM_GETSHORTCUTBYCMDID: {
        struct { unsigned char modifiers; unsigned char key; } *sk =
            (void *)(intptr_t)lParam;
        if (sk) { sk->modifiers = 0; sk->key = 0; }
        return 0;
    }
    case NPPM_REMOVESHORTCUTBYCMDID:  return 1;

    /* Catalog scaffolding plugins may probe. */
    case NPPM_GETNBUSERLANG:          return (long)udl_count();
    case NPPM_ISAUTOINDENTON:         return g_prefs.auto_indent ? 1 : 0;
    case NPPM_GETTABCOLORID:          return -1;  /* tab colour API stub */
    case NPPM_GETTOOLBARICONSETCHOICE:return (long)g_prefs.toolbar_icon_scale;
    /* Stubs for the remaining canonical handlers. */
    case NPPM_DISABLEAUTOUPDATE:        return 1;
    case NPPM_DOCLISTDISABLEEXTCOLUMN:  return 1;
    case NPPM_DOCLISTDISABLEPATHCOLUMN: return 1;
    case NPPM_MODELESSDIALOG:           return 1;
    case NPPM_GETCURRENTCMDLINE: {
        char *out = (char *)(intptr_t)lParam; if (out) out[0] = '\0';
        return 0;
    }
    case NPPM_GETNATIVELANGFILENAME: {
        char *out = (char *)(intptr_t)lParam; if (out) out[0] = '\0';
        return 0;
    }
    case NPPM_GETCURRENTNATIVELANGENCODING: return 65001;  /* UTF-8 */
    case NPPM_GETWINDOWSVERSION:            return 0;       /* not Windows */
    case NPPM_ADDTOOLBARICON:               return 1;
    case NPPM_ENCODESCI:
    case NPPM_DECODESCI:                    return 1;
    case NPPM_CREATELEXER:                  return 0;       /* stub */
    case NPPM_GETEXTERNALLEXERAUTOINDENTMODE: return 0;
    case NPPM_SETEXTERNALLEXERAUTOINDENTMODE: return 1;
    case NPPM_ADDSCNMODIFIEDFLAGS:          return 1;
    case NPPM_SETPLUGINSUBSCRIPTIONS:       return 1;
    case NPPM_ALLOCATESUPPORTED:            return 1;

    default:
        return 0L;
    }
}

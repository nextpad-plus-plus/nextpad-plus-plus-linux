// Nextpad++ terminal frontend — Phase 1/2 single-buffer editor.
// Scope (docs/tui/implementation-plan.md T1.2–T1.4 + T2.2b): open one file,
// edit with full Scintilla behavior and syntax highlighting, save, status
// row, quit. Since T2.2b, ncurses is RENDER-ONLY: stdin is read raw and
// decoded by tui/input/vtparser (the Fresh port), which enables the kitty
// keyboard protocol, SGR mouse, and bracketed paste (tui/term/modes.h).
// Keys: ^S save · ^Q quit · ^Z undo · ^Y redo · ^G goto line · ^T wrap toggle.
//
// Linux: transcription of the macOS tui/main.mm (docs/12 §8.3). Every
// `NSString *` became `std::string` (empty = nil), Foundation file/encoding/
// preferences/process calls became GLib + the shared src/ modules compiled
// headless into npp_core (encoding.c, langsmgr.c, paths.c, lexer.c, udl.c,
// stylestore.c). Behaviour, the command table, key bindings, menus, help
// text and layouts are the macOS TUI's; `// Linux:` marks each divergence.

#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <curses.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>

#include "Scintilla.h"
#include "SciLexer.h"
#include "ScintillaCurses.h"

#include "chrome/dimming.h"
#include "chrome/menubar.h"
#include "chrome/palette.h"
#include "chrome/panel.h"
#include "chrome/popup.h"
#include "chrome/scrollbar.h"
#include "chrome/statusbar.h"
#include "chrome/tabbar.h"
#include "input/vtparser.h"
#include "scene/scene.h"
#include "shim/funclist.h"
#include "shim/interop.h"
#include "shim/session.h"
#include "shim/theme.h"
#include "shim/workspace.h"

#include <set>
#include "term/colorcaps.h"
#include "term/modes.h"

#include <map>

// Linux: the shared src/ modules (npp_core, -DNPP_TUI headless seam) stand in
// for NppLangsManager / NppBuiltinLanguages / NppPaths and Foundation's
// encodings. C headers without their own extern "C" guards are wrapped.
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
extern "C" {
#include "branding.h"
#include "langsmgr.h"
#include "encoding.h"
#include "paths.h"
#include "prefs.h"
#include "stylestore.h"
}
#include "langcaptions.h"
#include "lexer.h"
#include "udl.h"

// Lexilla is linked statically (same pattern as EditorView.mm).
// Linux: through src/lexilla_bridge.cpp (the GUI's own bridge).
extern "C" void *lexilla_create_lexer(const char *name);

// Linux: config.xml attribute writer (shim/interop.cpp, phase 2a); declared
// here so main.cpp compiles against a tree whose interop.h predates it.
namespace npptui {
bool SetGuiConfigAttr(const std::string &configPath, const std::string &guiConfigName,
                      const std::string &attr, const std::string &value);
}

#define SSM(m, w, l) scintilla_send_message(sci, (m), (uptr_t)(w), (sptr_t)(l))

using npptui::Event;
using npptui::KeyKind;

static void *sci = nullptr;
static WINDOW *statusWin = nullptr;
static WINDOW *scrollWin = nullptr;  // our fresh-style editor scrollbar column
static WINDOW *tabWin = nullptr;     // buffer tab bar (row 1)
static WINDOW *menuWin = nullptr;    // horizontal menu bar (row 0)
static npptui::ScrollbarMouse gSbMouse;
static std::vector<npptui::Rect> gTabSpans;   // clickable tab spans, absolute coords
static std::vector<npptui::Rect> gMenuSpans;  // clickable menu-title spans
static int gMenuActive = -1;  // open menu index while the menu modal runs

// The menu structure (T3.2): titles + the command ids under each. Item
// labels/shortcuts come from the command table, so menus and the palette
// can never drift apart.
struct TuiMenu {
  const char *title;
  std::vector<const char *> commandIds;
};
static const std::vector<TuiMenu> &menuDefinition(void) {
  // "---" = group divider, placed where the GUI's menus draw theirs.
  static const std::vector<TuiMenu> menus = {
      {"File", {"new", "open-file", "reload", "---",
                "save", "save-as", "save-all", "---",
                "close-buffer", "close-all", "---",
                "open-in-gui", "---",
                "quit", "quit-discard"}},
      {"Edit", {"undo", "redo", "---",
                "cut", "copy", "paste", "select-all", "---",
                "duplicate-line", "delete-line", "move-line-up", "move-line-down",
                "join-lines", "trim-trailing", "---",
                "uppercase", "lowercase", "---",
                "indent", "outdent", "---",
                "toggle-comment", "block-comment", "---",
                "eol-crlf", "eol-lf", "eol-cr", "---",
                "copy-path", "copy-filename", "copy-dirpath"}},
      {"Search", {"find", "replace", "find-next", "find-prev", "---",
                  "goto-line", "goto-brace", "---",
                  "toggle-bookmark", "next-bookmark", "prev-bookmark", "clear-bookmarks"}},
      {"View", {"toggle-wrap", "---",
                "toggle-line-numbers", "toggle-whitespace", "toggle-eol-view",
                "toggle-indent-guides", "---",
                "toggle-fold", "fold-all", "unfold-all", "---",
                "toggle-doclist", "toggle-funclist", "toggle-project", "toggle-foldertree",
                "---",
                "summary"}},
      {"Encoding", {"enc-utf8", "enc-utf8-bom", "---",
                    "enc-utf16le", "enc-utf16be", "---",
                    "enc-cp1252"}},
      {"Language", {"language-select"}},
      {"Settings", {"preferences"}},
      {"Buffers", {"buffers-list", "---", "next-buffer", "prev-buffer"}},
      {"Help", {"help"}},
  };
  return menus;
}

// Linux: the on-disk encoding is the src/encoding.c display key WITHOUT its
// BOM variant ("UTF-8", "UTF-16 LE", "Windows-1252", "ISO-8859-1", …); the
// BOM travels separately as hasBOM, exactly as NSStringEncoding + hasBOM did.
typedef std::string TuiEncoding;
static const char *const kEncUtf8 = "UTF-8";
static const char *const kEncUtf16LE = "UTF-16 LE";
static const char *const kEncUtf16BE = "UTF-16 BE";
static const char *const kEncCp1252 = "Windows-1252";

// One open buffer: a Scintilla document plus the view state that does NOT
// live in the document (caret, scroll) and our metadata. The view shows one
// document at a time (SCI_SETDOCPOINTER); the others stay alive via the
// reference we hold from SCI_CREATEDOCUMENT/SCI_ADDREFDOCUMENT.
struct TuiBuffer {
  std::string path;  // absolute, or empty for an unnamed buffer
  std::string lang;  // langs.xml internal name ("udl:Name" for a UDL), or empty = plain text
  std::string backupPath;  // hot-exit snapshot to restore as MODIFIED text
  sptr_t doc = 0;
  bool loaded = false;  // file bytes pulled in (buffers load lazily)
  bool dirtyCache = false;  // last known modify state (exact for the current buffer)
  long caret = 0;
  long firstVisible = 0;
  // On-disk encoding (T3.4): the buffer edits as UTF-8 internally and saves
  // back in this encoding; hasBOM round-trips a UTF-8 BOM.
  TuiEncoding encoding = kEncUtf8;
  bool hasBOM = false;
};
static std::vector<TuiBuffer> gBuffers;
static size_t gCur = 0;
static npptui::TermModes termModes;
static npptui::InputParser gParser;
// Detected once at startup (override: NPP_TUI_COLOR_MODE). The Phase-2 chrome
// renderers key their palette handling off this; Scinterm degrades the editor
// area on its own.
static npptui::ColorCapability gColorCaps = npptui::ColorCapability::Color256;
// The active theme (T2.8): real stylers.xml when available, built-in dark
// defaults otherwise. Every editor and chrome color goes through this.
static npptui::TuiTheme gTheme;

static npptui::PopupTheme currentPopupTheme(void) {
  npptui::PopupTheme t;
  t.bg = gTheme.popupBg;
  t.borderFg = gTheme.popupBorder;
  t.textFg = gTheme.popupText;
  t.selectionBg = gTheme.popupSelBg;
  t.selectionFg = gTheme.popupSelFg;
  t.mutedFg = gTheme.mutedFg;
  t.scrollbar = npptui::ScrollbarColors{gTheme.scrollTrack, gTheme.scrollThumb};
  return t;
}

static npptui::TabBarTheme currentTabTheme(void) {
  npptui::TabBarTheme t;
  t.barBg = gTheme.barBg;
  t.activeBg = gTheme.tabActiveBg;
  t.activeFg = gTheme.tabActiveFg;
  t.inactiveBg = gTheme.tabInactiveBg;
  t.inactiveFg = gTheme.tabInactiveFg;
  return t;
}
static volatile sig_atomic_t gResized = 0;
static std::string filePath;   // absolute path, or empty for an unnamed buffer
static std::string langName;   // langs.xml internal name, or empty = plain text
// On-disk encoding of the CURRENT buffer (mirrors filePath/langName; the
// per-buffer copies live in TuiBuffer). The editor always edits UTF-8.
static TuiEncoding curEncoding = kEncUtf8;
static bool curHasBOM = false;
// Brace-highlight cache (SCN_UPDATEUI skips redundant BRACEHIGHLIGHT calls);
// reset on buffer switches so a stale pair can't suppress the first update.
static long gLastBracePos = -2, gLastMatchPos = -2;

// ── path helpers (Linux: the NSString path category, GLib edition) ───────────

static std::string lastPathComponent(const std::string &p) {
  gchar *b = g_path_get_basename(p.c_str());
  std::string s = b;
  g_free(b);
  return s;
}

static std::string deletingLastPathComponent(const std::string &p) {
  gchar *d = g_path_get_dirname(p.c_str());
  std::string s = d;
  g_free(d);
  return s;
}

static std::string pathExtension(const std::string &p) {
  std::string base = lastPathComponent(p);
  size_t dot = base.rfind('.');
  if (dot == std::string::npos || dot == 0) return "";
  return base.substr(dot + 1);
}

static std::string abbreviatingWithTilde(const std::string &p) {
  const char *home = g_get_home_dir();
  if (home && *home) {
    size_t n = strlen(home);
    if (p.compare(0, n, home) == 0 && (p.size() == n || p[n] == '/'))
      return "~" + p.substr(n);
  }
  return p;
}

// stringByStandardizingPath + the cwd join: one canonical absolute path.
static std::string standardizedAbsolutePath(const std::string &p) {
  gchar *c = g_canonicalize_filename(p.c_str(), nullptr);
  std::string s = c;
  g_free(c);
  return s;
}

static std::string lowercased(const std::string &s) {
  gchar *l = g_utf8_strdown(s.c_str(), -1);
  std::string out = l;
  g_free(l);
  return out;
}

static bool fileExists(const std::string &p) {
  return !p.empty() && g_file_test(p.c_str(), G_FILE_TEST_EXISTS);
}

static std::string formatString(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static std::string formatString(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  gchar *s = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  std::string out = s;
  g_free(s);
  return out;
}

// ── GUI interop state (Phase 4 "companion mode") ──────────────────────────────
// The GUI app and this TUI share the config dir and the defaults domain; a
// 2s mtime watch mirrors the GUI's theme and editor preferences into a
// RUNNING TUI (even over SSH). See shim/interop.h.
// Linux: the "defaults domain" is the GUI's config.xml.
static npptui::GuiEditorPrefs gGuiPrefs;
static npptui::FileWatch *gInteropWatch = nullptr;
static std::string gThemePath;      // the theme XML this session resolved to
static std::string gGuiConfigPath;  // the GUI's config.xml (watch index 1)
static double gNextInteropCheck = 0;

// $NPP_TUI_GUI_DOMAIN overrides the defaults domain (tests must never write
// into the user's real GUI preferences).
// Linux: returns the config.xml PATH. $NPP_TUI_GUI_CONFIG names it outright;
// the macOS-era $NPP_TUI_GUI_DOMAIN (kept so the verbatim e2e batteries stay
// isolated) maps to a scratch tree under the temp dir that never coincides
// with the user's real settings; otherwise the GUI's own config.xml.
static std::string guiDomain(void) {
  const char *e = getenv("NPP_TUI_GUI_CONFIG");
  if (e && e[0]) return e;
  const char *d = getenv("NPP_TUI_GUI_DOMAIN");
  if (d && d[0]) return std::string(g_get_tmp_dir()) + "/nextpad-tui-" + d + "/config.xml";
  gchar *p = npp_user_file(nullptr, "config.xml");
  std::string s = p;
  g_free(p);
  return s;
}

// Linux: the GUI's user data dir the config.xml lives in — where the GUI
// also persists project1.workspace (Project panel) and stylers.xml.
static std::string guiDataDir(void) { return deletingLastPathComponent(guiDomain()); }

static double monotonicSeconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Linux: the few config.xml facts main.cpp reads itself (the editor prefs go
// through shim/interop.h's LoadGuiEditorPrefs): the DarkMode group and the
// Folder-as-Workspace roots (<GUIConfig name="Workspace"><Root path=…/>).
struct GuiConfigFacts {
  bool present = false;
  int darkEnable = -1;   // -1 absent
  int darkAuto = -1;
  std::string theme;     // DarkMode theme= ("" = Default)
  std::vector<std::string> workspaceRoots;
};

struct GuiConfigParseState {
  GuiConfigFacts *out;
  std::string group;
};

static void guiConfigStart(GMarkupParseContext *, const gchar *el, const gchar **names,
                           const gchar **vals, gpointer ud, GError **) {
  GuiConfigParseState *st = (GuiConfigParseState *)ud;
  if (strcmp(el, "GUIConfig") == 0) {
    st->group.clear();
    for (int i = 0; names[i]; i++)
      if (strcmp(names[i], "name") == 0) st->group = vals[i];
    if (st->group == "DarkMode") {
      for (int i = 0; names[i]; i++) {
        auto yes = [](const char *v) { return g_ascii_strcasecmp(v, "yes") == 0 ? 1 : 0; };
        if (strcmp(names[i], "enable") == 0) st->out->darkEnable = yes(vals[i]);
        else if (strcmp(names[i], "darkModeAuto") == 0) st->out->darkAuto = yes(vals[i]);
        else if (strcmp(names[i], "theme") == 0) st->out->theme = vals[i];
      }
    }
    return;
  }
  if (st->group == "Workspace" && strcmp(el, "Root") == 0) {
    for (int i = 0; names[i]; i++)
      if (strcmp(names[i], "path") == 0 && vals[i][0])
        st->out->workspaceRoots.push_back(vals[i]);
  }
}

static void guiConfigEnd(GMarkupParseContext *, const gchar *el, gpointer ud, GError **) {
  GuiConfigParseState *st = (GuiConfigParseState *)ud;
  if (strcmp(el, "GUIConfig") == 0) st->group.clear();
}

static GuiConfigFacts readGuiConfigFacts(void) {
  GuiConfigFacts facts;
  gchar *text = nullptr;
  gsize len = 0;
  std::string path = guiDomain();
  if (!g_file_get_contents(path.c_str(), &text, &len, nullptr)) return facts;
  GuiConfigParseState st{&facts, ""};
  GMarkupParser parser = {guiConfigStart, guiConfigEnd, nullptr, nullptr, nullptr};
  GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, (GMarkupParseFlags)0, &st, nullptr);
  facts.present = g_markup_parse_context_parse(ctx, text, (gssize)len, nullptr) &&
                  g_markup_parse_context_end_parse(ctx, nullptr);
  g_markup_parse_context_free(ctx);
  g_free(text);
  return facts;
}

// Linux: the GUI's effective appearance (src/theme.c): darkModeAuto follows
// the GNOME color-scheme when a session bus is reachable (never over a bare
// SSH login, where GSettings would fall back noisily), else light.
static bool guiAppearanceIsDark(const GuiConfigFacts &f) {
  if (f.darkAuto == 1) {
    if (!getenv("DBUS_SESSION_BUS_ADDRESS")) return false;
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    if (!src) return false;
    GSettingsSchema *schema =
        g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE);
    if (!schema) return false;
    bool dark = false;
    if (g_settings_schema_has_key(schema, "color-scheme")) {
      GSettings *s = g_settings_new("org.gnome.desktop.interface");
      if (s) {
        gchar *v = g_settings_get_string(s, "color-scheme");
        dark = v && strcmp(v, "prefer-dark") == 0;
        g_free(v);
        g_object_unref(s);
      }
    }
    g_settings_schema_unref(schema);
    return dark;
  }
  return f.darkEnable == 1;
}

// Linux: "<preset>[-<suffix>].xml" in the user themes dir, then the bundled
// themes (src/theme.c find_theme_xml). Empty when nothing matches.
static std::string findThemeXml(const std::string &preset, const char *suffix) {
  if (preset.empty()) return "";
  std::string fname = suffix ? preset + "-" + suffix + ".xml" : preset + ".xml";
  std::string user = guiDataDir() + "/themes/" + fname;
  if (fileExists(user)) return user;
  gchar *bundle = npp_bundle_file("themes", fname.c_str());
  std::string b = bundle;
  g_free(bundle);
  if (fileExists(b)) return b;
  return "";
}

static const char *const kDefaultThemeName = "Default";

// The theme file the GUI is ACTUALLY using. stylers.xml is only the
// "Default (stylers.xml)" theme; the Style Configurator persists the active
// theme NAME as NPPActiveTheme in the defaults, and dark mode auto-commits
// "DarkModeDefault" there — so a dark GUI never touches stylers.xml. Named
// themes resolve to <config>/themes/<name>.xml (the GUI copies a bundled
// theme here on first commit), with the app bundle's Resources/themes as
// the fallback for pristine bundled themes. $NPP_TUI_THEME still overrides
// everything (tests).
// Linux: the name is <GUIConfig name="DarkMode" theme=…> in config.xml and
// the resolution is src/theme.c's — "Default" + dark → DarkModeDefault.xml,
// "Default" + light → the user's stylers.xml else the bundled
// stylers.model.xml, a named preset → <preset>-dark|light.xml then
// <preset>.xml (user themes dir, then the bundled themes).
static std::string resolveThemePath(void) {
  const char *envTheme = getenv("NPP_TUI_THEME");
  if (envTheme && envTheme[0]) return envTheme;
  GuiConfigFacts facts = readGuiConfigFacts();
  bool dark = guiAppearanceIsDark(facts);
  std::string preset = facts.theme;
  bool isDefault = preset.empty() || preset == kDefaultThemeName;
  if (!isDefault) {
    std::string p = findThemeXml(preset, dark ? "dark" : "light");
    if (p.empty()) p = findThemeXml(preset, nullptr);
    if (!p.empty()) return p;
  } else if (dark) {
    std::string p = findThemeXml("DarkModeDefault", nullptr);
    if (!p.empty()) return p;
  }
  std::string user = guiDataDir() + "/stylers.xml";
  if (fileExists(user)) return user;
  gchar *model = npp_bundle_file(nullptr, "stylers.model.xml");
  std::string m = model;
  g_free(model);
  return m;
}

// Modal loops must not have refreshAll painted under them by the interop
// tick (the popup would vanish until their next frame); depth-guarded.
static int gModalDepth = 0;
struct ModalScope {
  ModalScope() { ++gModalDepth; }
  ~ModalScope() { --gModalDepth; }
};

static void interopTick(void);  // defined below the theme/prefs appliers

// Bookmark marker number (fold markers own 25–31; 20 is free).
static const int kBookmarkMarker = 20;

// Short display name for the status bar ("utf-8", "iso-8859-1", …).
// Linux: the src/encoding.c display key, lowercased (it IS the IANA name for
// the single-byte sets: "windows-1252", "iso-8859-1"); UTF-16 keeps the
// macOS spelling ("utf-16le"/"utf-16be") the status bar always showed.
static std::string encodingDisplayName(const TuiEncoding &enc, bool hasBOM) {
  if (enc == kEncUtf8) return hasBOM ? "utf-8-bom" : "utf-8";
  if (enc == kEncUtf16LE) return "utf-16le";
  if (enc == kEncUtf16BE) return "utf-16be";
  for (int i = 0; i < npp_encoding_count; i++)
    if (enc == npp_encodings[i].display) return lowercased(enc);
  return "?";
}

// ── terminal lifecycle ────────────────────────────────────────────────────────

static void restoreTerminal(void) {
  if (!isendwin()) {
    npptui::TermModesUndo(termModes, STDOUT_FILENO);
    endwin();
  }
}

static void onFatalSignal(int) {
  // Async-signal-safe path: untracked disables first, then leave curses.
  npptui::TermModesEmergencyCleanup(STDOUT_FILENO);
  if (!isendwin()) endwin();
  _exit(1);
}

static void onWinch(int) { gResized = 1; }

// ── input pump (T2.2b) ────────────────────────────────────────────────────────

// All decoded events flow through one queue that every consumer — the main
// loop AND the modal prompts — pops one event at a time. A prompt opened by
// event N must see events N+1… of the same read chunk (a `^G 150\r ^Q`
// burst arrives as one read); draining the pty into per-consumer batches
// instead deadlocked the prompt against events stranded in the outer batch.
static std::deque<Event> gQueue;
static const int kEscTimeoutMs = 50;

// One poll+read step. Blocks while the parser has no ambiguous escape prefix;
// otherwise waits kEscTimeoutMs and, on timeout, flushes the prefix as the
// key a legacy terminal means by it (Esc / Alt+…). Returns false when the
// terminal is gone (EOF or read error).
static bool pumpOnce(void) {
  struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
  // No longer blocks forever when idle: the 2s ceiling is the interop
  // heartbeat (nextEvent runs interopTick between pumps).
  int timeout = gParser.escapePending() ? kEscTimeoutMs : 2000;
  int r = poll(&pfd, 1, timeout);
  if (r < 0) return errno == EINTR;  // a signal (e.g. SIGWINCH); caller re-checks
  if (r == 0) {
    for (Event &e : gParser.flush()) gQueue.push_back(std::move(e));
    return true;
  }
  uint8_t buf[4096];
  ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
  if (n <= 0) return false;  // pty gone
  for (Event &e : gParser.parse(buf, (size_t)n)) gQueue.push_back(std::move(e));
  return true;
}

static void handleResize(void);
static void refreshAll(const std::string &transient);

// Pops the next event, refilling the queue (and servicing resizes) as needed.
// Returns false when the terminal is gone.
static bool nextEvent(Event &out) {
  while (gQueue.empty()) {
    if (gResized) {
      gResized = 0;
      handleResize();
      refreshAll("");
    }
    interopTick();
    if (!pumpOnce()) return false;
  }
  out = std::move(gQueue.front());
  gQueue.pop_front();
  return true;
}

// ── layout / resize ───────────────────────────────────────────────────────────

// ── docked side panel (Phase 4.5) ─────────────────────────────────────────────
// One left-dock pane hosting one of the panels at a time.
enum class PanelKind { DocList, Functions, Project, FolderTree };
static WINDOW *panelWin = nullptr;
static bool gPanelVisible = false;
static bool gPanelFocused = false;
static PanelKind gPanelKind = PanelKind::DocList;
static npptui::ScenePanel gPanel;
static void panelActivate(size_t idx);  // per-kind Enter/click behavior (below)

// Function List cache: rescan only when the document or its text changed.
static long gTextVersion = 0;  // bumped by SCN_MODIFIED insert/delete
static std::vector<npptui::FuncItem> gFuncItems;
static bool gFuncHadParser = false;
static sptr_t gFuncDoc = 0;
static long gFuncVersion = -1;

// Project (workspace) state: tree + collapsed-folder keys + row references
// built alongside the visible rows each repaint.
static npptui::WsNode gWsRoot;
static bool gWsLoaded = false;
static std::string gWsPath;
static std::set<std::string> gWsCollapsed;
struct WsRowRef {
  bool isFile = false;
  std::string pathOrKey;  // files: absolute path; folders: collapse key
};
static std::vector<WsRowRef> gWsRowRefs;

// Folder as Workspace: live filesystem roots (GUI's FolderTreePanelRoots),
// lazily listed per directory; expanding a directory re-reads it.
static std::vector<std::string> gFtRoots;
static std::set<std::string> gFtExpanded;                            // dir paths
static std::map<std::string, std::vector<npptui::WsNode>> gFtCache;  // dir → children
static std::vector<WsRowRef> gFtRowRefs;

static int panelWidth(void) {
  int w = 30;
  if (w > COLS / 3) w = COLS / 3;
  return w < 16 ? 16 : w;
}

static void layoutWindows(void) {
  // Row 0: menu bar. Row 1: tab bar. Rows 2..LINES-2: [panel |] editor +
  // our scrollbar column (the rightmost; Scinterm's own bar is off).
  // Row LINES-1: status.
  int editorRows = LINES > 3 ? LINES - 3 : 1;
  int panelW = gPanelVisible ? panelWidth() : 0;
  WINDOW *win = scintilla_get_window(sci);
  int editorCols = COLS - 1 - panelW;
  wresize(win, editorRows, editorCols > 0 ? editorCols : 1);
  mvwin(win, 2, panelW);
  if (gPanelVisible) {
    if (!panelWin) panelWin = newwin(editorRows, panelW, 2, 0);
    else {
      wresize(panelWin, editorRows, panelW);
      mvwin(panelWin, 2, 0);
    }
  } else if (panelWin) {
    delwin(panelWin);
    panelWin = nullptr;
  }
  if (menuWin) {
    wresize(menuWin, 1, COLS);
    mvwin(menuWin, 0, 0);
  }
  if (tabWin) {
    wresize(tabWin, 1, COLS);
    mvwin(tabWin, 1, 0);
  }
  if (statusWin) {
    wresize(statusWin, 1, COLS);
    mvwin(statusWin, LINES - 1, 0);
  }
  if (scrollWin) {
    wresize(scrollWin, editorRows, 1);
    mvwin(scrollWin, 2, COLS - 1);
  }
}

static void handleResize(void) {
  // ncurses no longer sees KEY_RESIZE (it never reads input); take the new
  // size from the tty and resize curses ourselves.
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1 && ws.ws_col > 0)
    resizeterm(ws.ws_row, ws.ws_col);
  layoutWindows();
}

// ── status row (Scene-projected since T2.4) ───────────────────────────────────

// The chrome renderers draw onto this curses-backed row.
struct CursesRowSurface : npptui::RowSurface {
  WINDOW *win;
  explicit CursesRowSurface(WINDOW *w) : win(w) {}
  int width() const override { return COLS; }
  void putText(int x, const std::string &utf8) override {
    mvwaddstr(win, 0, x, utf8.c_str());
  }
};

static npptui::SceneStatus currentStatusScene(const std::string &transient) {
  npptui::SceneStatus s;
  long pos = SSM(SCI_GETCURRENTPOS, 0, 0);
  s.line = SSM(SCI_LINEFROMPOSITION, pos, 0) + 1;
  s.column = SSM(SCI_GETCOLUMN, pos, 0) + 1;
  s.dirty = SSM(SCI_GETMODIFY, 0, 0) != 0;
  s.fileName = !filePath.empty() ? lastPathComponent(filePath) : "(untitled)";
  s.language = !langName.empty() ? langName : "text";
  if (curEncoding != kEncUtf8 || curHasBOM)
    s.encoding = encodingDisplayName(curEncoding, curHasBOM);
  s.transient = transient;
  return s;
}

// Reverse-video row shared by the status bar and the prompts.
template <typename RenderFn>
static void drawRow(RenderFn render) {
  if (!statusWin) return;
  werase(statusWin);
  wattron(statusWin, A_REVERSE);
  mvwhline(statusWin, 0, 0, ' ', COLS);
  CursesRowSurface surface(statusWin);
  render(surface);
  wattroff(statusWin, A_REVERSE);
}

static void drawStatus(const std::string &transient) {
  npptui::SceneStatus s = currentStatusScene(transient);
  drawRow([&](npptui::RowSurface &out) { npptui::RenderStatusRow(s, out); });
  wnoutrefresh(statusWin);
}

// Scroll state of the editor in display-line space (wrap-aware).
static npptui::ScrollbarState editorScrollState(void) {
  long total = SSM(SCI_VISIBLEFROMDOCLINE, SSM(SCI_GETLINECOUNT, 0, 0), 0);
  long visible = SSM(SCI_LINESONSCREEN, 0, 0);
  long first = SSM(SCI_GETFIRSTVISIBLELINE, 0, 0);
  return npptui::ScrollbarState((size_t)(total > 0 ? total : 0),
                                (size_t)(visible > 0 ? visible : 0),
                                (size_t)(first > 0 ? first : 0));
}

static npptui::Rect editorScrollTrack(void) {
  return npptui::Rect{COLS - 1, 2, 1, LINES > 3 ? LINES - 3 : 1};
}

// Fresh-style scrollbar + tab/menu bars (defined below CursesCellSurface).
static void drawScrollbar(void);
static void drawTabBar(void);
static void drawMenuBar(void);

// The line-number margin grows with the document (a fixed width truncated
// 6+-digit numbers past 100k lines): digits of the last line number plus a
// padding cell, floor of 5 so small files keep the familiar gutter.
static bool gShowLineNumbers = true;  // View → Show Line Numbers

static void updateLineNumberMargin(void) {
  static long lastWidth = 0;
  long width = 0;
  if (gShowLineNumbers) {
    long lines = SSM(SCI_GETLINECOUNT, 0, 0);
    int digits = 1;
    while (lines >= 10) {
      lines /= 10;
      digits++;
    }
    width = digits + 2 > 5 ? digits + 2 : 5;
  }
  if (width != lastWidth) {
    SSM(SCI_SETMARGINWIDTHN, 0, width);
    lastWidth = width;
  }
}

static void drawPanel(void);  // defined below CursesCellSurface

static void refreshAll(const std::string &transient) {
  updateLineNumberMargin();
  drawMenuBar();
  drawTabBar();
  drawStatus(transient);
  drawScrollbar();
  drawPanel();
  scintilla_noutrefresh(sci);
  // Scinterm's UpdateCursor wmove()s the caret AFTER its own wnoutrefresh,
  // so the moved cursor never reaches the virtual screen on its own — one
  // more wnoutrefresh propagates it, which is what parks the terminal's
  // hardware cursor on the caret.
  wnoutrefresh(scintilla_get_window(sci));
  doupdate();
}

// ── prompts (parser-driven since T2.2b; Scene-projected since T2.4) ───────────

static void drawPrompt(const npptui::ScenePrompt &p) {
  drawRow([&](npptui::RowSurface &out) { npptui::RenderPromptRow(p, out); });
  wrefresh(statusWin);
}

static void utf8Append(std::string &s, char32_t cp);  // find/replace section

// Single-key prompt on the status row. Returns the character (27 for Esc,
// '\r' for Enter, 8 for Backspace) — or 27 if the terminal is gone.
[[maybe_unused]] static char32_t promptKey(const std::string &label) {
  drawPrompt(npptui::ScenePrompt{label, ""});
  for (;;) {
    Event e;
    if (!nextEvent(e)) return 27;
    if (e.type != Event::Type::Key) continue;
    const npptui::KeyEvent &k = e.key.event;
    if (k.kind == npptui::KeyEventKind::Release) continue;
    switch (k.code.kind) {
      case KeyKind::Esc: return 27;
      case KeyKind::Enter: return '\r';
      case KeyKind::Backspace: return 8;
      case KeyKind::Char: return k.code.ch;
      default: return 0;  // any other key cancels
    }
  }
}

// Blocking one-line prompt on the status row. Returns nil on Esc.
// Linux: an empty string stands in for nil (every caller tests `.length`).
static std::string promptStatus(const std::string &label) {
  ModalScope modalScope;
  std::string input;
  for (;;) {
    drawPrompt(npptui::ScenePrompt{label, input});
    Event e;
    if (!nextEvent(e)) return "";
    if (e.type == Event::Type::Paste) {
      if (g_utf8_validate(e.paste.data(), (gssize)e.paste.size(), nullptr))
        input.append(e.paste.data(), e.paste.size());
      continue;
    }
    if (e.type != Event::Type::Key) continue;
    const npptui::KeyEvent &k = e.key.event;
    if (k.kind == npptui::KeyEventKind::Release) continue;
    if (k.code.kind == KeyKind::Esc) return "";
    if (k.code.kind == KeyKind::Enter) return input;
    if (k.code.kind == KeyKind::Backspace) {
      // Drop the last code point, not the last byte.
      while (!input.empty()) {
        unsigned char last = (unsigned char)input.back();
        input.pop_back();
        if (last < 0x80 || last >= 0xC0) break;
      }
    } else if (k.code.kind == KeyKind::Char && k.code.ch >= 32 && k.code.ch <= 0xFFFF &&
               !(k.mods & (npptui::kModCtrl | npptui::kModAlt))) {
      utf8Append(input, k.code.ch);
    }
  }
}

// ── chrome color pairs (T2.5b) ────────────────────────────────────────────────

// Curses pair/color slots for chrome colors, allocated from the HIGH end of
// both spaces: Scinterm 5.5 allocates its own colors/pairs from the low end
// with no reservation API (that arrives in Scinterm 6.0 as
// scintilla_set_color_offsets — revisit at the upgrade). Probe results:
// xterm-256color grants 256 colors / 32767 pairs / can_change_color.
static short chromeColorSlot(const npptui::Color &c) {
  using npptui::Color;
  switch (c.kind) {
    case Color::Kind::Default: return -1;  // use_default_colors
    case Color::Kind::Named:
    case Color::Kind::Indexed: return (short)c.idx;
    case Color::Kind::Rgb: break;
  }
  static std::map<uint32_t, short> slots;
  static short nextSlot = 0;
  if (nextSlot == 0) nextSlot = (short)(COLORS > 64 ? COLORS - 1 : 63);
  uint32_t key = ((uint32_t)c.rgb.r << 16) | ((uint32_t)c.rgb.g << 8) | c.rgb.b;
  auto it = slots.find(key);
  if (it != slots.end()) return it->second;
  if (!can_change_color() || nextSlot <= 200)
    return (short)npptui::RgbTo256(c.rgb.r, c.rgb.g, c.rgb.b);
  short slot = nextSlot--;
  init_color(slot, (short)(c.rgb.r * 1000 / 255), (short)(c.rgb.g * 1000 / 255),
             (short)(c.rgb.b * 1000 / 255));
  slots[key] = slot;
  return slot;
}

static short chromePairFor(const npptui::CellStyle &style) {
  static std::map<uint32_t, short> pairs;
  static short nextPair = 0;
  // Pair numbers are capped at 255: Apple's ncurses is an ABI-5 build whose
  // classic attribute encoding holds 8 bits of pair number, so pairs above
  // 255 silently truncate (the "invisible dialog" bug). Scinterm allocates
  // pairs from 1 upward and uses a few dozen; the high end of 0..255 is ours.
  // (Linux ncurses 6 is ABI-6 with wide pairs, but the cap costs nothing and
  // keeps one allocation policy across both ports.)
  if (nextPair == 0) nextPair = (short)(COLOR_PAIRS > 256 ? 255 : COLOR_PAIRS - 1);
  short fg = chromeColorSlot(style.fg);
  short bg = chromeColorSlot(style.bg);
  uint32_t key = ((uint32_t)(uint16_t)fg << 16) | (uint16_t)bg;
  auto it = pairs.find(key);
  if (it != pairs.end()) return it->second;
  short pair = nextPair--;
  init_pair(pair, fg, bg);
  pairs[key] = pair;
  return pair;
}

// Chrome renderers paint popups onto this window-backed surface. put()
// receives ABSOLUTE screen coordinates; the surface subtracts its origin.
struct CursesCellSurface : npptui::CellSurface {
  WINDOW *win;
  int ox, oy;
  CursesCellSurface(WINDOW *w, int originX, int originY) : win(w), ox(originX), oy(originY) {}
  int width() const override { return COLS; }
  int height() const override { return LINES; }
  void put(int x, int y, const std::string &utf8, const npptui::CellStyle &style) override {
    attr_t attrs = A_NORMAL;
    if (style.bold) attrs |= A_BOLD;
    if (style.dim) attrs |= A_DIM;
    if (style.underline) attrs |= A_UNDERLINE;
    wattr_set(win, attrs, chromePairFor(style), nullptr);
    mvwaddstr(win, y - oy, x - ox, utf8.c_str());
  }
};

// ── panel content builders (one per kind) ─────────────────────────────────────

static void buildDocListRows(void) {
  gPanel.title = "Document List";
  gPanel.rows.clear();
  for (size_t i = 0; i < gBuffers.size(); i++) {
    const TuiBuffer &b = gBuffers[i];
    npptui::PanelRow row;
    row.text = !b.path.empty() ? lastPathComponent(b.path) : "(untitled)";
    row.dirty = i == gCur ? SSM(SCI_GETMODIFY, 0, 0) != 0
                          : (b.dirtyCache || (!b.loaded && !b.backupPath.empty()));
    if (!b.path.empty()) row.detail = abbreviatingWithTilde(deletingLastPathComponent(b.path));
    gPanel.rows.push_back(std::move(row));
  }
  // The selection tracks the active buffer while the panel is unfocused.
  if (!gPanelFocused) gPanel.selected = gCur;
}

// Linux: the functionList rules are keyed by the langs.xml name; a UDL's
// rules file carries the UDL's own name (no "udl:" prefix).
static std::string funclistLangName(void) {
  if (langName.compare(0, 4, "udl:") == 0) return langName.substr(4);
  return langName;
}

static void buildFunctionRows(void) {
  gPanel.title = "Function List";
  sptr_t doc = gBuffers.empty() ? 0 : gBuffers[gCur].doc;
  if (doc != gFuncDoc || gTextVersion != gFuncVersion) {
    gFuncDoc = doc;
    gFuncVersion = gTextVersion;
    gFuncItems.clear();
    gFuncHadParser = false;
    long len = SSM(SCI_GETLENGTH, 0, 0);
    // ICU regex over the whole document: cap like the GUI's large-file
    // guards rather than stall the UI on huge buffers.
    // Linux: GRegex (PCRE) — same cap.
    if (len <= 4 * 1024 * 1024) {
      const char *buf = (const char *)SSM(SCI_GETCHARACTERPOINTER, 0, 0);
      std::string text = len > 0 ? std::string(buf, (size_t)len) : std::string();
      if (g_utf8_validate(text.data(), (gssize)text.size(), nullptr))
        gFuncHadParser = npptui::ScanFunctions(text, funclistLangName(), gFuncItems);
    }
  }
  gPanel.rows.clear();
  if (langName.empty() || !gFuncHadParser) {
    npptui::PanelRow row;
    row.text = "(no parser for this language)";
    gPanel.rows.push_back(std::move(row));
  } else if (gFuncItems.empty()) {
    npptui::PanelRow row;
    row.text = "(no functions found)";
    gPanel.rows.push_back(std::move(row));
  } else {
    for (const npptui::FuncItem &it : gFuncItems) {
      npptui::PanelRow row;
      row.text = std::string((size_t)it.depth * 2, ' ') + it.name;
      row.detail = std::to_string(it.line);
      gPanel.rows.push_back(std::move(row));
    }
  }
}

static void appendWsRows(const npptui::WsNode &node, const std::string &key, int depth) {
  for (const npptui::WsNode &child : node.children) {
    std::string ckey = key + "/" + child.name;
    std::string indent((size_t)depth * 2, ' ');
    npptui::PanelRow row;
    if (child.isFile) {
      row.text = indent + child.name;
      gWsRowRefs.push_back({true, child.path});
      gPanel.rows.push_back(std::move(row));
    } else {
      bool collapsed = gWsCollapsed.count(ckey) > 0;
      row.text = indent + (collapsed ? "▸ " : "▾ ") + child.name;
      gWsRowRefs.push_back({false, ckey});
      gPanel.rows.push_back(std::move(row));
      if (!collapsed) appendWsRows(child, ckey, depth + 1);
    }
  }
}

static void buildProjectRows(void) {
  gPanel.title = gWsLoaded ? "Project — " + gWsRoot.name : std::string("Project");
  gPanel.rows.clear();
  gWsRowRefs.clear();
  if (!gWsLoaded) {
    npptui::PanelRow row;
    row.text = "(no workspace — press Enter)";
    gWsRowRefs.push_back({false, ""});
    gPanel.rows.push_back(std::move(row));
    return;
  }
  appendWsRows(gWsRoot, "", 0);
}

static void appendFolderRows(const std::string &dirPath, int depth) {
  auto it = gFtCache.find(dirPath);
  if (it == gFtCache.end()) return;
  for (const npptui::WsNode &child : it->second) {
    std::string indent((size_t)depth * 2, ' ');
    npptui::PanelRow row;
    if (child.isFile) {
      row.text = indent + child.name;
      gFtRowRefs.push_back({true, child.path});
      gPanel.rows.push_back(std::move(row));
    } else {
      bool expanded = gFtExpanded.count(child.path) > 0;
      row.text = indent + (expanded ? "▾ " : "▸ ") + child.name;
      gFtRowRefs.push_back({false, child.path});
      gPanel.rows.push_back(std::move(row));
      if (expanded) appendFolderRows(child.path, depth + 1);
    }
  }
}

static void buildFolderTreeRows(void) {
  gPanel.title = "Folder as Workspace";
  gPanel.rows.clear();
  gFtRowRefs.clear();
  if (gFtRoots.empty()) {
    npptui::PanelRow row;
    row.text = "(no folder — press Enter)";
    gFtRowRefs.push_back({false, ""});
    gPanel.rows.push_back(std::move(row));
    return;
  }
  for (const std::string &root : gFtRoots) {
    bool expanded = gFtExpanded.count(root) > 0;
    npptui::PanelRow row;
    std::string name = lastPathComponent(root);
    row.text = std::string(expanded ? "▾ " : "▸ ") + name;
    gFtRowRefs.push_back({false, root});
    gPanel.rows.push_back(std::move(row));
    if (expanded) appendFolderRows(root, 1);
  }
}

// Rebuilds the dock's rows from the live state (cached scans no-op when
// nothing changed). Called by every paint AND, on Linux, before a focused
// dock handles a navigation key: when the palette's Enter that shows the
// panel, F6, Down and Enter arrive in ONE read (an e2e battery or a fast
// paste), the main loop drains them all before the first repaint, so the
// rows — and the Function List's item cache — did not exist yet and
// Down/Enter silently acted on an empty list. Same latent race in the macOS
// main.mm (it wins on pty timing there).
static void buildPanelRows(void) {
  switch (gPanelKind) {
    case PanelKind::DocList: buildDocListRows(); break;
    case PanelKind::Functions: buildFunctionRows(); break;
    case PanelKind::Project: buildProjectRows(); break;
    case PanelKind::FolderTree: buildFolderTreeRows(); break;
  }
  if (gPanel.selected >= gPanel.rows.size())
    gPanel.selected = gPanel.rows.empty() ? 0 : gPanel.rows.size() - 1;
}

// Paints the dock, rebuilding its rows from the live state each frame.
static void drawPanel(void) {
  if (!gPanelVisible || !panelWin) return;
  buildPanelRows();
  gPanel.focused = gPanelFocused;
  int rows = 0, cols = 0;
  getmaxyx(panelWin, rows, cols);
  gPanel.clampScroll(rows);
  werase(panelWin);
  CursesCellSurface surface(panelWin, 0, 2);
  npptui::PanelTheme theme;
  theme.bg = gTheme.popupBg;
  theme.fg = gTheme.popupText;
  theme.titleBg = gTheme.tabInactiveBg;
  theme.titleFg = gTheme.tabActiveFg;
  theme.selBg = gTheme.popupSelBg;
  theme.selFg = gTheme.popupSelFg;
  theme.mutedFg = gTheme.mutedFg;
  theme.borderFg = gTheme.popupBorder;
  npptui::RenderPanel(gPanel, npptui::Rect{0, 2, cols, rows}, surface, theme);
  wnoutrefresh(panelWin);
}

// The dim-behind pass, curses edition. Numeric dimming exactly like Fresh's
// (each pair's colors divided by 3 into a cached dimmed pair): Terminal.app
// does not render the faint attribute on 256-color text, so an A_DIM-only
// pass looked like no dimming at all. Components that are the terminal
// default have no honest RGB to divide and take A_DIM instead (the
// tui/chrome/dimming.h rule). Safe to do destructively: Scinterm repaints
// its window in full on every refresh.
static void dimWindow(WINDOW *win) {
  static std::map<short, std::pair<short, bool>> dimPairs;  // pair → (dimmed pair, needsDimAttr)
  int rows = 0, cols = 0;
  getmaxyx(win, rows, cols);
  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < cols; x++) {
      cchar_t cc;
      if (mvwin_wch(win, y, x, &cc) == ERR) continue;
      attr_t attrs = 0;
      short pair = 0;
      wchar_t wch[CCHARW_MAX];
      if (getcchar(&cc, wch, &attrs, &pair, nullptr) == ERR) continue;

      auto it = dimPairs.find(pair);
      if (it == dimPairs.end()) {
        short f = -1, b = -1;
        pair_content(pair, &f, &b);
        auto dimOf = [](short color) {
          if (color < 0) return npptui::Color::def();
          short r = 0, g = 0, bl = 0;
          color_content(color, &r, &g, &bl);  // current definition, 0-1000
          return npptui::Color::fromRgb((uint8_t)(r * 255 / 1000 / 3),
                                        (uint8_t)(g * 255 / 1000 / 3),
                                        (uint8_t)(bl * 255 / 1000 / 3));
        };
        npptui::CellStyle dimmed;
        dimmed.fg = dimOf(f);
        dimmed.bg = dimOf(b);
        it = dimPairs.emplace(pair, std::make_pair(chromePairFor(dimmed), f < 0 || b < 0))
                 .first;
      }
      attr_t newAttrs = (attrs & ~(attr_t)A_COLOR) | (it->second.second ? A_DIM : 0);
      mvwchgat(win, y, x, 1, newAttrs, it->second.first, nullptr);
    }
  }
}

// Fresh-style scrollbar: solid thumb on a subtle track, invisible when the
// whole document fits on screen.
static void drawScrollbar(void) {
  if (!scrollWin) return;
  npptui::ScrollbarState state = editorScrollState();
  CursesCellSurface surface(scrollWin, COLS - 1, 1);
  npptui::Rect track = editorScrollTrack();
  if (state.totalItems <= state.visibleItems) {
    npptui::CellStyle bg;
    bg.bg = gTheme.editorBg;
    surface.fill(track, " ", bg);
  } else {
    npptui::RenderScrollbar(surface, track, state,
                            npptui::ScrollbarColors{gTheme.scrollTrack, gTheme.scrollThumb});
  }
  wnoutrefresh(scrollWin);
}

// The buffer tab bar on row 0, projected through SceneTabs; spans cached for
// click routing.
static void drawTabBar(void) {
  if (!tabWin) return;
  npptui::SceneTabs tabs;
  for (size_t i = 0; i < gBuffers.size(); i++) {
    npptui::SceneTabs::Tab t;
    t.title = !gBuffers[i].path.empty() ? lastPathComponent(gBuffers[i].path) : "(untitled)";
    t.dirty = i == gCur ? SSM(SCI_GETMODIFY, 0, 0) != 0 : gBuffers[i].dirtyCache;
    t.active = i == gCur;
    tabs.tabs.push_back(std::move(t));
  }
  CursesCellSurface surface(tabWin, 0, 0);
  gTabSpans.clear();
  npptui::RenderTabBar(tabs, 0, surface, gTabSpans, currentTabTheme());
  // Spans are window-relative row 0; clicks arrive with absolute rows.
  for (npptui::Rect &s : gTabSpans) s.y = 1;
  wnoutrefresh(tabWin);
}

static void drawMenuBar(void) {
  if (!menuWin) return;
  npptui::SceneMenuBar bar;
  for (const TuiMenu &m : menuDefinition()) bar.titles.push_back(m.title);
  bar.active = gMenuActive;
  npptui::MenuBarTheme theme;
  theme.barBg = gTheme.popupBg;
  theme.fg = gTheme.popupText;
  theme.activeBg = gTheme.popupSelBg;
  theme.activeFg = gTheme.popupSelFg;
  CursesCellSurface surface(menuWin, 0, 0);
  gMenuSpans.clear();
  npptui::RenderMenuBar(bar, 0, surface, gMenuSpans, theme);
  wnoutrefresh(menuWin);
}

// ── styling (T1.3 generic palette; themed since T2.8) ─────────────────────────

// Fallback palette for languages the theme file doesn't cover (0xBBGGRR).
enum : long {
  kComment = 0x55996A, kKeyword = 0xD69C56, kString = 0x7891CE,
  kNumber = 0xA8CEB5, kPreproc = 0xC086C5, kError = 0x4040C0,
};


// Generic dark theme: ask the lexer for its style names (SCI_NAMEOFSTYLE) and
// color by name fragment — works for every Lexilla lexer without per-language
// tables. The full theme shim replaces this at T2.8.
static void applyStyleForName(int style, const char *name) {
  std::string n = lowercased(name);
  auto contains = [&](const char *frag) { return n.find(frag) != std::string::npos; };
  long fore = -1;
  if (contains("comment")) fore = kComment;
  else if (contains("string") || contains("character") ||
           contains("triple") || contains("backticks") ||
           contains("here")) fore = kString;
  else if (contains("number")) fore = kNumber;
  else if (contains("preprocessor")) fore = kPreproc;
  else if (contains("error")) fore = kError;
  else if (contains("word") || contains("keyword")) fore = kKeyword;
  else if (contains("regex") || contains("verbatim")) fore = kString;
  if (fore >= 0) SSM(SCI_STYLESETFORE, style, fore);
}

// Linux: the chrome styles STYLECLEARALL wipes (line numbers, brace
// highlight) — split out so the UDL path, whose udl_apply runs its own
// STYLECLEARALL, can re-assert them without repeating the base pass.
static void applyChromeStyles(void) {
  SSM(SCI_STYLESETFORE, STYLE_LINENUMBER, gTheme.lineNrFg);
  SSM(SCI_STYLESETBACK, STYLE_LINENUMBER, gTheme.lineNrBg);
  // Brace matching (GUI parity: "Brace highlight style" / "Bad brace
  // colour"); the SCN_UPDATEUI handler drives BRACEHIGHLIGHT.
  SSM(SCI_STYLESETFORE, STYLE_BRACELIGHT, gTheme.braceFg);
  SSM(SCI_STYLESETBACK, STYLE_BRACELIGHT,
      gTheme.braceBg >= 0 ? gTheme.braceBg : gTheme.defaultBg);
  SSM(SCI_STYLESETBOLD, STYLE_BRACELIGHT, gTheme.braceBold);
  SSM(SCI_STYLESETFORE, STYLE_BRACEBAD, gTheme.badBraceFg);
  SSM(SCI_STYLESETBACK, STYLE_BRACEBAD,
      gTheme.badBraceBg >= 0 ? gTheme.badBraceBg : gTheme.defaultBg);
}

static void applyBaseTheme(void) {
  SSM(SCI_STYLESETFORE, STYLE_DEFAULT, gTheme.defaultFg);
  SSM(SCI_STYLESETBACK, STYLE_DEFAULT, gTheme.defaultBg);
  SSM(SCI_STYLECLEARALL, 0, 0);
  applyChromeStyles();
  SSM(SCI_SETCARETLINEVISIBLE, 1, 0);
  SSM(SCI_SETCARETLINEBACK, gTheme.caretLineBg, 0);
  SSM(SCI_SETSELBACK, 1, gTheme.selectionBg);
  SSM(SCI_SETCARETFORE, gTheme.caretFg, 0);
  // CARETSTYLE_CURSES lets Scinterm drive the terminal's own cursor
  // (curs_set on/off as the caret scrolls in and out of view).
  SSM(SCI_SETCARETSTYLE, CARETSTYLE_CURSES, 0);
  // Fold chrome from the theme, mirroring the GUI's mapping (EditorView
  // applyThemeColors): "Fold" fg/bg → marker fore/back (Scinterm: fore =
  // glyph color, back = cell bg), "Fold margin" bg → the margin column,
  // "Fold active" fg → the highlighted marker (bold in curses).
  SSM(SCI_SETFOLDMARGINCOLOUR, 1, gTheme.foldMarginBg);
  SSM(SCI_SETFOLDMARGINHICOLOUR, 1, gTheme.foldMarginBg);
  static const int kFoldMarkers[] = {
      SC_MARKNUM_FOLDER,    SC_MARKNUM_FOLDEROPEN,    SC_MARKNUM_FOLDEREND,
      SC_MARKNUM_FOLDEROPENMID, SC_MARKNUM_FOLDERSUB, SC_MARKNUM_FOLDERTAIL,
      SC_MARKNUM_FOLDERMIDTAIL,
  };
  for (int marker : kFoldMarkers) {
    SSM(SCI_MARKERSETFORE, marker, gTheme.foldFg);
    SSM(SCI_MARKERSETBACK, marker, gTheme.foldBg);
    SSM(SCI_MARKERSETBACKSELECTED, marker, gTheme.foldActiveFg);
  }
  SSM(SCI_MARKERENABLEHIGHLIGHT, 1, 0);
  // Bookmark flag (⚑ in the spacer margin) takes the "Fold active" accent.
  SSM(SCI_MARKERSETFORE, kBookmarkMarker, gTheme.foldActiveFg);
  SSM(SCI_MARKERSETBACK, kBookmarkMarker, gTheme.foldMarginBg);
}

// Generic fallback for languages the theme file doesn't cover: color by the
// lexer's own style names.
static void applyGenericLexerStyles(void) {
  char name[128];
  for (int style = 0; style < 256; style++) {
    name[0] = '\0';
    long len = SSM(SCI_NAMEOFSTYLE, style, name);
    if (len > 0 && name[0]) applyStyleForName(style, name);
  }
}

static bool isUdlLang(const std::string &lang) { return lang.compare(0, 4, "udl:") == 0; }

static void applyLexerStyles(void) {
  if (!langName.empty()) {
    // Linux: a UDL carries its own colours (udl_apply set them); the theme's
    // lexer blocks and the name-based fallback are for built-in lexers only.
    if (isUdlLang(langName)) return;
    auto it = gTheme.lexerStyles.find(langName);
    if (it != gTheme.lexerStyles.end()) {
      for (const npptui::ThemeLexerStyle &s : it->second) {
        if (s.styleID < 0 || s.styleID > 255) continue;
        if (s.fg >= 0) SSM(SCI_STYLESETFORE, s.styleID, s.fg);
        if (s.bg >= 0) SSM(SCI_STYLESETBACK, s.styleID, s.bg);
        if (s.bold) SSM(SCI_STYLESETBOLD, s.styleID, 1);
        if (s.italic) SSM(SCI_STYLESETITALIC, s.styleID, 1);
        if (s.underline) SSM(SCI_STYLESETUNDERLINE, s.styleID, 1);
      }
      return;
    }
  }
  applyGenericLexerStyles();
}

static void applyGuiPrefsToDoc(void);  // defined with the interop appliers below

// Linux: udl_apply's Default Style / Global override come from
// src/stylestore.c, so the store follows the theme file this session
// resolved to (the GUI's own pipeline); missing file → the GUI's default
// resolution (user stylers.xml, else the bundled model).
static void syncStylestoreToTheme(void) {
  stylestore_load_theme(fileExists(gThemePath) ? gThemePath.c_str() : nullptr);
  // udl.c blends UDL token backgrounds toward the editor's when the
  // appearance is dark — tell the prefs stub which one the theme is.
  g_prefs.appearance = gTheme.dark ? APPEAR_DARK : APPEAR_LIGHT;
}

// Apply a language (langs.xml internal name, nil = plain text) to the
// CURRENT document: lexer, keywords, fold property, styles, and the GUI's
// per-language tab overrides. Language menu and extension detection both
// land here.
// Linux: "udl:<Name>" applies a User Defined Language through src/udl.c —
// the macOS TUI has no UDL support.
static void applyLanguage(const std::string &lang) {
  langName.clear();
  void *lexer = nullptr;
  if (isUdlLang(lang)) {
    udl_load_all();
    int idx = udl_find_by_name(lang.c_str() + 4);
    if (idx >= 0) {
      udl_apply(sci, idx);   // full pipeline: default + STYLECLEARALL + UDL styles
      applyChromeStyles();   // what its STYLECLEARALL took with it
      SSM(SCI_SETPROPERTY, "fold", "1");
      langName = lang;
      applyGuiPrefsToDoc();
      return;
    }
    // Unknown UDL name → plain text, like the GUI.
  } else if (!lang.empty()) {
    const char *lexerID = lexer_lexilla_name(lang.c_str());
    if (lexerID) lexer = lexilla_create_lexer(lexerID);
  }
  if (lexer) {
    SSM(SCI_SETILEXER, 0, lexer);
    langName = lang;
    // Keyword class → SCI_SETKEYWORDS index, same base mapping as
    // EditorView.mm (per-language overrides arrive with the theme shim).
    static const struct { const char *kwClass; int index; } kwIndex[] = {
        {"instre1", 0}, {"type1", 1}, {"instre2", 2}, {"type2", 3},
        {"type3", 4}, {"type4", 5}, {"type5", 6}, {"type6", 7}, {"type7", 8},
    };
    for (const auto &k : kwIndex) {
      const char *kw = langsmgr_keywords(lang.c_str(), k.kwClass);
      if (kw && *kw) SSM(SCI_SETKEYWORDS, k.index, kw);
    }
    // Fold data is computed by the lexer and gated on this per-document
    // property — without it the fold margin stays empty.
    SSM(SCI_SETPROPERTY, "fold", "1");
  } else {
    SSM(SCI_SETILEXER, 0, 0);  // plain text
  }
  applyLexerStyles();
  applyGuiPrefsToDoc();
}

static void setupLanguage(void) {
  std::string lang;
  if (!filePath.empty()) {
    std::string ext = lowercased(pathExtension(filePath));
    if (!ext.empty()) {
      const char *l = langsmgr_ext_to_lang(ext.c_str());
      if (l) lang = l;
      // Linux: built-in languages win; a UDL claims the extension only when
      // langs.xml leaves it free (the GUI's lexer_apply_from_path order).
      if (lang.empty()) {
        udl_load_all();
        int idx = udl_find_by_ext(ext.c_str());
        if (idx >= 0) lang = udl_key(idx);
      }
    }
  }
  applyLanguage(lang);
}

// ── GUI interop appliers (Phase 4) ────────────────────────────────────────────

// Tab settings live in the DOCUMENT; applied on every buffer activation and
// on a preference change, honoring the GUI's per-language overrides.
static void applyGuiPrefsToDoc(void) {
  int width = gGuiPrefs.tabWidth;
  int useTabs = gGuiPrefs.useTabs;
  if (!langName.empty()) {
    auto it = gGuiPrefs.tabOverrides.find(langName);
    if (it != gGuiPrefs.tabOverrides.end()) {
      if (it->second.tabWidth != -1) width = it->second.tabWidth;
      if (it->second.useTabs != -1) useTabs = it->second.useTabs;
    }
  }
  if (width != -1) SSM(SCI_SETTABWIDTH, width, 0);
  if (useTabs != -1) SSM(SCI_SETUSETABS, useTabs, 0);
}

// View-level mirrors. Only runs at startup and when the GUI's plist actually
// changed, so a TUI-local ^T wrap toggle is not fought every tick — a GUI
// change re-asserts, a local toggle sticks until then.
static void applyGuiPrefsToView(void) {
  if (gGuiPrefs.wordWrap != -1)
    SSM(SCI_SETWRAPMODE, gGuiPrefs.wordWrap ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
  if (gGuiPrefs.indentGuides != -1)
    SSM(SCI_SETINDENTATIONGUIDES, gGuiPrefs.indentGuides ? SC_IV_LOOKBOTH : SC_IV_NONE, 0);
  if (gGuiPrefs.showWhitespace != -1)
    SSM(SCI_SETVIEWWS, gGuiPrefs.showWhitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE, 0);
  if (gGuiPrefs.showEOL != -1) SSM(SCI_SETVIEWEOL, gGuiPrefs.showEOL, 0);
  if (gGuiPrefs.edgeMode != -1) SSM(SCI_SETEDGEMODE, gGuiPrefs.edgeMode, 0);
  if (gGuiPrefs.edgeColumn != -1) SSM(SCI_SETEDGECOLUMN, gGuiPrefs.edgeColumn, 0);
}

// The 2s heartbeat (called from nextEvent between pumps): mirrors GUI-side
// changes into the running TUI. Watch index 0 = the resolved stylers.xml
// (theme), 1 = the GUI's defaults plist (editor preferences).
// Linux: index 1 is config.xml.
static bool reloadThemeFromPath(void) {
  npptui::TuiTheme fresh = npptui::DefaultTuiTheme();
  if (gThemePath.empty() || !npptui::LoadTuiTheme(gThemePath, fresh)) return false;
  gTheme = fresh;
  syncStylestoreToTheme();
  applyBaseTheme();
  if (isUdlLang(langName)) applyLanguage(langName);  // Linux: UDL colours re-derive
  else applyLexerStyles();
  return true;
}

static void resetInteropWatch(void) {
  delete gInteropWatch;
  gInteropWatch = new npptui::FileWatch({gThemePath, gGuiConfigPath});
}

static void interopTick(void) {
  if (!gInteropWatch) return;
  double now = monotonicSeconds();
  if (now < gNextInteropCheck) return;
  gNextInteropCheck = now + 2.0;
  bool themeChanged = false, prefsChanged = false;
  for (size_t idx : gInteropWatch->poll()) {
    if (idx == 0) {
      themeChanged = reloadThemeFromPath();
    } else if (idx == 1) {
      gGuiPrefs = npptui::LoadGuiEditorPrefs(guiDomain());
      applyGuiPrefsToView();
      applyGuiPrefsToDoc();
      prefsChanged = true;
      // A defaults write can also mean the GUI switched its ACTIVE THEME
      // (NPPActiveTheme — dark mode commits DarkModeDefault there without
      // ever touching stylers.xml). Re-resolve; on a move, re-point the
      // watch at the new file and load it.
      std::string newPath = resolveThemePath();
      if (newPath != gThemePath) {
        gThemePath = newPath;
        resetInteropWatch();
        themeChanged = reloadThemeFromPath() || themeChanged;
      }
    }
  }
  std::string note;
  if (themeChanged && prefsChanged) note = "— theme + preferences updated from Nextpad++";
  else if (themeChanged) note = "— theme updated from Nextpad++";
  else if (prefsChanged) note = "— preferences updated from Nextpad++";
  // Repaint only outside modals (a modal's popup would be painted over; the
  // new styles still apply and its next frame catches up).
  if (!note.empty() && gModalDepth == 0) refreshAll(note);
}

// ── file I/O (T1.4) ───────────────────────────────────────────────────────────

// ── encodings (T3.4) ──────────────────────────────────────────────────────────
// Linux: src/encoding.c does the detection (BOM → UTF-8 validation →
// uchardet → Latin-1) and the decode; the strict encode-back is done here
// with g_convert because encoding_from_utf8 falls back to raw UTF-8 on an
// unconvertible character, and a save must fail instead.

// The npp_encodings[] key for an (encoding, BOM) pair: the table carries the
// BOM variants as separate rows ("UTF-8 BOM", "UTF-16 LE BOM", …).
static std::string encodingTableKey(const TuiEncoding &enc, bool hasBOM) {
  if (hasBOM && (enc == kEncUtf8 || enc == kEncUtf16LE || enc == kEncUtf16BE))
    return enc + " BOM";
  return enc;
}

// The reverse: a table key back into (encoding, BOM).
static void splitEncodingKey(const char *key, TuiEncoding *outEnc, bool *outBOM) {
  std::string k = key ? key : kEncUtf8;
  *outBOM = encoding_has_bom(k.c_str());
  if (*outBOM && k.size() > 4 && k.compare(k.size() - 4, 4, " BOM") == 0) k.resize(k.size() - 4);
  *outEnc = k;
}

static const char *iconvNameFor(const std::string &tableKey) {
  for (int i = 0; i < npp_encoding_count; i++)
    if (tableKey == npp_encodings[i].display) return npp_encodings[i].iconv;
  return nullptr;
}

// Detect a file's encoding and return its content as the UTF-8 bytes the
// editor works in. Chain: UTF-8 (±BOM, fast path — bytes pass through after a
// validity check) → Foundation's own detection (catches BOM'd UTF-16/32) →
// CP1252 → Latin-1 (never fails). Conversion back happens at save.
// Linux: UTF-8 (±BOM) → BOM'd UTF-16 → uchardet → Latin-1 (encoding_detect).
static std::string readFileAsUtf8(const std::string &path, TuiEncoding *outEnc, bool *outBOM) {
  *outEnc = kEncUtf8;
  *outBOM = false;
  gchar *raw = nullptr;
  gsize rawLen = 0;
  if (!g_file_get_contents(path.c_str(), &raw, &rawLen, nullptr) || rawLen == 0) {
    g_free(raw);
    return "";
  }
  const char *key = encoding_detect((const guchar *)raw, rawLen);
  splitEncodingKey(key, outEnc, outBOM);
  gsize outLen = 0;
  char *utf8 = encoding_to_utf8(key, (const guchar *)raw, rawLen, &outLen);
  g_free(raw);
  std::string out = utf8 ? std::string(utf8, outLen) : std::string();
  g_free(utf8);
  return out;
}

// Encoding of the file on disk at `path` (BOM-aware), without keeping the
// content. Used when saving hot-exit snapshots (which are always UTF-8) over
// a file whose original encoding was detected in a previous run.
static TuiEncoding diskEncodingAtPath(const std::string &path, bool *outBOM) {
  TuiEncoding enc = kEncUtf8;
  *outBOM = false;
  if (fileExists(path)) (void)readFileAsUtf8(path, &enc, outBOM);
  return enc;
}

// Encode the buffer's UTF-8 text back into `enc` (strict — a save must
// never silently mangle characters the encoding cannot hold). Returns false
// when the conversion fails.
static bool encodeForDisk(const std::string &utf8, const TuiEncoding &enc, bool hasBOM,
                          std::string &out) {
  if (enc == kEncUtf8) {
    out = hasBOM ? "\xEF\xBB\xBF" + utf8 : utf8;
    return true;
  }
  const char *iconv = iconvNameFor(encodingTableKey(enc, hasBOM));
  if (!iconv) return false;
  gsize wlen = 0;
  GError *err = nullptr;
  char *conv = g_convert(utf8.data(), (gssize)utf8.size(), iconv, "UTF-8", nullptr, &wlen, &err);
  if (!conv) {
    g_clear_error(&err);
    return false;
  }
  // Explicit-endian UTF-16 writers emit no BOM; restore the one the file had.
  out.clear();
  if (hasBOM && enc == kEncUtf16BE) out = "\xFE\xFF";
  else if (hasBOM && enc == kEncUtf16LE) out = "\xFF\xFE";
  out.append(conv, wlen);
  g_free(conv);
  return true;
}

// Linux: NSDataWritingAtomic — g_file_set_contents writes a temp file in the
// directory and renames it into place.
static bool writeAtomically(const std::string &path, const std::string &bytes) {
  return g_file_set_contents(path.c_str(), bytes.data(), (gssize)bytes.size(), nullptr);
}

static bool isWritableFile(const std::string &path) { return access(path.c_str(), W_OK) == 0; }

// Writes the current document to filePath in the given ON-DISK encoding
// (buffer text is always UTF-8). Strict conversion: characters the target
// encoding cannot hold fail the save rather than silently mangling the file.
static bool saveFile(const TuiEncoding &enc, bool hasBOM) {
  if (filePath.empty()) return false;
  // Atomic writes replace the file by rename, which succeeds even when the
  // target itself is not writable (only the directory matters) — so refuse
  // explicitly, or a read-only file would be silently overwritten.
  if (fileExists(filePath) && !isWritableFile(filePath)) return false;
  long len = SSM(SCI_GETLENGTH, 0, 0);
  const char *bytes = (const char *)SSM(SCI_GETCHARACTERPOINTER, 0, 0);
  std::string utf8 = len > 0 ? std::string(bytes, (size_t)len) : std::string();
  std::string data;
  if (!encodeForDisk(utf8, enc, hasBOM, data)) return false;
  if (!writeAtomically(filePath, data)) return false;
  SSM(SCI_SETSAVEPOINT, 0, 0);
  return true;
}

// ── buffers (T2.7a) ───────────────────────────────────────────────────────────

static void syncViewStateToBuffer(void) {
  TuiBuffer &b = gBuffers[gCur];
  b.path = filePath;
  b.lang = langName;
  b.caret = SSM(SCI_GETCURRENTPOS, 0, 0);
  b.firstVisible = SSM(SCI_GETFIRSTVISIBLELINE, 0, 0);
  b.dirtyCache = SSM(SCI_GETMODIFY, 0, 0) != 0;
}

// Restore the view-side state of the current buffer after any document swap.
static void restoreCurrentViewState(void) {
  const TuiBuffer &b = gBuffers[gCur];
  SSM(SCI_SETDOCPOINTER, 0, b.doc);
  SSM(SCI_GOTOPOS, b.caret, 0);
  SSM(SCI_SETFIRSTVISIBLELINE, b.firstVisible, 0);
}

// Point the view at gCur's document, lazily loading its file, reapplying the
// view-wide styles for its lexer (styles live in the VIEW; the lexer, text,
// undo and modify state travel with the DOCUMENT), and restoring caret/scroll.
static void activateCurrentBuffer(void) {
  TuiBuffer &b = gBuffers[gCur];
  SSM(SCI_SETDOCPOINTER, 0, b.doc);
  filePath = b.path;
  langName = b.lang;
  applyBaseTheme();
  if (!b.loaded) {
    if (fileExists(b.backupPath)) {
      // Hot exit: restore the snapshot as MODIFIED text. Deliberately NO
      // SCI_EMPTYUNDOBUFFER here — Scintilla's EmptyUndoBuffer also sets the
      // save point, which would mark the buffer clean and lose the snapshot
      // on the next quit-without-save. The insert stays on the undo stack
      // (undoing all the way back yields an empty buffer, same as typing).
      // Snapshots are always UTF-8; the SAVE encoding is re-detected from
      // the original file still sitting on disk (deleted since → UTF-8).
      gchar *data = nullptr;
      gsize len = 0;
      if (g_file_get_contents(b.backupPath.c_str(), &data, &len, nullptr) && len)
        SSM(SCI_APPENDTEXT, len, data);
      g_free(data);
      bool bom = false;
      b.encoding = diskEncodingAtPath(b.path, &bom);
      b.hasBOM = bom;
      b.backupPath.clear();
      b.dirtyCache = true;
    } else {
      if (!filePath.empty()) {
        bool bom = false;
        std::string data = readFileAsUtf8(filePath, &b.encoding, &bom);
        b.hasBOM = bom;
        if (!data.empty()) SSM(SCI_APPENDTEXT, data.size(), data.data());
      }
      SSM(SCI_EMPTYUNDOBUFFER, 0, 0);
      SSM(SCI_SETSAVEPOINT, 0, 0);
    }
    b.loaded = true;
    setupLanguage();  // lexer + keywords onto this document, styles onto the view
    b.lang = langName;
  } else if (isUdlLang(langName)) {
    applyLanguage(langName);  // Linux: UDL styles + lexer travel as one unit
  } else {
    applyLexerStyles();
  }
  applyGuiPrefsToDoc();  // tab settings are per-document (GUI interop)
  gLastBracePos = gLastMatchPos = -2;
  curEncoding = b.encoding;
  curHasBOM = b.hasBOM;
  SSM(SCI_GOTOPOS, b.caret, 0);
  SSM(SCI_SETFIRSTVISIBLELINE, b.firstVisible, 0);
}

static void switchToBuffer(size_t i) {
  if (i >= gBuffers.size() || i == gCur) return;
  syncViewStateToBuffer();
  gCur = i;
  activateCurrentBuffer();
}

// Saves every dirty buffer that has a path — including buffers restored by
// hot exit but never opened this run, whose snapshot bytes ARE the unsaved
// content and are copied to the real file directly. Returns false when an
// unnamed dirty buffer (or a failed write) remains.
static bool saveAllBuffers(void) {
  syncViewStateToBuffer();
  bool allSaved = true;
  for (TuiBuffer &b : gBuffers) {
    if (!b.loaded) {
      if (b.backupPath.empty()) continue;  // clean, nothing to save
      if (b.path.empty()) {
        allSaved = false;  // unnamed: needs an interactive Save As
        continue;
      }
      // Same read-only refusal as saveFile: atomic writes replace by rename
      // and would silently overwrite a chmod-protected file. Snapshot bytes
      // are UTF-8; convert back to the on-disk file's own encoding.
      gchar *snap = nullptr;
      gsize snapLen = 0;
      bool haveData = g_file_get_contents(b.backupPath.c_str(), &snap, &snapLen, nullptr);
      bool bom = false;
      TuiEncoding enc = diskEncodingAtPath(b.path, &bom);
      std::string data;
      bool encoded = haveData && encodeForDisk(std::string(snap, snapLen), enc, bom, data);
      g_free(snap);
      if (encoded && !(fileExists(b.path) && !isWritableFile(b.path)) &&
          writeAtomically(b.path, data)) {
        b.backupPath.clear();
        b.dirtyCache = false;
      } else {
        allSaved = false;
      }
      continue;
    }
    SSM(SCI_SETDOCPOINTER, 0, b.doc);
    if (!SSM(SCI_GETMODIFY, 0, 0)) continue;
    if (b.path.empty()) {
      allSaved = false;
      continue;
    }
    filePath = b.path;  // saveFile reads the global
    if (saveFile(b.encoding, b.hasBOM)) b.dirtyCache = false;
    else allSaved = false;
  }
  filePath = gBuffers[gCur].path;
  langName = gBuffers[gCur].lang;
  restoreCurrentViewState();
  return allSaved;
}

// ── modal list popup (T2.5b) ──────────────────────────────────────────────────

// One frame of a modal: clean editor repaint, optional dim pass (find-style
// overlays skip it so matches stay visible), status row, popup on top,
// hardware cursor hidden. Recreates the popup window when its area changes
// (content growth, resize).
static void renderModalPopup(npptui::Popup &popup, WINDOW *&popWin, npptui::Rect &area,
                             bool dim = true) {
  npptui::PopupTheme theme = currentPopupTheme();
  npptui::Rect bounds{0, 0, COLS, LINES};
  npptui::Rect editorBounds{0, 2, COLS, LINES > 3 ? LINES - 3 : 1};
  npptui::Rect newArea = popup.calculateArea(editorBounds, -1, -1);
  if (!popWin || !(newArea == area)) {
    if (popWin) delwin(popWin);
    area = newArea;
    popWin = newwin(area.height, area.width, area.y, area.x);
  }
  scintilla_noutrefresh(sci);  // repaint clean, then (optionally) dim
  if (dim) dimWindow(scintilla_get_window(sci));
  wnoutrefresh(scintilla_get_window(sci));
  drawScrollbar();
  if (dim) dimWindow(scrollWin);
  wnoutrefresh(scrollWin);
  drawTabBar();
  if (dim) dimWindow(tabWin);
  wnoutrefresh(tabWin);
  drawMenuBar();
  if (dim) dimWindow(menuWin);
  wnoutrefresh(menuWin);
  if (gPanelVisible && panelWin) {
    drawPanel();
    if (dim) dimWindow(panelWin);
    wnoutrefresh(panelWin);
  }
  drawStatus("");
  CursesCellSurface surface(popWin, area.x, area.y);
  popup.render(area, bounds, theme, surface);
  wnoutrefresh(popWin);
  curs_set(0);  // the hardware cursor has no place inside a modal
  doupdate();
}

// Runs a centered list popup modally with the dim-behind pass. Returns the
// selected item's data, or "" on cancel. Consumes events one at a time from
// the shared queue (the T2.2b rule: modal loops never hold a private batch).
// keyShortcuts maps a plain character to an item index for one-key confirm.
static std::string runListPopupModal(npptui::Popup &popup,
                                     const std::map<char32_t, size_t> &keyShortcuts) {
  ModalScope modalScope;
  WINDOW *popWin = nullptr;
  npptui::Rect area{};
  auto redraw = [&]() { renderModalPopup(popup, popWin, area); };

  std::string result;
  bool done = false;
  redraw();
  while (!done) {
    Event e;
    if (!nextEvent(e)) break;  // terminal gone: cancel
    if (e.type == Event::Type::Key) {
      const npptui::KeyEvent &k = e.key.event;
      if (k.kind == npptui::KeyEventKind::Release) continue;
      switch (k.code.kind) {
        case KeyKind::Esc: done = true; break;
        case KeyKind::Up: popup.selectPrev(); break;
        case KeyKind::Down: popup.selectNext(); break;
        case KeyKind::Home: popup.selectFirst(); break;
        case KeyKind::End: popup.selectLast(); break;
        case KeyKind::PageUp: popup.pageUp(); break;
        case KeyKind::PageDown: popup.pageDown(); break;
        case KeyKind::Enter:
          if (const npptui::PopupListItem *item = popup.selectedItem()) result = item->data;
          done = true;
          break;
        case KeyKind::Char: {
          auto it = keyShortcuts.find(k.code.ch);
          if (it != keyShortcuts.end() && popup.selectIndex(it->second)) {
            result = popup.items[it->second].data;
            done = true;
          }
          break;
        }
        default: break;
      }
    } else if (e.type == Event::Type::Mouse) {
      const npptui::MouseEvent &m = e.mouse;
      if (m.kind == npptui::MouseKind::ScrollUp) popup.scrollBy(-3);
      else if (m.kind == npptui::MouseKind::ScrollDown) popup.scrollBy(3);
      else if (m.kind == npptui::MouseKind::Down) {
        if (npptui::PointInRect(area, m.column, m.row)) {
          int row = m.row - area.y - (popup.bordered ? 1 : 0);
          size_t idx = popup.scrollOffset + (size_t)(row < 0 ? 0 : row);
          if (row >= 0 && popup.selectIndex(idx)) {
            result = popup.items[idx].data;
            done = true;
          }
        } else {
          done = true;  // click outside cancels
        }
      }
    }
    if (!done) redraw();
  }
  if (popWin) delwin(popWin);
  curs_set(1);
  return result;
}

// Runs a read-only text popup modally (Help card): arrows/PgUp/PgDn/wheel
// scroll, Esc/Enter/q/click-outside dismiss. Same one-event-at-a-time rule.
static void runTextPopupModal(npptui::Popup &popup) {
  ModalScope modalScope;
  WINDOW *popWin = nullptr;
  npptui::Rect area{};
  bool done = false;
  renderModalPopup(popup, popWin, area);
  while (!done) {
    Event e;
    if (!nextEvent(e)) break;
    if (e.type == Event::Type::Key) {
      const npptui::KeyEvent &k = e.key.event;
      if (k.kind == npptui::KeyEventKind::Release) continue;
      switch (k.code.kind) {
        case KeyKind::Esc:
        case KeyKind::Enter: done = true; break;
        case KeyKind::Up: popup.scrollBy(-1); break;
        case KeyKind::Down: popup.scrollBy(1); break;
        case KeyKind::PageUp: popup.pageUp(); break;
        case KeyKind::PageDown: popup.pageDown(); break;
        case KeyKind::Home: popup.scrollOffset = 0; break;
        case KeyKind::Char:
          if (k.code.ch == 'q' || k.code.ch == 'Q') done = true;
          break;
        default: break;
      }
    } else if (e.type == Event::Type::Mouse) {
      const npptui::MouseEvent &m = e.mouse;
      if (m.kind == npptui::MouseKind::ScrollUp) popup.scrollBy(-3);
      else if (m.kind == npptui::MouseKind::ScrollDown) popup.scrollBy(3);
      else if (m.kind == npptui::MouseKind::Down &&
               !npptui::PointInRect(area, m.column, m.row))
        done = true;
    }
    if (!done) renderModalPopup(popup, popWin, area);
  }
  if (popWin) delwin(popWin);
  curs_set(1);
}

// ── find/replace (T2.7b) ──────────────────────────────────────────────────────

static void utf8Append(std::string &s, char32_t cp) {
  if (cp < 0x80) {
    s += (char)cp;
  } else if (cp < 0x800) {
    s += (char)(0xC0 | (cp >> 6));
    s += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += (char)(0xE0 | (cp >> 12));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  } else {
    s += (char)(0xF0 | (cp >> 18));
    s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
}

static std::string gFindQuery;  // persists across popups; F3 reuses it

// Smart case: an all-lowercase query matches case-insensitively; any capital
// makes it exact.
static int findSearchFlags(void) {
  for (char c : gFindQuery)
    if (c >= 'A' && c <= 'Z') return SCFIND_MATCHCASE;
  return 0;
}

// Search [from,to) (to < from searches backwards) and select the match.
// Returns the match start, or -1.
static long findInRange(long from, long to) {
  SSM(SCI_SETTARGETSTART, from, 0);
  SSM(SCI_SETTARGETEND, to, 0);
  SSM(SCI_SETSEARCHFLAGS, findSearchFlags(), 0);
  long pos = SSM(SCI_SEARCHINTARGET, gFindQuery.size(), gFindQuery.c_str());
  if (pos >= 0)
    SSM(SCI_SETSEL, SSM(SCI_GETTARGETSTART, 0, 0), SSM(SCI_GETTARGETEND, 0, 0));
  return pos;
}

static bool findFrom(long from, bool wrap) {
  if (gFindQuery.empty()) return false;
  long len = SSM(SCI_GETLENGTH, 0, 0);
  if (findInRange(from, len) >= 0) return true;
  return wrap && findInRange(0, len) >= 0;
}

static bool findPrevFrom(long from) {
  if (gFindQuery.empty()) return false;
  if (findInRange(from, 0) >= 0) return true;  // backwards
  return findInRange(SSM(SCI_GETLENGTH, 0, 0), 0) >= 0;  // wrap from the end
}

// Occurrence count for the info line: capped at 1000, skipped (-1) on files
// past 8 MB so typing in the find popup never stalls.
static long countMatches(void) {
  if (gFindQuery.empty()) return 0;
  long len = SSM(SCI_GETLENGTH, 0, 0);
  if (len > 8 * 1024 * 1024) return -1;
  long count = 0, pos = 0;
  while (count < 1000) {
    SSM(SCI_SETTARGETSTART, pos, 0);
    SSM(SCI_SETTARGETEND, len, 0);
    SSM(SCI_SETSEARCHFLAGS, findSearchFlags(), 0);
    if (SSM(SCI_SEARCHINTARGET, gFindQuery.size(), gFindQuery.c_str()) < 0) break;
    count++;
    long e = SSM(SCI_GETTARGETEND, 0, 0);
    pos = e > pos ? e : pos + 1;  // guard against zero-width stalls
  }
  return count;
}

static void cmdFindNext(std::string *transient) {
  if (gFindQuery.empty()) {
    *transient = "— no find query (use ^F)";
    return;
  }
  if (!findFrom(SSM(SCI_GETSELECTIONEND, 0, 0), true)) *transient = "— no results";
}

static void cmdFindPrev(std::string *transient) {
  if (gFindQuery.empty()) {
    *transient = "— no find query (use ^F)";
    return;
  }
  if (!findPrevFrom(SSM(SCI_GETSELECTIONSTART, 0, 0))) *transient = "— no results";
}

// Find/replace popups share this near-top-right, no-dim overlay loop. The
// caller supplies the per-event handling via the switch cases it cares about.
static npptui::Popup makeFindStylePopup(const std::string &title) {
  npptui::Popup popup = npptui::Popup::text({""});
  popup.title = title;
  popup.showInput = true;
  popup.width = 46;
  popup.maxHeight = 4;  // borders + input + one info line
  return popup;
}

static void placeFindPopup(npptui::Popup &popup) {
  int w = popup.width < COLS ? popup.width : COLS;
  int x = COLS - w - 2;
  popup.position = npptui::PopupPosition::fixed(x > 0 ? x : 0, 2);
}

static void runFindPopup(void) {
  ModalScope modalScope;
  long anchor = SSM(SCI_GETSELECTIONSTART, 0, 0);
  // Prefill from a short single-line selection (what you select is what you
  // usually want to find).
  long selStart = anchor, selEnd = SSM(SCI_GETSELECTIONEND, 0, 0);
  if (selEnd > selStart && selEnd - selStart < 200) {
    std::string sel((size_t)(selEnd - selStart), '\0');
    Sci_TextRangeFull tr{{(Sci_Position)selStart, (Sci_Position)selEnd}, sel.data()};
    SSM(SCI_GETTEXTRANGEFULL, 0, &tr);
    if (sel.find('\n') == std::string::npos) gFindQuery = sel;
  }

  npptui::Popup popup = makeFindStylePopup(" Find ");
  auto updateSearch = [&](bool fromAnchor) {
    popup.input = gFindQuery;
    if (gFindQuery.empty()) {
      SSM(SCI_GOTOPOS, anchor, 0);
      popup.textLines = {"Type to search"};
    } else if (findFrom(fromAnchor ? anchor : SSM(SCI_GETSELECTIONEND, 0, 0), true)) {
      long n = countMatches();
      popup.textLines = {n < 0 ? std::string("large file — count skipped")
                         : n >= 1000 ? std::string("1000+ matches")
                                     : std::to_string(n) + " match" + (n == 1 ? "" : "es")};
    } else {
      SSM(SCI_GOTOPOS, anchor, 0);
      popup.textLines = {"No results"};
    }
    placeFindPopup(popup);
  };

  WINDOW *popWin = nullptr;
  npptui::Rect area{};
  updateSearch(true);
  renderModalPopup(popup, popWin, area, /*dim=*/false);
  bool done = false;
  while (!done) {
    Event e;
    if (!nextEvent(e)) break;
    if (e.type == Event::Type::Key) {
      const npptui::KeyEvent &k = e.key.event;
      if (k.kind == npptui::KeyEventKind::Release) continue;
      switch (k.code.kind) {
        case KeyKind::Esc: done = true; break;
        case KeyKind::Enter: updateSearch(false); break;  // next match (wraps)
        case KeyKind::Backspace:
          while (!gFindQuery.empty()) {
            unsigned char last = (unsigned char)gFindQuery.back();
            gFindQuery.pop_back();
            if (last < 0x80 || last >= 0xC0) break;
          }
          updateSearch(true);
          break;
        case KeyKind::Char:
          if (!(k.mods & (npptui::kModCtrl | npptui::kModAlt)) && k.code.ch >= 32) {
            utf8Append(gFindQuery, k.code.ch);
            updateSearch(true);
          }
          break;
        default: break;
      }
    } else if (e.type == Event::Type::Paste) {
      for (char c : e.paste)
        if ((unsigned char)c >= 32 || (unsigned char)c >= 0x80) gFindQuery += c;
      updateSearch(true);
    } else if (e.type == Event::Type::Mouse) {
      if (e.mouse.kind == npptui::MouseKind::Down &&
          !npptui::PointInRect(area, e.mouse.column, e.mouse.row))
        done = true;
    }
    if (!done) renderModalPopup(popup, popWin, area, /*dim=*/false);
  }
  if (popWin) delwin(popWin);
  curs_set(1);
}

static void runReplacePopup(std::string *transient) {
  ModalScope modalScope;
  if (gFindQuery.empty()) {
    *transient = "— no find query (use ^F first)";
    return;
  }
  std::string shownQuery = gFindQuery.size() > 14 ? gFindQuery.substr(0, 13) + "…" : gFindQuery;
  npptui::Popup popup = makeFindStylePopup(" Replace \"" + shownQuery + "\" ");
  std::string replacement;
  long replaced = 0;

  // Position on a match to start with.
  bool haveMatch = findFrom(SSM(SCI_GETSELECTIONSTART, 0, 0), true);

  auto updateStatus = [&](const std::string &extra) {
    popup.input = replacement;
    std::string s = extra.empty() ? "Enter: replace+next · ^A: replace all" : extra;
    popup.textLines = {s};
    placeFindPopup(popup);
  };

  // Replace the current selection when it is a query match; reselects the
  // next occurrence. Returns false when nothing was replaced.
  auto replaceCurrent = [&]() -> bool {
    long selStart = SSM(SCI_GETSELECTIONSTART, 0, 0);
    long selEnd = SSM(SCI_GETSELECTIONEND, 0, 0);
    if (selEnd <= selStart) return false;
    SSM(SCI_SETTARGETSTART, selStart, 0);
    SSM(SCI_SETTARGETEND, selEnd, 0);
    SSM(SCI_SETSEARCHFLAGS, findSearchFlags(), 0);
    if (SSM(SCI_SEARCHINTARGET, gFindQuery.size(), gFindQuery.c_str()) != selStart ||
        SSM(SCI_GETTARGETEND, 0, 0) != selEnd)
      return false;
    SSM(SCI_REPLACETARGET, replacement.size(), replacement.c_str());
    replaced++;
    long next = selStart + (long)replacement.size();
    haveMatch = findFrom(next, true);
    return true;
  };

  auto replaceAll = [&]() {
    SSM(SCI_BEGINUNDOACTION, 0, 0);
    long pos = 0, n = 0;
    for (;;) {
      long len = SSM(SCI_GETLENGTH, 0, 0);
      SSM(SCI_SETTARGETSTART, pos, 0);
      SSM(SCI_SETTARGETEND, len, 0);
      SSM(SCI_SETSEARCHFLAGS, findSearchFlags(), 0);
      long s = SSM(SCI_SEARCHINTARGET, gFindQuery.size(), gFindQuery.c_str());
      if (s < 0) break;
      SSM(SCI_REPLACETARGET, replacement.size(), replacement.c_str());
      pos = s + (long)replacement.size();
      n++;
    }
    SSM(SCI_ENDUNDOACTION, 0, 0);
    replaced += n;
    updateStatus(std::to_string(n) + " replaced");
  };

  WINDOW *popWin = nullptr;
  npptui::Rect area{};
  updateStatus(haveMatch ? "" : "No results");
  renderModalPopup(popup, popWin, area, /*dim=*/false);
  bool done = false;
  while (!done) {
    Event e;
    if (!nextEvent(e)) break;
    if (e.type == Event::Type::Key) {
      const npptui::KeyEvent &k = e.key.event;
      if (k.kind == npptui::KeyEventKind::Release) continue;
      switch (k.code.kind) {
        case KeyKind::Esc: done = true; break;
        case KeyKind::Enter:
          if (haveMatch && replaceCurrent())
            updateStatus(haveMatch ? std::to_string(replaced) + " replaced"
                                   : std::to_string(replaced) + " replaced — no more");
          else
            updateStatus("No results");
          break;
        case KeyKind::Backspace:
          while (!replacement.empty()) {
            unsigned char last = (unsigned char)replacement.back();
            replacement.pop_back();
            if (last < 0x80 || last >= 0xC0) break;
          }
          updateStatus("");
          break;
        case KeyKind::Char:
          if (k.mods == npptui::kModCtrl && (k.code.ch == 'a' || k.code.ch == 'A')) {
            replaceAll();
          } else if (!(k.mods & (npptui::kModCtrl | npptui::kModAlt)) && k.code.ch >= 32) {
            utf8Append(replacement, k.code.ch);
            updateStatus("");
          }
          break;
        default: break;
      }
    } else if (e.type == Event::Type::Paste) {
      for (char c : e.paste)
        if ((unsigned char)c >= 32 || (unsigned char)c >= 0x80) replacement += c;
      updateStatus("");
    } else if (e.type == Event::Type::Mouse) {
      if (e.mouse.kind == npptui::MouseKind::Down &&
          !npptui::PointInRect(area, e.mouse.column, e.mouse.row))
        done = true;
    }
    if (!done) renderModalPopup(popup, popWin, area, /*dim=*/false);
  }
  if (popWin) delwin(popWin);
  curs_set(1);
  if (replaced > 0) *transient = formatString("— %ld replaced", replaced);
}

// ── session (T2.9) ────────────────────────────────────────────────────────────

// $NPP_TUI_SESSION override, else the shared config dir. NEVER the GUI's
// session.plist (see shim/session.h).
// Linux: tui-session.xml next to the GUI's config.xml.
static std::string sessionFilePath(void) {
  const char *env = getenv("NPP_TUI_SESSION");
  if (env && env[0]) return env;
  gchar *p = npp_user_file(nullptr, "tui-session.xml");
  std::string s = p;
  g_free(p);
  return s;
}

// Hot-exit snapshot dir: OURS, never the GUI's backup machinery (same
// two-writers lesson as the session file). $NPP_TUI_BACKUP_DIR overrides.
// Linux: <user data dir>/tui-backup — deliberately NOT the GUI's backup/,
// whose "<name>@<stamp>" files the prune below would otherwise delete.
static std::string backupDirPath(void) {
  const char *env = getenv("NPP_TUI_BACKUP_DIR");
  std::string dir;
  if (env && env[0]) {
    dir = env;
  } else {
    gchar *p = npp_user_file(nullptr, "tui-backup");
    dir = p;
    g_free(p);
  }
  g_mkdir_with_parents(dir.c_str(), 0755);
  return dir;
}

// Set by the explicit "Quit, Discard Unsaved Changes" command: the session
// still records files/positions, but no snapshots are written.
static bool gDiscardOnQuit = false;

// Saved on clean quit: every restorable buffer with caret/scroll, plus a
// hot-exit snapshot (snap-<uuid>.txt) for each buffer that is modified — so
// ^Q needs no dialog and unsaved work survives the relaunch. Buffers loaded
// from the session but never opened this run keep their previous snapshot.
static void saveSession(void) {
  syncViewStateToBuffer();
  npptui::TuiSession session;
  size_t namedCurrent = 0;
  for (size_t i = 0; i < gBuffers.size(); i++) {
    TuiBuffer &b = gBuffers[i];
    npptui::SessionBuffer entry;
    entry.path = b.path;
    entry.caret = b.caret;
    entry.firstVisible = b.firstVisible;
    if (!gDiscardOnQuit) {
      if (b.loaded) {
        SSM(SCI_SETDOCPOINTER, 0, b.doc);
        if (SSM(SCI_GETMODIFY, 0, 0)) {
          // GUI-parity naming (EditorView saveBackupToDirectory + the #288
          // uniqueness fix): "<name>@<yyyy-MM-dd_HHmmss>", with -2/-3
          // appended on collision so a same-second twin never overwrites
          // another buffer's only copy of its unsaved text.
          std::string base = !b.path.empty() ? lastPathComponent(b.path) : "untitled";
          GDateTime *now = g_date_time_new_now_local();
          gchar *stamp = g_date_time_format(now, "%Y-%m-%d_%H%M%S");
          std::string name = base + "@" + stamp;
          g_free(stamp);
          g_date_time_unref(now);
          std::string dir = backupDirPath();
          std::string snap = dir + "/" + name;
          for (unsigned long n = 2; fileExists(snap); n++)
            snap = dir + "/" + formatString("%s-%lu", name.c_str(), n);
          long len = SSM(SCI_GETLENGTH, 0, 0);
          const char *bytes = (const char *)SSM(SCI_GETCHARACTERPOINTER, 0, 0);
          std::string data = len > 0 ? std::string(bytes, (size_t)len) : std::string();
          if (writeAtomically(snap, data)) entry.backup = snap;
        }
      } else if (!b.backupPath.empty()) {
        entry.backup = b.backupPath;  // unopened this run: keep it
      }
    }
    if (entry.path.empty() && entry.backup.empty()) continue;
    if (i == gCur) namedCurrent = session.buffers.size();
    session.buffers.push_back(std::move(entry));
  }
  session.current = namedCurrent;
  restoreCurrentViewState();
  if (!npptui::SaveTuiSession(sessionFilePath(), session)) return;

  // Prune OUR orphaned snapshots: only files in our dir whose name carries
  // the "@yyyy-MM-dd_HHmmss" tail we generate (plus legacy snap-*.txt from
  // the first hot-exit build) and that no session entry references. Anything
  // else in the directory is left strictly alone.
  std::set<std::string> referenced;
  for (const npptui::SessionBuffer &e : session.buffers)
    if (!e.backup.empty()) referenced.insert(e.backup);
  std::string dir = backupDirPath();
  static GRegex *ownedPattern =
      g_regex_new("@\\d{4}-\\d{2}-\\d{2}_\\d{6}(-\\d+)?$", (GRegexCompileFlags)0,
                  (GRegexMatchFlags)0, nullptr);
  GDir *d = g_dir_open(dir.c_str(), 0, nullptr);
  if (!d) return;
  while (const gchar *name = g_dir_read_name(d)) {
    std::string n = name;
    bool legacy = n.compare(0, 5, "snap-") == 0 && n.size() >= 4 &&
                  n.compare(n.size() - 4, 4, ".txt") == 0;
    bool owned = ownedPattern && g_regex_match(ownedPattern, name, (GRegexMatchFlags)0, nullptr);
    if (!legacy && !owned) continue;
    std::string full = dir + "/" + n;
    if (!referenced.count(full)) g_unlink(full.c_str());
  }
  g_dir_close(d);
}

// ── system clipboard bridge (T3.3) ────────────────────────────────────────────

// After a copy/cut, Scinterm's internal clipboard is mirrored outward two
// ways: OSC 52 (reaches the user's LOCAL clipboard even over SSH, on
// kitty/Ghostty/iTerm2/WezTerm; Terminal.app ignores it harmlessly) and
// pbcopy when running locally. NPP_TUI_NO_CLIPBOARD suppresses both (tests
// must never clobber the user's clipboard).
// Linux: OSC 52 is ALWAYS emitted (Fresh's model — it is what makes SSH copy
// work in kitty/foot/WezTerm/Ghostty and is harmless elsewhere); the local
// tool chain is wl-copy under Wayland, xclip under X11, when installed and
// not over SSH. Reads go through wl-paste → xclip -o → xsel -ob.
static bool clipboardBridgeEnabled(void) { return getenv("NPP_TUI_NO_CLIPBOARD") == nullptr; }

static bool haveProgram(const char *name) {
  gchar *p = g_find_program_in_path(name);
  bool ok = p != nullptr;
  g_free(p);
  return ok;
}

// The local copy command for this session, or "" when none applies.
static std::string localClipboardCopyCommand(void) {
  if (getenv("SSH_TTY")) return "";
  const char *wl = getenv("WAYLAND_DISPLAY");
  const char *x = getenv("DISPLAY");
  if (wl && wl[0] && haveProgram("wl-copy")) return "wl-copy";
  if (x && x[0] && haveProgram("xclip")) return "xclip -selection clipboard -i";
  if (x && x[0] && haveProgram("xsel")) return "xsel -ib";
  return "";
}

// The local paste command for this session, or "" when none applies.
static std::string localClipboardPasteCommand(void) {
  const char *wl = getenv("WAYLAND_DISPLAY");
  const char *x = getenv("DISPLAY");
  if (wl && wl[0] && haveProgram("wl-paste")) return "wl-paste --no-newline";
  if (x && x[0] && haveProgram("xclip")) return "xclip -selection clipboard -o";
  if (x && x[0] && haveProgram("xsel")) return "xsel -ob";
  return "";
}

static void systemClipboardSet(const char *text, size_t len) {
  if (!clipboardBridgeEnabled()) return;
  if (len == 0 || len > 1024 * 1024) return;
  gchar *b64 = g_base64_encode((const guchar *)text, len);
  // OSC 52: set the "c" (clipboard) selection.
  fprintf(stdout, "\x1b]52;c;%s\x07", b64);
  fflush(stdout);
  g_free(b64);
  std::string cmd = localClipboardCopyCommand();
  if (!cmd.empty()) {
    // Fire-and-forget: the tool may outlive us (wl-copy forks to serve the
    // selection); SIGPIPE is ignored process-wide (main) so a tool that
    // exits early cannot kill the editor.
    FILE *pb = popen((cmd + " 2>/dev/null").c_str(), "w");
    if (pb) {
      fwrite(text, 1, len, pb);
      pclose(pb);
    }
  }
}

static void bridgeClipboardOut(void) {
  int len = 0;
  char *text = scintilla_get_clipboard(sci, &len);
  if (!text) return;
  if (len > 0) systemClipboardSet(text, (size_t)len);
  free(text);
}

// ^V pastes the SYSTEM clipboard when running locally (over SSH the
// terminal's own paste arrives via bracketed paste instead). Falls back to
// Scintilla's internal clipboard when the system one is empty/unavailable.
static bool bridgeClipboardIn(void) {
  if (!clipboardBridgeEnabled() || getenv("SSH_TTY")) return false;
  std::string cmd = localClipboardPasteCommand();
  if (cmd.empty()) return false;
  FILE *pb = popen((cmd + " 2>/dev/null").c_str(), "r");
  if (!pb) return false;
  std::string content;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, pb)) > 0) content.append(buf, n);
  pclose(pb);
  if (content.empty()) return false;
  SSM(SCI_REPLACESEL, 0, content.c_str());
  return true;
}

// ── commands (T2.6) ───────────────────────────────────────────────────────────

static void cmdSave(std::string *transient) {
  if (filePath.empty()) {
    std::string p = promptStatus("Save as: ");
    if (!p.empty()) {
      filePath = standardizedAbsolutePath(p);
      setupLanguage();
    }
  }
  *transient = saveFile(curEncoding, curHasBOM) ? "— saved" : "— SAVE FAILED";
  gBuffers[gCur].path = filePath;
  gBuffers[gCur].lang = langName;
}

// Shared save/discard/cancel dialog; returns "save", "discard" or "".
static std::string unsavedChangesDialog(const std::string &title, const char *saveLabel,
                                        const char *discardLabel) {
  npptui::PopupListItem save(saveLabel);
  save.data = "save";
  save.detail = "s";
  npptui::PopupListItem discard(discardLabel);
  discard.data = "discard";
  discard.detail = "q";
  npptui::PopupListItem cancel("Cancel");
  cancel.data = "cancel";
  cancel.detail = "esc";
  npptui::Popup popup = npptui::Popup::list({save, discard, cancel});
  popup.title = std::string(" ") + title + " ";
  popup.position = npptui::PopupPosition::centered();
  popup.width = 34;
  popup.maxHeight = 5;
  std::string choice =
      runListPopupModal(popup, {{'s', 0}, {'S', 0}, {'q', 1}, {'Q', 1}});
  return choice == "cancel" ? "" : choice;
}

// Generic two-choice confirmation; true = confirmed.
static bool confirmDialog(const std::string &title, const char *confirmLabel) {
  npptui::PopupListItem go(confirmLabel);
  go.data = "go";
  npptui::PopupListItem cancel("Cancel");
  cancel.data = "cancel";
  cancel.detail = "esc";
  npptui::Popup popup = npptui::Popup::list({go, cancel});
  popup.title = std::string(" ") + title + " ";
  popup.position = npptui::PopupPosition::centered();
  popup.width = std::max(34, (int)npptui::DisplayWidthUtf8(confirmLabel) + 6);
  popup.maxHeight = 4;
  return runListPopupModal(popup, {}) == "go";
}

// Hot exit (Notepad++ behavior): quit instantly — unsaved work is
// snapshotted by saveSession and restored dirty on the next launch.
static void cmdQuit(bool &running, std::string *) { running = false; }

static void cmdQuitDiscard(bool &running) {
  gDiscardOnQuit = true;
  running = false;
}

static void cmdSaveAll(std::string *transient) {
  *transient = saveAllBuffers() ? "— all saved" : "— some buffers not saved (unnamed or read-only)";
}

static void cmdNextBuffer(void) { switchToBuffer((gCur + 1) % gBuffers.size()); }
static void cmdPrevBuffer(void) {
  switchToBuffer((gCur + gBuffers.size() - 1) % gBuffers.size());
}

// Buffers list (^L) — the quick-switch popup: every open buffer with
// its dirty marker and folder, current one preselected; Enter/click switches.
static void cmdBuffersList(void) {
  syncViewStateToBuffer();
  std::vector<npptui::PopupListItem> items;
  int longest = 0;
  for (size_t i = 0; i < gBuffers.size(); i++) {
    const TuiBuffer &b = gBuffers[i];
    std::string label = !b.path.empty() ? lastPathComponent(b.path) : "(untitled)";
    bool dirty = i == gCur ? SSM(SCI_GETMODIFY, 0, 0) != 0
                           : (b.dirtyCache || (!b.loaded && !b.backupPath.empty()));
    if (dirty) label += " •";
    npptui::PopupListItem item(label);
    item.data = std::to_string(i);
    if (!b.path.empty()) item.detail = abbreviatingWithTilde(deletingLastPathComponent(b.path));
    int w = npptui::DisplayWidthUtf8(label) + npptui::DisplayWidthUtf8(item.detail);
    longest = std::max(longest, w);
    items.push_back(std::move(item));
  }
  npptui::Popup popup = npptui::Popup::list(std::move(items));
  popup.title = " Buffers ";
  popup.position = npptui::PopupPosition::centered();
  popup.width = std::min(COLS - 4, std::max(36, longest + 8));
  popup.maxHeight = 16;
  popup.selectIndex(gCur);
  std::string choice = runListPopupModal(popup, {});
  if (!choice.empty()) switchToBuffer((size_t)std::stoul(choice));
}

// Opens (or switches to) `abs` — the shared tail of Open File, the CLI
// merge, and Project-panel activation.
static void openPath(const std::string &abs) {
  for (size_t i = 0; i < gBuffers.size(); i++) {
    if (!gBuffers[i].path.empty() && gBuffers[i].path == abs) {
      switchToBuffer(i);
      return;
    }
  }
  TuiBuffer b;
  b.path = abs;
  b.doc = SSM(SCI_CREATEDOCUMENT, 0, 0);
  gBuffers.push_back(b);
  switchToBuffer(gBuffers.size() - 1);
}

static void cmdOpenFile(void) {
  std::string p = promptStatus("Open: ");
  if (p.empty()) return;
  openPath(standardizedAbsolutePath(p));
}

// ── Project panel loading + per-kind activation ───────────────────────────────

static bool loadWorkspaceFrom(const std::string &path) {
  if (!npptui::LoadWorkspace(path, gWsRoot)) return false;
  gWsLoaded = true;
  gWsPath = path;
  gWsCollapsed.clear();
  gPanel.selected = 0;
  gPanel.scrollOffset = 0;
  return true;
}

// The GUI's open workspace first (ProjectPanelWorkspace0 in the shared
// defaults — interop again), else a path prompt.
// Linux: the GUI persists its Project panel as <user dir>/project1..3.workspace
// (src/project.c); the first one present is "the GUI's open workspace".
static void projectPromptAndLoad(std::string *transient) {
  for (int i = 1; i <= 3; i++) {
    std::string ws = guiDataDir() + formatString("/project%d.workspace", i);
    if (fileExists(ws) && loadWorkspaceFrom(ws)) {
      *transient = "— workspace from the GUI";
      return;
    }
  }
  std::string p = promptStatus("Workspace (.workspace): ");
  if (p.empty()) return;
  std::string abs = standardizedAbsolutePath(p);
  *transient = loadWorkspaceFrom(abs) ? "— workspace loaded"
                                      : "— could not load the workspace";
}

// Expand a directory: (re)read its children — expansion IS the refresh.
static void ftExpand(const std::string &dirPath) {
  std::vector<npptui::WsNode> kids;
  npptui::ListFolderChildren(dirPath, kids);
  gFtCache[dirPath] = std::move(kids);
  gFtExpanded.insert(dirPath);
}

// The GUI's open roots first (FolderTreePanelRoots in the shared defaults),
// else a folder prompt. Roots start expanded.
// Linux: the roots are config.xml's <GUIConfig name="Workspace"><Root path=…/>.
static void folderTreePromptAndLoad(std::string *transient) {
  GuiConfigFacts facts = readGuiConfigFacts();
  for (const std::string &r : facts.workspaceRoots) {
    if (!g_file_test(r.c_str(), G_FILE_TEST_IS_DIR)) continue;
    gFtRoots.push_back(r);
    ftExpand(r);
  }
  if (!gFtRoots.empty()) {
    *transient = "— folders from the GUI";
    return;
  }
  std::string p = promptStatus("Folder: ");
  if (p.empty()) return;
  std::string abs = standardizedAbsolutePath(p);
  if (g_file_test(abs.c_str(), G_FILE_TEST_IS_DIR)) {
    gFtRoots.push_back(abs);
    ftExpand(abs);
    *transient = "— folder loaded";
  } else {
    *transient = "— not a folder";
  }
}

// Enter/click on a dock row, by panel kind.
static void panelActivate(size_t idx) {
  switch (gPanelKind) {
    case PanelKind::DocList:
      if (idx < gBuffers.size()) switchToBuffer(idx);
      break;
    case PanelKind::Functions:
      if (idx < gFuncItems.size()) SSM(SCI_GOTOLINE, gFuncItems[idx].line - 1, 0);
      break;
    case PanelKind::Project:
      if (idx >= gWsRowRefs.size()) break;
      if (gWsRowRefs[idx].isFile) {
        openPath(gWsRowRefs[idx].pathOrKey);
      } else if (!gWsRowRefs[idx].pathOrKey.empty()) {
        // Folder: toggle collapse.
        const std::string &key = gWsRowRefs[idx].pathOrKey;
        if (gWsCollapsed.count(key)) gWsCollapsed.erase(key);
        else gWsCollapsed.insert(key);
      } else {
        std::string t;
        projectPromptAndLoad(&t);  // the "(no workspace)" placeholder row
      }
      break;
    case PanelKind::FolderTree:
      if (idx >= gFtRowRefs.size()) break;
      if (gFtRowRefs[idx].isFile) {
        openPath(gFtRowRefs[idx].pathOrKey);
      } else if (!gFtRowRefs[idx].pathOrKey.empty()) {
        const std::string &path = gFtRowRefs[idx].pathOrKey;
        if (gFtExpanded.count(path)) gFtExpanded.erase(path);
        else ftExpand(path);  // expanding re-reads the directory
      } else {
        std::string t;
        folderTreePromptAndLoad(&t);  // the "(no folder)" placeholder row
      }
      break;
  }
}

static void cmdTogglePanel(PanelKind kind, std::string *transient) {
  const char *name = kind == PanelKind::DocList ? "Document List"
                     : kind == PanelKind::Functions ? "Function List"
                     : kind == PanelKind::Project ? "Project"
                                                  : "Folder as Workspace";
  if (gPanelVisible && gPanelKind == kind) {
    gPanelVisible = false;
    gPanelFocused = false;
    *transient = formatString("— %s hidden", name);
  } else {
    gPanelKind = kind;
    gPanelVisible = true;
    gPanel.selected = 0;
    gPanel.scrollOffset = 0;
    if (kind == PanelKind::Project && !gWsLoaded) projectPromptAndLoad(transient);
    if (kind == PanelKind::FolderTree && gFtRoots.empty())
      folderTreePromptAndLoad(transient);
    if (transient->empty())
      *transient = formatString("— %s shown (F6 focuses it)", name);
  }
  layoutWindows();
}

// Returns false when the user cancelled (Close All stops there).
static bool cmdCloseBuffer(std::string *transient) {
  if (SSM(SCI_GETMODIFY, 0, 0)) {
    std::string choice =
        unsavedChangesDialog("Close buffer", "Save and close", "Close without saving");
    if (choice.empty()) return false;
    if (choice == "save") {
      cmdSave(transient);
      if (SSM(SCI_GETMODIFY, 0, 0)) return false;  // save failed or Save-As cancelled
    }
  }
  sptr_t closing = gBuffers[gCur].doc;
  gBuffers.erase(gBuffers.begin() + (long)gCur);
  if (gBuffers.empty()) {
    TuiBuffer fresh;
    fresh.doc = SSM(SCI_CREATEDOCUMENT, 0, 0);
    fresh.loaded = true;  // an empty unnamed buffer has nothing to load
    gBuffers.push_back(fresh);
  }
  if (gCur >= gBuffers.size()) gCur = gBuffers.size() - 1;
  activateCurrentBuffer();
  SSM(SCI_RELEASEDOCUMENT, 0, closing);
  return true;
}

// New unnamed buffer (^N), like the GUI's File → New.
static void cmdNew(void) {
  TuiBuffer b;
  b.doc = SSM(SCI_CREATEDOCUMENT, 0, 0);
  b.loaded = true;  // nothing to load
  gBuffers.push_back(std::move(b));
  switchToBuffer(gBuffers.size() - 1);
}

// Save As — always prompts, keeps the buffer's encoding, re-detects the
// language from the new extension.
static void cmdSaveAs(std::string *transient) {
  std::string p = promptStatus("Save as: ");
  if (p.empty()) return;
  filePath = standardizedAbsolutePath(p);
  setupLanguage();
  *transient = saveFile(curEncoding, curHasBOM) ? "— saved" : "— SAVE FAILED";
  gBuffers[gCur].path = filePath;
  gBuffers[gCur].lang = langName;
}

// Reload from Disk — re-reads the file (re-detecting its encoding),
// confirming first when there are unsaved changes. Keeps caret/scroll.
static void cmdReload(std::string *transient) {
  if (filePath.empty()) {
    *transient = "— unnamed buffer: nothing to reload";
    return;
  }
  if (!fileExists(filePath)) {
    *transient = "— file is gone from disk";
    return;
  }
  if (SSM(SCI_GETMODIFY, 0, 0) &&
      !confirmDialog("Reload from disk", "Reload, discard changes"))
    return;
  long caret = SSM(SCI_GETCURRENTPOS, 0, 0);
  long first = SSM(SCI_GETFIRSTVISIBLELINE, 0, 0);
  TuiBuffer &b = gBuffers[gCur];
  bool bom = false;
  std::string data = readFileAsUtf8(filePath, &b.encoding, &bom);
  b.hasBOM = bom;
  curEncoding = b.encoding;
  curHasBOM = bom;
  SSM(SCI_CLEARALL, 0, 0);
  if (!data.empty()) SSM(SCI_APPENDTEXT, data.size(), data.data());
  SSM(SCI_EMPTYUNDOBUFFER, 0, 0);
  SSM(SCI_SETSAVEPOINT, 0, 0);
  b.dirtyCache = false;
  long len = SSM(SCI_GETLENGTH, 0, 0);
  SSM(SCI_GOTOPOS, caret < len ? caret : len, 0);
  SSM(SCI_SETFIRSTVISIBLELINE, first, 0);
  *transient = "— reloaded";
}

// Close All — closes buffers one at a time (each dirty one gets its own
// dialog, like the GUI); a Cancel stops the sweep.
static void cmdCloseAll(std::string *transient) {
  while (true) {
    bool last = gBuffers.size() == 1;
    if (last && gBuffers[0].path.empty() && !SSM(SCI_GETMODIFY, 0, 0) &&
        SSM(SCI_GETLENGTH, 0, 0) == 0)
      break;  // already the single fresh empty buffer
    if (!cmdCloseBuffer(transient)) return;
    if (last) break;  // the close above replaced it with a fresh one
  }
}

// ── Edit menu commands ────────────────────────────────────────────────────────

static void cmdCut(void) {
  SSM(SCI_CUT, 0, 0);
  bridgeClipboardOut();
}
static void cmdCopy(void) {
  SSM(SCI_COPYALLOWLINE, 0, 0);  // no selection = copy the line, GUI-style
  bridgeClipboardOut();
}
static void cmdPaste(void) {
  if (!bridgeClipboardIn()) SSM(SCI_PASTE, 0, 0);
}

static void cmdJoinLines(void) {
  long l1 = SSM(SCI_LINEFROMPOSITION, SSM(SCI_GETSELECTIONSTART, 0, 0), 0);
  long l2 = SSM(SCI_LINEFROMPOSITION, SSM(SCI_GETSELECTIONEND, 0, 0), 0);
  if (l1 == l2) l2 = l1 + 1;  // no selection: join with the next line
  if (l2 >= SSM(SCI_GETLINECOUNT, 0, 0)) return;
  SSM(SCI_SETTARGETRANGE, SSM(SCI_POSITIONFROMLINE, l1, 0),
      SSM(SCI_GETLINEENDPOSITION, l2, 0));
  SSM(SCI_LINESJOIN, 0, 0);
}

static void cmdTrimTrailing(std::string *transient) {
  long lines = SSM(SCI_GETLINECOUNT, 0, 0);
  long trimmed = 0;
  SSM(SCI_BEGINUNDOACTION, 0, 0);
  for (long l = 0; l < lines; l++) {
    long start = SSM(SCI_POSITIONFROMLINE, l, 0);
    long end = SSM(SCI_GETLINEENDPOSITION, l, 0);
    long p = end;
    while (p > start) {
      int ch = (int)SSM(SCI_GETCHARAT, p - 1, 0);
      if (ch != ' ' && ch != '\t') break;
      p--;
    }
    if (p < end) {
      SSM(SCI_DELETERANGE, p, end - p);
      trimmed++;
    }
  }
  SSM(SCI_ENDUNDOACTION, 0, 0);
  *transient = formatString("— trimmed %ld line(s)", trimmed);
}

static void cmdConvertEol(int mode, std::string *transient) {
  SSM(SCI_CONVERTEOLS, mode, 0);
  SSM(SCI_SETEOLMODE, mode, 0);
  *transient = mode == SC_EOL_CRLF ? "— EOL: Windows (CR LF)"
             : mode == SC_EOL_LF   ? "— EOL: Unix (LF)"
                                   : "— EOL: Macintosh (CR)";
}

// Line-comment toggle (langs.xml commentLine, e.g. "//"): if every
// non-blank selected line is already commented, uncomment; else comment.
// The marker goes at the first non-blank column; removal also eats one
// following space (the one insertion adds).
static long firstNonBlankPos(long line) {  // -1 = blank line
  long start = SSM(SCI_POSITIONFROMLINE, line, 0);
  long end = SSM(SCI_GETLINEENDPOSITION, line, 0);
  for (long p = start; p < end; p++) {
    int ch = (int)SSM(SCI_GETCHARAT, p, 0);
    if (ch != ' ' && ch != '\t') return p;
  }
  return -1;
}

static bool textAtPosIs(long pos, const std::string &s) {
  for (size_t i = 0; i < s.size(); i++)
    if ((int)SSM(SCI_GETCHARAT, pos + (long)i, 0) != (unsigned char)s[i]) return false;
  return true;
}

// Linux: the langs.xml comment markers; a UDL has none in langs.xml.
static std::string langCommentLine(void) {
  const char *s = langName.empty() || isUdlLang(langName) ? nullptr
                                                          : langsmgr_comment_line(langName.c_str());
  return s ? s : "";
}
static std::string langCommentStart(void) {
  const char *s = langName.empty() || isUdlLang(langName) ? nullptr
                                                          : langsmgr_comment_start(langName.c_str());
  return s ? s : "";
}
static std::string langCommentEnd(void) {
  const char *s = langName.empty() || isUdlLang(langName) ? nullptr
                                                          : langsmgr_comment_end(langName.c_str());
  return s ? s : "";
}

static void cmdToggleComment(std::string *transient) {
  std::string pre = langCommentLine();
  if (pre.empty()) {
    *transient = "— no line comment defined for this language";
    return;
  }
  long selStart = SSM(SCI_GETSELECTIONSTART, 0, 0);
  long selEnd = SSM(SCI_GETSELECTIONEND, 0, 0);
  long l1 = SSM(SCI_LINEFROMPOSITION, selStart, 0);
  long l2 = SSM(SCI_LINEFROMPOSITION, selEnd, 0);
  // A selection ending exactly at a line start does not include that line.
  if (l2 > l1 && SSM(SCI_POSITIONFROMLINE, l2, 0) == selEnd) l2--;
  bool any = false, allCommented = true;
  for (long l = l1; l <= l2; l++) {
    long p = firstNonBlankPos(l);
    if (p < 0) continue;
    any = true;
    if (!textAtPosIs(p, pre)) {
      allCommented = false;
      break;
    }
  }
  if (!any) return;
  std::string insert = pre + " ";
  SSM(SCI_BEGINUNDOACTION, 0, 0);
  for (long l = l1; l <= l2; l++) {
    long p = firstNonBlankPos(l);
    if (p < 0) continue;
    if (allCommented) {
      long n = (long)pre.size();
      if ((int)SSM(SCI_GETCHARAT, p + n, 0) == ' ') n++;
      SSM(SCI_DELETERANGE, p, n);
    } else {
      SSM(SCI_INSERTTEXT, p, insert.c_str());
    }
  }
  SSM(SCI_ENDUNDOACTION, 0, 0);
}

static void cmdBlockComment(std::string *transient) {
  std::string s = langCommentStart(), e = langCommentEnd();
  if (s.empty() || e.empty()) {
    *transient = "— no block comment defined for this language";
    return;
  }
  long selStart = SSM(SCI_GETSELECTIONSTART, 0, 0);
  long selEnd = SSM(SCI_GETSELECTIONEND, 0, 0);
  if (selStart == selEnd) {
    *transient = "— select text to block-comment";
    return;
  }
  long sLen = (long)s.size(), eLen = (long)e.size();
  SSM(SCI_BEGINUNDOACTION, 0, 0);
  if (selEnd - selStart >= sLen + eLen && textAtPosIs(selStart, s) &&
      textAtPosIs(selEnd - eLen, e)) {
    SSM(SCI_DELETERANGE, selEnd - eLen, eLen);
    SSM(SCI_DELETERANGE, selStart, sLen);
    SSM(SCI_SETSEL, selStart, selEnd - sLen - eLen);
  } else {
    SSM(SCI_INSERTTEXT, selEnd, e.c_str());
    SSM(SCI_INSERTTEXT, selStart, s.c_str());
    SSM(SCI_SETSEL, selStart, selEnd + sLen + eLen);
  }
  SSM(SCI_ENDUNDOACTION, 0, 0);
}

// Copy to Clipboard (GUI Edit submenu): into both Scintilla's internal
// clipboard (so ^V works everywhere) and the system one.
static void cmdCopyPathText(const std::string &text, std::string *transient) {
  if (text.empty()) {
    *transient = "— unnamed buffer has no path";
    return;
  }
  SSM(SCI_COPYTEXT, text.size(), text.c_str());
  systemClipboardSet(text.c_str(), text.size());
  *transient = "— copied";
}

static void cmdToggleWrap(std::string *transient) {
  long mode = SSM(SCI_GETWRAPMODE, 0, 0);
  SSM(SCI_SETWRAPMODE, mode == SC_WRAP_NONE ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
  *transient = mode == SC_WRAP_NONE ? "— wrap on" : "— wrap off";
}

// NSString integerValue: leading whitespace, optional sign, digits; 0 otherwise.
static long integerValue(const std::string &s) { return strtol(s.c_str(), nullptr, 10); }

static void cmdGotoLine(void) {
  std::string s = promptStatus("Go to line: ");
  if (!s.empty()) SSM(SCI_GOTOLINE, integerValue(s) - 1, 0);
}

// ── Search menu: brace + bookmarks ────────────────────────────────────────────

static void cmdGotoBrace(std::string *transient) {
  auto isBrace = [](int c) { return c && strchr("()[]{}<>", c) != nullptr; };
  long pos = SSM(SCI_GETCURRENTPOS, 0, 0);
  long bracePos = -1;
  if (isBrace((int)SSM(SCI_GETCHARAT, pos, 0))) bracePos = pos;
  else if (pos > 0 && isBrace((int)SSM(SCI_GETCHARAT, pos - 1, 0))) bracePos = pos - 1;
  if (bracePos < 0) {
    *transient = "— no brace at the caret";
    return;
  }
  long match = SSM(SCI_BRACEMATCH, bracePos, 0);
  if (match < 0) {
    *transient = "— no matching brace";
    return;
  }
  SSM(SCI_GOTOPOS, match, 0);
}

static void cmdToggleBookmark(void) {
  long line = SSM(SCI_LINEFROMPOSITION, SSM(SCI_GETCURRENTPOS, 0, 0), 0);
  if (SSM(SCI_MARKERGET, line, 0) & (1 << kBookmarkMarker))
    SSM(SCI_MARKERDELETE, line, kBookmarkMarker);
  else
    SSM(SCI_MARKERADD, line, kBookmarkMarker);
}

static void cmdNextBookmark(std::string *transient) {
  long line = SSM(SCI_LINEFROMPOSITION, SSM(SCI_GETCURRENTPOS, 0, 0), 0);
  long found = SSM(SCI_MARKERNEXT, line + 1, 1 << kBookmarkMarker);
  if (found < 0) found = SSM(SCI_MARKERNEXT, 0, 1 << kBookmarkMarker);  // wrap
  if (found < 0) *transient = "— no bookmarks";
  else SSM(SCI_GOTOLINE, found, 0);
}

static void cmdPrevBookmark(std::string *transient) {
  long line = SSM(SCI_LINEFROMPOSITION, SSM(SCI_GETCURRENTPOS, 0, 0), 0);
  long found = line > 0 ? SSM(SCI_MARKERPREVIOUS, line - 1, 1 << kBookmarkMarker) : -1;
  if (found < 0)
    found = SSM(SCI_MARKERPREVIOUS, SSM(SCI_GETLINECOUNT, 0, 0) - 1,
                1 << kBookmarkMarker);  // wrap
  if (found < 0) *transient = "— no bookmarks";
  else SSM(SCI_GOTOLINE, found, 0);
}

static void cmdClearBookmarks(void) { SSM(SCI_MARKERDELETEALL, kBookmarkMarker, 0); }

// ── Encoding menu ─────────────────────────────────────────────────────────────

// GUI "Convert to …" semantics, terminal edition: the buffer text (always
// UTF-8 in the editor) is unchanged; the ON-DISK encoding for the next
// save changes, immediately visible in the status bar. Strict conversion
// still applies at save time — characters the target cannot hold fail the
// save rather than mangle.
static void cmdConvertEncoding(const TuiEncoding &enc, bool bom, std::string *transient) {
  TuiBuffer &b = gBuffers[gCur];
  b.encoding = enc;
  b.hasBOM = bom;
  curEncoding = enc;
  curHasBOM = bom;
  *transient = "— will save as " + encodingDisplayName(enc, bom);
}

// ── View menu toggles + Summary ───────────────────────────────────────────────

static void cmdToggleLineNumbers(std::string *transient) {
  gShowLineNumbers = !gShowLineNumbers;
  updateLineNumberMargin();
  *transient = gShowLineNumbers ? "— line numbers on" : "— line numbers off";
}

static void cmdToggleWhitespace(std::string *transient) {
  bool on = SSM(SCI_GETVIEWWS, 0, 0) != SCWS_INVISIBLE;
  SSM(SCI_SETVIEWWS, on ? SCWS_INVISIBLE : SCWS_VISIBLEALWAYS, 0);
  *transient = on ? "— white space hidden" : "— white space shown";
}

static void cmdToggleEolView(std::string *transient) {
  bool on = SSM(SCI_GETVIEWEOL, 0, 0) != 0;
  SSM(SCI_SETVIEWEOL, !on, 0);
  *transient = on ? "— line endings hidden" : "— line endings shown";
}

static void cmdToggleIndentGuides(std::string *transient) {
  bool on = SSM(SCI_GETINDENTATIONGUIDES, 0, 0) != SC_IV_NONE;
  SSM(SCI_SETINDENTATIONGUIDES, on ? SC_IV_NONE : SC_IV_LOOKBOTH, 0);
  *transient = on ? "— indent guides hidden" : "— indent guides shown";
}

// View → Summary… (GUI parity): document statistics in a text popup.
static void cmdSummary(void) {
  long bytes = SSM(SCI_GETLENGTH, 0, 0);
  long chars = SSM(SCI_COUNTCHARACTERS, 0, bytes);
  long lines = SSM(SCI_GETLINECOUNT, 0, 0);
  const char *buf = (const char *)SSM(SCI_GETCHARACTERPOINTER, 0, 0);
  long words = 0;
  bool inWord = false;
  for (long i = 0; i < bytes; i++) {
    unsigned char c = (unsigned char)buf[i];
    bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    if (!ws && !inWord) words++;
    inWord = !ws;
  }
  long selLen = SSM(SCI_GETSELECTIONEND, 0, 0) - SSM(SCI_GETSELECTIONSTART, 0, 0);
  long eol = SSM(SCI_GETEOLMODE, 0, 0);
  std::vector<std::string> out;
  out.push_back(std::string("Full file path: ") +
                (!filePath.empty() ? filePath : "(untitled)"));
  out.push_back("");
  out.push_back("Characters: " + std::to_string(chars));
  out.push_back("Words: " + std::to_string(words));
  out.push_back("Lines: " + std::to_string(lines));
  out.push_back("Document length (bytes): " + std::to_string(bytes));
  out.push_back(std::string("EOL format: ") +
                (eol == SC_EOL_CRLF ? "Windows (CR LF)"
                 : eol == SC_EOL_CR ? "Macintosh (CR)"
                                    : "Unix (LF)"));
  out.push_back(std::string("Encoding: ") + encodingDisplayName(curEncoding, curHasBOM));
  out.push_back(std::string("Language: ") + (!langName.empty() ? langName : "text"));
  if (selLen > 0) out.push_back("Selection (bytes): " + std::to_string(selLen));
  npptui::Popup popup = npptui::Popup::text(std::move(out));
  popup.title = " Summary ";
  popup.position = npptui::PopupPosition::centered();
  popup.width = std::min(COLS - 4, 64);
  popup.maxHeight = 14;
  runTextPopupModal(popup);
}

// Toggle the fold at the caret — from anywhere inside the block, not just
// its header line (Notepad++ behavior).
static void cmdToggleFold(void) {
  long line = SSM(SCI_LINEFROMPOSITION, SSM(SCI_GETCURRENTPOS, 0, 0), 0);
  if (!(SSM(SCI_GETFOLDLEVEL, line, 0) & SC_FOLDLEVELHEADERFLAG))
    line = SSM(SCI_GETFOLDPARENT, line, 0);
  if (line >= 0) SSM(SCI_TOGGLEFOLD, line, 0);
}

static void cmdFoldAll(void) { SSM(SCI_FOLDALL, SC_FOLDACTION_CONTRACT, 0); }
static void cmdUnfoldAll(void) { SSM(SCI_FOLDALL, SC_FOLDACTION_EXPAND, 0); }

// Linux: the GUI executable — the installed `nextpad-plus-plus` symlink, the
// `Nextpad++` binary on PATH, else a `Nextpad++` sibling of this executable
// (a build tree). Empty when none is found.
static std::string guiExecutablePath(void) {
  for (const char *name : {"nextpad-plus-plus", "Nextpad++"}) {
    gchar *p = g_find_program_in_path(name);
    if (p) {
      std::string s = p;
      g_free(p);
      return s;
    }
  }
  gchar *self = g_file_read_link("/proc/self/exe", nullptr);
  if (self) {
    std::string sibling = deletingLastPathComponent(self) + "/Nextpad++";
    g_free(self);
    if (g_file_test(sibling.c_str(), G_FILE_TEST_IS_EXECUTABLE)) return sibling;
  }
  return "";
}

// Hand the current file to the GUI app (Phase 4). Saves first so the GUI
// sees this buffer's content. `open --args` is silently dropped by a running
// app, so the caret line-jump (-n) only happens on a fresh GUI launch.
// Linux: the GUI is single-instance (G_APPLICATION_HANDLES_OPEN) — one
// g_spawn_async with the glued "-n<line>" reaches a running instance or
// starts one; no pgrep probe.
static void cmdOpenInGui(std::string *transient) {
  if (filePath.empty()) {
    *transient = "— unnamed buffer: save it first";
    return;
  }
  if (SSM(SCI_GETMODIFY, 0, 0) && !saveFile(curEncoding, curHasBOM)) {
    *transient = "— SAVE FAILED (not handed off)";
    return;
  }
  std::string exe = guiExecutablePath();
  if (exe.empty()) {
    *transient = "— Nextpad++ app not found";
    return;
  }
  long line = SSM(SCI_LINEFROMPOSITION, SSM(SCI_GETCURRENTPOS, 0, 0), 0) + 1;
  std::string lineArg = formatString("-n%ld", line);
  gchar *argv[] = {(gchar *)exe.c_str(), (gchar *)lineArg.c_str(), (gchar *)filePath.c_str(),
                   nullptr};
  GError *err = nullptr;
  bool ok = g_spawn_async(nullptr, argv, nullptr,
                          (GSpawnFlags)(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                          nullptr, nullptr, nullptr, &err);
  if (err) g_error_free(err);
  *transient = ok ? "— opened in Nextpad++ (GUI)" : "— could not launch Nextpad++";
}

struct TuiCommand {
  const char *id, *title, *shortcut;
};
static void cmdLanguageSelect(void);  // needs the picker, defined below
static void cmdPreferences(std::string *transient);  // ditto
static const TuiCommand kCommands[] = {
    {"new", "New", "^N"},
    {"open-file", "Open File…", "^O"},
    {"reload", "Reload from Disk", ""},
    {"save", "Save", "^S"},
    {"save-as", "Save As…", ""},
    {"close-buffer", "Close Buffer", "^W"},
    {"close-all", "Close All", ""},
    {"next-buffer", "Next Buffer", "^⇥"},
    {"prev-buffer", "Previous Buffer", "^⇧⇥"},
    {"find", "Find…", "^F"},
    {"replace", "Replace…", "^R"},
    {"find-next", "Find Next", "F3"},
    {"find-prev", "Find Previous", "⇧F3"},
    {"goto-line", "Go to Line…", "^G"},
    {"goto-brace", "Go to Matching Brace", ""},
    {"toggle-bookmark", "Toggle Bookmark", "^F2"},
    {"next-bookmark", "Next Bookmark", "F2"},
    {"prev-bookmark", "Previous Bookmark", "⇧F2"},
    {"clear-bookmarks", "Clear All Bookmarks", ""},
    {"toggle-fold", "Toggle Fold", "^B"},
    {"fold-all", "Fold All", ""},
    {"unfold-all", "Unfold All", ""},
    {"toggle-wrap", "Toggle Word Wrap", "^T"},
    {"toggle-line-numbers", "Show Line Numbers", ""},
    {"toggle-whitespace", "Show White Space", ""},
    {"toggle-eol-view", "Show End of Line", ""},
    {"toggle-indent-guides", "Show Indent Guide", ""},
    {"summary", "Summary…", ""},
    {"toggle-doclist", "Document List Panel", "F6"},
    {"toggle-funclist", "Function List Panel", ""},
    {"toggle-project", "Project Panel", ""},
    {"toggle-foldertree", "Folder as Workspace Panel", ""},
    {"enc-utf8", "Convert to UTF-8", ""},
    {"enc-utf8-bom", "Convert to UTF-8-BOM", ""},
    {"enc-utf16le", "Convert to UTF-16 LE BOM", ""},
    {"enc-utf16be", "Convert to UTF-16 BE BOM", ""},
    {"enc-cp1252", "Convert to Windows-1252 (ANSI)", ""},
    {"undo", "Undo", "^Z"},
    {"redo", "Redo", "^Y"},
    {"cut", "Cut", "^X"},
    {"copy", "Copy", "^C"},
    {"paste", "Paste", "^V"},
    {"select-all", "Select All", "^A"},
    {"duplicate-line", "Duplicate Current Line", "^D"},
    {"delete-line", "Delete Current Line", ""},
    {"move-line-up", "Move Current Line Up", ""},
    {"move-line-down", "Move Current Line Down", ""},
    {"join-lines", "Join Lines", ""},
    {"trim-trailing", "Trim Trailing Space", ""},
    {"uppercase", "UPPERCASE", "^⇧U"},
    {"lowercase", "lowercase", "^U"},
    {"indent", "Increase Line Indent", ""},
    {"outdent", "Decrease Line Indent", ""},
    {"toggle-comment", "Toggle Single Line Comment", ""},
    {"block-comment", "Toggle Block Comment", ""},
    {"eol-crlf", "EOL Conversion: Windows (CR LF)", ""},
    {"eol-lf", "EOL Conversion: Unix (LF)", ""},
    {"eol-cr", "EOL Conversion: Macintosh (CR)", ""},
    {"copy-path", "Copy File Path", ""},
    {"copy-filename", "Copy File Name", ""},
    {"copy-dirpath", "Copy Directory Path", ""},
    {"save-all", "Save All", ""},
    {"open-in-gui", "Open in Nextpad++ (GUI)", ""},
    {"buffers-list", "Buffers…", "^L"},
    {"language-select", "Set Language…", ""},
    {"preferences", "Preferences…", ""},
    {"help", "Help — Key Bindings", "F1"},
    {"quit", "Quit", "^Q"},
    {"quit-discard", "Quit, Discard Unsaved Changes", ""},
};

// Help card (F1): every command with its key, plus the bindings that are not
// commands. Plain text popup — scrolls, Esc/Enter/q closes.
static void cmdHelp(void) {
  std::vector<std::string> lines;
  auto row = [&](const std::string &title, const std::string &key) {
    std::string l = "  " + title;
    int pad = 36 - npptui::DisplayWidthUtf8(l);
    l.append((size_t)std::max(pad, 1), ' ');
    l += key;
    lines.push_back(std::move(l));
  };
  lines.push_back("Every command is also in the palette (^P)");
  lines.push_back("and the menus (F10 or click the top row).");
  lines.push_back("");
  for (const TuiCommand &c : kCommands) row(c.title, c.shortcut[0] ? c.shortcut : "—");
  row("Command Palette…", "^P");
  row("Menu Bar", "F10");
  lines.push_back("");
  lines.push_back("Quit is instant (hot exit): unsaved changes");
  lines.push_back("are snapshotted and restored on relaunch.");
  npptui::Popup popup = npptui::Popup::text(std::move(lines));
  popup.title = " Nextpad++ — Help ";
  popup.position = npptui::PopupPosition::centered();
  popup.width = 48;
  popup.maxHeight = LINES > 8 ? LINES - 4 : 4;
  runTextPopupModal(popup);
}

static void dispatchCommand(const std::string &id, bool &running, std::string *transient) {
  if (id == "save") cmdSave(transient);
  else if (id == "new") cmdNew();
  else if (id == "save-as") cmdSaveAs(transient);
  else if (id == "reload") cmdReload(transient);
  else if (id == "close-all") cmdCloseAll(transient);
  else if (id == "quit") cmdQuit(running, transient);
  else if (id == "undo") SSM(SCI_UNDO, 0, 0);
  else if (id == "redo") SSM(SCI_REDO, 0, 0);
  else if (id == "cut") cmdCut();
  else if (id == "copy") cmdCopy();
  else if (id == "paste") cmdPaste();
  else if (id == "duplicate-line") SSM(SCI_SELECTIONDUPLICATE, 0, 0);
  else if (id == "delete-line") SSM(SCI_LINEDELETE, 0, 0);
  else if (id == "move-line-up") SSM(SCI_MOVESELECTEDLINESUP, 0, 0);
  else if (id == "move-line-down") SSM(SCI_MOVESELECTEDLINESDOWN, 0, 0);
  else if (id == "join-lines") cmdJoinLines();
  else if (id == "trim-trailing") cmdTrimTrailing(transient);
  else if (id == "uppercase") SSM(SCI_UPPERCASE, 0, 0);
  else if (id == "lowercase") SSM(SCI_LOWERCASE, 0, 0);
  else if (id == "indent") SSM(SCI_TAB, 0, 0);
  else if (id == "outdent") SSM(SCI_BACKTAB, 0, 0);
  else if (id == "toggle-comment") cmdToggleComment(transient);
  else if (id == "block-comment") cmdBlockComment(transient);
  else if (id == "eol-crlf") cmdConvertEol(SC_EOL_CRLF, transient);
  else if (id == "eol-lf") cmdConvertEol(SC_EOL_LF, transient);
  else if (id == "eol-cr") cmdConvertEol(SC_EOL_CR, transient);
  else if (id == "copy-path") cmdCopyPathText(filePath, transient);
  else if (id == "copy-filename")
    cmdCopyPathText(filePath.empty() ? "" : lastPathComponent(filePath), transient);
  else if (id == "copy-dirpath")
    cmdCopyPathText(filePath.empty() ? "" : deletingLastPathComponent(filePath), transient);
  else if (id == "toggle-wrap") cmdToggleWrap(transient);
  else if (id == "goto-line") cmdGotoLine();
  else if (id == "select-all") SSM(SCI_SELECTALL, 0, 0);
  else if (id == "open-file") cmdOpenFile();
  else if (id == "close-buffer") cmdCloseBuffer(transient);
  else if (id == "next-buffer") cmdNextBuffer();
  else if (id == "prev-buffer") cmdPrevBuffer();
  else if (id == "find") runFindPopup();
  else if (id == "replace") runReplacePopup(transient);
  else if (id == "find-next") cmdFindNext(transient);
  else if (id == "find-prev") cmdFindPrev(transient);
  else if (id == "save-all") cmdSaveAll(transient);
  else if (id == "quit-discard") cmdQuitDiscard(running);
  else if (id == "toggle-line-numbers") cmdToggleLineNumbers(transient);
  else if (id == "toggle-whitespace") cmdToggleWhitespace(transient);
  else if (id == "toggle-eol-view") cmdToggleEolView(transient);
  else if (id == "toggle-indent-guides") cmdToggleIndentGuides(transient);
  else if (id == "summary") cmdSummary();
  else if (id == "toggle-doclist") cmdTogglePanel(PanelKind::DocList, transient);
  else if (id == "toggle-funclist") cmdTogglePanel(PanelKind::Functions, transient);
  else if (id == "toggle-project") cmdTogglePanel(PanelKind::Project, transient);
  else if (id == "toggle-foldertree") cmdTogglePanel(PanelKind::FolderTree, transient);
  else if (id == "enc-utf8") cmdConvertEncoding(kEncUtf8, false, transient);
  else if (id == "enc-utf8-bom") cmdConvertEncoding(kEncUtf8, true, transient);
  else if (id == "enc-utf16le") cmdConvertEncoding(kEncUtf16LE, true, transient);
  else if (id == "enc-utf16be") cmdConvertEncoding(kEncUtf16BE, true, transient);
  else if (id == "enc-cp1252") cmdConvertEncoding(kEncCp1252, false, transient);
  else if (id == "toggle-fold") cmdToggleFold();
  else if (id == "fold-all") cmdFoldAll();
  else if (id == "unfold-all") cmdUnfoldAll();
  else if (id == "goto-brace") cmdGotoBrace(transient);
  else if (id == "toggle-bookmark") cmdToggleBookmark();
  else if (id == "next-bookmark") cmdNextBookmark(transient);
  else if (id == "prev-bookmark") cmdPrevBookmark(transient);
  else if (id == "clear-bookmarks") cmdClearBookmarks();
  else if (id == "buffers-list") cmdBuffersList();
  else if (id == "help") cmdHelp();
  else if (id == "open-in-gui") cmdOpenInGui(transient);
  else if (id == "language-select") cmdLanguageSelect();
  else if (id == "preferences") cmdPreferences(transient);
}

// ── command palette (T2.6): ^P — fuzzy-filtered list of every command ─────────

// Fuzzy-filtered picker (the palette UI, generalized): type-to-filter over
// `entries`, Enter/click returns the chosen id, Esc returns "". A non-zero
// toggleChord closes the picker on that Ctrl chord (^P for the palette).
static std::string runFilteredPicker(const std::vector<npptui::PaletteEntry> &entries,
                                     const char *title, char32_t toggleChord) {
  ModalScope modalScope;
  std::string query;
  npptui::Popup popup = npptui::Popup::list({});
  popup.title = title;
  popup.showInput = true;
  popup.width = 44;
  popup.maxHeight = 12;

  auto rebuild = [&](bool resetSelection) {
    std::vector<size_t> filtered = npptui::PaletteFilter(entries, query);
    std::vector<npptui::PopupListItem> items;
    for (size_t idx : filtered) {
      npptui::PopupListItem it(entries[idx].title);
      it.data = entries[idx].id;
      it.detail = entries[idx].shortcut;
      items.push_back(std::move(it));
    }
    popup.items = std::move(items);
    if (resetSelection || popup.selected >= popup.items.size()) {
      popup.selected = 0;
      popup.scrollOffset = 0;
    }
    popup.input = query;
    // Near the top like fresh's palette; recomputed each frame for resizes.
    int w = popup.width < COLS ? popup.width : COLS;
    popup.position = npptui::PopupPosition::fixed((COLS - w) / 2, 2);
  };

  WINDOW *popWin = nullptr;
  npptui::Rect area{};
  std::string chosen;
  bool done = false;
  rebuild(true);
  renderModalPopup(popup, popWin, area);
  while (!done) {
    Event e;
    if (!nextEvent(e)) break;
    if (e.type == Event::Type::Key) {
      const npptui::KeyEvent &k = e.key.event;
      if (k.kind == npptui::KeyEventKind::Release) continue;
      switch (k.code.kind) {
        case KeyKind::Esc: done = true; break;
        case KeyKind::Enter:
          if (const npptui::PopupListItem *item = popup.selectedItem()) chosen = item->data;
          done = true;
          break;
        case KeyKind::Up: popup.selectPrev(); break;
        case KeyKind::Down: popup.selectNext(); break;
        case KeyKind::Home: popup.selectFirst(); break;
        case KeyKind::End: popup.selectLast(); break;
        case KeyKind::PageUp: popup.pageUp(); break;
        case KeyKind::PageDown: popup.pageDown(); break;
        case KeyKind::Backspace:
          // Drop the last code point, not the last byte.
          while (!query.empty()) {
            unsigned char last = (unsigned char)query.back();
            query.pop_back();
            if (last < 0x80 || last >= 0xC0) break;
          }
          rebuild(true);
          break;
        case KeyKind::Char:
          if (toggleChord && k.mods == npptui::kModCtrl &&
              (k.code.ch == toggleChord || k.code.ch == toggleChord - 32)) {
            done = true;  // the opening chord toggles the picker closed
          } else if (!(k.mods & (npptui::kModCtrl | npptui::kModAlt)) && k.code.ch >= 32) {
            utf8Append(query, k.code.ch);
            rebuild(true);
          }
          break;
        default: break;
      }
    } else if (e.type == Event::Type::Paste) {
      for (char c : e.paste)
        if ((unsigned char)c >= 32 || (unsigned char)c >= 0x80) query += c;
      rebuild(true);
    } else if (e.type == Event::Type::Mouse) {
      const npptui::MouseEvent &m = e.mouse;
      if (m.kind == npptui::MouseKind::ScrollUp) popup.scrollBy(-3);
      else if (m.kind == npptui::MouseKind::ScrollDown) popup.scrollBy(3);
      else if (m.kind == npptui::MouseKind::Down) {
        if (npptui::PointInRect(area, m.column, m.row)) {
          int row = m.row - area.y - (popup.bordered ? 1 : 0) - (popup.showInput ? 1 : 0);
          size_t idx = popup.scrollOffset + (size_t)(row < 0 ? 0 : row);
          if (row >= 0 && popup.selectIndex(idx)) {
            chosen = popup.items[idx].data;
            done = true;
          }
        } else {
          done = true;
        }
      }
    }
    if (!done) renderModalPopup(popup, popWin, area);
  }
  if (popWin) delwin(popWin);
  curs_set(1);
  return chosen;
}

static void runCommandPalette(bool &running, std::string *transient) {
  std::vector<npptui::PaletteEntry> entries;
  for (const TuiCommand &c : kCommands) entries.push_back({c.id, c.title, c.shortcut});
  std::string chosen = runFilteredPicker(entries, " Commands ", 'p');
  if (!chosen.empty()) dispatchCommand(chosen, running, transient);
}

// Language → Set Language… — the GUI's Language menu as a filterable
// picker over every built-in language (captions match the Windows menu).
// Linux: the languages are langs.xml's (langsmgr), captioned like the GUI's
// Language menu (langcaptions.c, else the status-bar name), followed by the
// User Defined Languages.
static void cmdLanguageSelect(void) {
  std::vector<npptui::PaletteEntry> entries;
  int count = 0;
  const char **all = langsmgr_all_languages(&count);
  for (int i = 0; i < count; i++) {
    const char *caption = lang_caption(all[i]);
    entries.push_back({all[i], caption ? caption : lexer_display_name(all[i]), ""});
  }
  udl_load_all();
  for (int i = 0; i < udl_count(); i++)
    entries.push_back({udl_key(i), udl_name(i), "UDL"});
  std::string chosen = runFilteredPicker(entries, " Language ", 0);
  if (chosen.empty()) return;
  applyLanguage(chosen);
  gBuffers[gCur].lang = langName;
}

// ── Preferences (Phase 4.5): ONE settings store, two frontends ────────────────
// The popup edits the SAME defaults domain the GUI reads (CFPreferences
// writes), so there is no TUI-only settings file to diverge: the TUI
// applies immediately, the GUI picks the values up on its next read.
// Linux: the store is config.xml; each macOS defaults key maps onto the
// GUIConfig group/attribute the GUI itself persists (src/prefs.c), written
// through the shim's read-modify-write SetGuiConfigAttr.

static void guiPrefWrite(const std::string &key, const std::string &value) {
  std::string group, attr, v = value;
  auto yesNo = [](const std::string &b) { return b == "1" ? "yes" : "no"; };
  auto showHide = [](const std::string &b) { return b == "1" ? "show" : "hide"; };
  if (key == "tabWidth") { group = "TabSetting"; attr = "size"; }
  else if (key == "useTabs") { group = "TabSetting"; attr = "replaceBySpace"; v = yesNo(value == "1" ? "0" : "1"); }
  else if (key == "wordWrap") { group = "ScintillaPrimaryView"; attr = "Wrap"; v = yesNo(value); }
  else if (key == "showIndentGuides") { group = "ScintillaPrimaryView"; attr = "indentGuideLine"; v = showHide(value); }
  else if (key == "showWhitespace") { group = "ScintillaPrimaryView"; attr = "whiteSpaceShow"; v = showHide(value); }
  else if (key == "showEOL") { group = "ScintillaPrimaryView"; attr = "eolShow"; v = showHide(value); }
  else if (key == "edgeColumn") { group = "ScintillaPrimaryView"; attr = "edgeMultiColumnPos"; }
  else if (key == "edgeMode") return;  // implied by edgeMultiColumnPos (0 = off) in the GUI's schema
  else if (key == "NPPActiveTheme") { group = "DarkMode"; attr = "theme"; }
  else return;
  npptui::SetGuiConfigAttr(guiDomain(), group, attr, v);
}

static std::string activeThemeName(void) {
  GuiConfigFacts facts = readGuiConfigFacts();
  return facts.theme.empty() ? kDefaultThemeName : facts.theme;
}

// Theme names the GUI would offer: user themes dir first, then the app
// bundle's, with the stylers.xml default pinned on top.
static std::vector<std::string> availableThemeNames(void) {
  std::vector<std::string> names = {kDefaultThemeName};
  std::set<std::string> seen;
  gchar *bundled = npp_bundle_file(nullptr, "themes");
  std::vector<std::string> dirs = {guiDataDir() + "/themes", bundled};
  g_free(bundled);
  for (const std::string &dir : dirs) {
    GDir *d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) continue;
    std::vector<std::string> files;
    while (const gchar *f = g_dir_read_name(d)) files.push_back(f);
    g_dir_close(d);
    std::sort(files.begin(), files.end());
    for (const std::string &f : files) {
      if (f.size() < 4 || f.compare(f.size() - 4, 4, ".xml") != 0) continue;
      std::string n = f.substr(0, f.size() - 4);
      if (seen.count(n)) continue;
      seen.insert(n);
      names.push_back(n);
    }
  }
  return names;
}

// After an NPPActiveTheme write: re-resolve, re-point the watch, reload.
static void applyThemeSelection(void) {
  std::string newPath = resolveThemePath();
  if (newPath != gThemePath) {
    gThemePath = newPath;
    if (gInteropWatch && !gGuiConfigPath.empty()) resetInteropWatch();
  }
  reloadThemeFromPath();
}

static void cmdPreferences(std::string *transient) {
  auto onOff = [](int v) {
    return std::string(v == 1 ? "on" : v == 0 ? "off" : "default");
  };
  while (true) {
    std::vector<npptui::PopupListItem> items;
    auto add = [&](const std::string &label, const char *id) {
      npptui::PopupListItem it(label);
      it.data = id;
      items.push_back(std::move(it));
    };
    add("Tab Width: " + (gGuiPrefs.tabWidth > 0 ? std::to_string(gGuiPrefs.tabWidth)
                                                : std::string("default")),
        "tabw");
    add("Use Tabs (not spaces): " + onOff(gGuiPrefs.useTabs), "usetabs");
    add("Word Wrap: " + onOff(gGuiPrefs.wordWrap), "wrap");
    add("Show Indent Guides: " + onOff(gGuiPrefs.indentGuides), "guides");
    add("Show White Space: " + onOff(gGuiPrefs.showWhitespace), "ws");
    add("Show End of Line: " + onOff(gGuiPrefs.showEOL), "eol");
    add("Edge Column: " + (gGuiPrefs.edgeColumn >= 0
                               ? std::to_string(gGuiPrefs.edgeColumn)
                               : std::string("default")),
        "edge");
    add(std::string("Theme: ") + activeThemeName(), "theme");
    npptui::Popup popup = npptui::Popup::list(std::move(items));
    popup.title = " Preferences — shared with the GUI ";
    popup.position = npptui::PopupPosition::centered();
    popup.width = 46;
    popup.maxHeight = 12;
    std::string choice = runListPopupModal(popup, {});
    if (choice.empty()) return;

    auto toggle = [&](const char *key, int &field) {
      int v = field == 1 ? 0 : 1;
      guiPrefWrite(key, v == 1 ? "1" : "0");
      field = v;
    };
    if (choice == "tabw") {
      std::string v = promptStatus("Tab width (1–64): ");
      int n = (int)integerValue(v);
      if (n >= 1 && n <= 64) {
        guiPrefWrite("tabWidth", std::to_string(n));
        gGuiPrefs.tabWidth = n;
      }
    } else if (choice == "usetabs") toggle("useTabs", gGuiPrefs.useTabs);
    else if (choice == "wrap") toggle("wordWrap", gGuiPrefs.wordWrap);
    else if (choice == "guides") toggle("showIndentGuides", gGuiPrefs.indentGuides);
    else if (choice == "ws") toggle("showWhitespace", gGuiPrefs.showWhitespace);
    else if (choice == "eol") toggle("showEOL", gGuiPrefs.showEOL);
    else if (choice == "edge") {
      std::string v = promptStatus("Edge column (0 = off): ");
      long n = integerValue(v);
      if (!v.empty() && n >= 0) {
        guiPrefWrite("edgeColumn", std::to_string(n));
        guiPrefWrite("edgeMode", n > 0 ? "1" : "0");
        gGuiPrefs.edgeColumn = n;
        gGuiPrefs.edgeMode = n > 0 ? 1 : 0;
      }
    } else if (choice == "theme") {
      std::vector<npptui::PaletteEntry> entries;
      for (const std::string &n : availableThemeNames()) entries.push_back({n, n, ""});
      std::string pick = runFilteredPicker(entries, " Theme ", 0);
      if (!pick.empty()) {
        guiPrefWrite("NPPActiveTheme", pick);
        applyThemeSelection();
        *transient = "— theme changed (GUI follows on its next launch)";
      }
    }
    applyGuiPrefsToView();
    applyGuiPrefsToDoc();
  }
}

// ── menu bar modal (T3.2) ─────────────────────────────────────────────────────

// Opens menu `startIndex` as a dropdown under its title. Left/Right move
// between menus, Up/Down/Enter select, Esc/F10/click-outside close, a click
// on another title switches, a click on an item runs it. One event at a
// time from the shared queue, like every modal.
static void runMenuBar(size_t startIndex, bool &running, std::string *transient) {
  ModalScope modalScope;
  const std::vector<TuiMenu> &menus = menuDefinition();
  if (menus.empty()) return;
  size_t menuIdx = startIndex < menus.size() ? startIndex : 0;
  npptui::Popup popup;
  WINDOW *popWin = nullptr;
  npptui::Rect area{};

  auto lookupCommand = [](const char *id) -> const TuiCommand * {
    for (const TuiCommand &c : kCommands)
      if (strcmp(c.id, id) == 0) return &c;
    return nullptr;
  };
  auto build = [&]() {
    std::vector<npptui::PopupListItem> items;
    int widest = 18;
    for (const char *id : menus[menuIdx].commandIds) {
      if (strcmp(id, "---") == 0) {
        items.push_back(npptui::PopupListItem::divider());
        continue;
      }
      const TuiCommand *cmd = lookupCommand(id);
      if (!cmd) continue;
      npptui::PopupListItem item(cmd->title);
      item.data = cmd->id;
      item.detail = cmd->shortcut;
      int w = npptui::DisplayWidthUtf8(item.text) + npptui::DisplayWidthUtf8(item.detail) + 8;
      if (w > widest) widest = w;
      items.push_back(std::move(item));
    }
    popup = npptui::Popup::list(std::move(items));
    popup.width = widest;
    popup.maxHeight = (int)popup.items.size() + 2;
    popup.showClose = false;  // fresh-style menu dropdown: no [×]
    popup.selectFirst();      // never land on a leading divider
    int x = menuIdx < gMenuSpans.size() ? gMenuSpans[menuIdx].x : 0;
    popup.position = npptui::PopupPosition::fixed(x, 1);
    gMenuActive = (int)menuIdx;
  };

  std::string chosen;
  bool done = false;
  build();
  renderModalPopup(popup, popWin, area, /*dim=*/false);
  while (!done) {
    Event e;
    if (!nextEvent(e)) break;
    if (e.type == Event::Type::Key) {
      const npptui::KeyEvent &k = e.key.event;
      if (k.kind == npptui::KeyEventKind::Release) continue;
      switch (k.code.kind) {
        case KeyKind::Esc: done = true; break;
        case KeyKind::Left: menuIdx = (menuIdx + menus.size() - 1) % menus.size(); build(); break;
        case KeyKind::Right: menuIdx = (menuIdx + 1) % menus.size(); build(); break;
        case KeyKind::Up: popup.selectPrev(); break;
        case KeyKind::Down: popup.selectNext(); break;
        case KeyKind::Home: popup.selectFirst(); break;
        case KeyKind::End: popup.selectLast(); break;
        case KeyKind::Enter:
          if (const npptui::PopupListItem *item = popup.selectedItem()) chosen = item->data;
          done = true;
          break;
        case KeyKind::F:
          if (k.code.num == 10) done = true;
          break;
        default: break;
      }
    } else if (e.type == Event::Type::Mouse) {
      const npptui::MouseEvent &m = e.mouse;
      if (m.kind == npptui::MouseKind::ScrollUp) popup.scrollBy(-3);
      else if (m.kind == npptui::MouseKind::ScrollDown) popup.scrollBy(3);
      else if (m.kind == npptui::MouseKind::Down) {
        bool onTitle = false;
        for (size_t j = 0; j < gMenuSpans.size(); j++) {
          if (npptui::PointInRect(gMenuSpans[j], m.column, m.row)) {
            onTitle = true;
            if (j == menuIdx) done = true;  // click the open title: close
            else {
              menuIdx = j;
              build();
            }
            break;
          }
        }
        if (!onTitle) {
          if (npptui::PointInRect(area, m.column, m.row)) {
            int row = m.row - area.y - (popup.bordered ? 1 : 0);
            size_t idx = popup.scrollOffset + (size_t)(row < 0 ? 0 : row);
            if (row >= 0 && popup.selectIndex(idx)) {
              chosen = popup.items[idx].data;
              done = true;
            }
          } else {
            done = true;
          }
        }
      }
    }
    if (!done) renderModalPopup(popup, popWin, area, /*dim=*/false);
  }
  if (popWin) delwin(popWin);
  gMenuActive = -1;
  curs_set(1);
  if (!chosen.empty()) dispatchCommand(chosen, running, transient);
}

// ── event application (T2.2b) ─────────────────────────────────────────────────

static int sciMods(uint8_t mods) {
  return ((mods & npptui::kModShift) ? SCMOD_SHIFT : 0) |
         ((mods & npptui::kModCtrl) ? SCMOD_CTRL : 0) |
         ((mods & npptui::kModAlt) ? SCMOD_ALT : 0);
}

static void applyKey(const npptui::KeyEvent &k, bool &running, std::string *transient) {
  // A focused dock owns navigation keys; plain typing never leaks into the
  // editor. Ctrl chords (^S/^Q/…) stay global.
  if (gPanelVisible && gPanelFocused) {
    int rows = 0, cols = 0;
    if (panelWin) getmaxyx(panelWin, rows, cols);
    (void)cols;
    buildPanelRows();  // Linux: rows may not have been painted yet (see above)
    switch (k.code.kind) {
      case KeyKind::Up: gPanel.moveSelection(-1, rows); return;
      case KeyKind::Down: gPanel.moveSelection(1, rows); return;
      case KeyKind::PageUp: gPanel.moveSelection(-(rows > 1 ? rows - 1 : 1), rows); return;
      case KeyKind::PageDown: gPanel.moveSelection(rows > 1 ? rows - 1 : 1, rows); return;
      case KeyKind::Home: gPanel.moveSelection(-1000000, rows); return;
      case KeyKind::End: gPanel.moveSelection(1000000, rows); return;
      case KeyKind::Enter: panelActivate(gPanel.selected); return;
      case KeyKind::Esc: gPanelFocused = false; return;
      case KeyKind::F:
        if (k.code.num == 6) {
          gPanelFocused = false;
          return;
        }
        break;  // other F-keys (F10 menu, F1 help) stay global
      case KeyKind::Char:
        if (k.mods & npptui::kModCtrl) break;
        return;
      default: return;
    }
  }
  // Our editor shortcuts: a plain Ctrl chord, spelled identically by the
  // legacy encoding (byte 0x13 → Char('s')+Ctrl) and the kitty protocol.
  if (k.code.kind == KeyKind::Char && k.mods == npptui::kModCtrl) {
    char32_t c = k.code.ch;
    if (c >= 'A' && c <= 'Z') c += 32;
    switch (c) {
      case 'q': cmdQuit(running, transient); return;
      case 's': cmdSave(transient); return;
      case 'z': SSM(SCI_UNDO, 0, 0); return;
      case 'y': SSM(SCI_REDO, 0, 0); return;
      case 't': cmdToggleWrap(transient); return;
      case 'g': cmdGotoLine(); return;
      case 'p': runCommandPalette(running, transient); return;
      case 'o': cmdOpenFile(); return;
      case 'w': cmdCloseBuffer(transient); return;
      case 'f': runFindPopup(); return;
      case 'r': runReplacePopup(transient); return;
      case 'b': cmdToggleFold(); return;
      case 'l': cmdBuffersList(); return;
      case 'n': cmdNew(); return;
      default: break;
    }
  }

  switch (k.code.kind) {
    case KeyKind::Up: scintilla_send_key(sci, SCK_UP, sciMods(k.mods)); break;
    case KeyKind::Down: scintilla_send_key(sci, SCK_DOWN, sciMods(k.mods)); break;
    case KeyKind::Left: scintilla_send_key(sci, SCK_LEFT, sciMods(k.mods)); break;
    case KeyKind::Right: scintilla_send_key(sci, SCK_RIGHT, sciMods(k.mods)); break;
    case KeyKind::Home: scintilla_send_key(sci, SCK_HOME, sciMods(k.mods)); break;
    case KeyKind::End: scintilla_send_key(sci, SCK_END, sciMods(k.mods)); break;
    case KeyKind::PageUp: scintilla_send_key(sci, SCK_PRIOR, sciMods(k.mods)); break;
    case KeyKind::PageDown: scintilla_send_key(sci, SCK_NEXT, sciMods(k.mods)); break;
    case KeyKind::Insert: scintilla_send_key(sci, SCK_INSERT, sciMods(k.mods)); break;
    case KeyKind::Delete: scintilla_send_key(sci, SCK_DELETE, sciMods(k.mods)); break;
    case KeyKind::Backspace: scintilla_send_key(sci, SCK_BACK, sciMods(k.mods)); break;
    case KeyKind::Enter: scintilla_send_key(sci, SCK_RETURN, sciMods(k.mods)); break;
    case KeyKind::Tab:
      // ^Tab / ^⇧Tab cycle buffers (kitty-protocol terminals; legacy ones
      // reach the same commands through the palette).
      if (k.mods & npptui::kModCtrl) {
        (k.mods & npptui::kModShift) ? cmdPrevBuffer() : cmdNextBuffer();
      } else {
        scintilla_send_key(sci, SCK_TAB, sciMods(k.mods));
      }
      break;
    case KeyKind::BackTab:
      if (k.mods & npptui::kModCtrl) cmdPrevBuffer();
      else scintilla_send_key(sci, SCK_TAB, SCMOD_SHIFT);
      break;
    case KeyKind::Esc: scintilla_send_key(sci, SCK_ESCAPE, sciMods(k.mods)); break;
    case KeyKind::Char: {
      if (k.mods & (npptui::kModCtrl | npptui::kModAlt)) {
        // A chord: give Scintilla's own keymap a shot (Ctrl+A select-all,
        // Ctrl+C/X/V clipboard, …). Keymap entries use uppercase.
        char32_t c = k.code.ch;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (k.mods == npptui::kModCtrl && c == 'V' && bridgeClipboardIn()) break;
        scintilla_send_key(sci, (int)c, sciMods(k.mods));
        if (k.mods == npptui::kModCtrl && (c == 'C' || c == 'X')) bridgeClipboardOut();
      } else {
        // Plain (or shift-folded) character: insert.
        scintilla_send_key(sci, (int)k.code.ch, 0);
      }
      break;
    }
    case KeyKind::F:
      if (k.code.num == 3) {
        (k.mods & npptui::kModShift) ? cmdFindPrev(transient) : cmdFindNext(transient);
      } else if (k.code.num == 2) {
        if (k.mods & npptui::kModCtrl) cmdToggleBookmark();
        else if (k.mods & npptui::kModShift) cmdPrevBookmark(transient);
        else cmdNextBookmark(transient);
      } else if (k.code.num == 10) {
        runMenuBar(0, running, transient);  // F10: the TUI menu convention
      } else if (k.code.num == 1) {
        cmdHelp();
      } else if (k.code.num == 6) {
        if (gPanelVisible) gPanelFocused = true;  // F6: focus the dock
      }
      break;
    default: break;  // other F-keys, media keys, lone modifiers: nothing bound yet
  }
}

static void applyEvent(const Event &e, bool &running, std::string *transient) {
  switch (e.type) {
    case Event::Type::Key:
      // Releases must not re-fire actions; repeats are keystrokes.
      if (e.key.event.kind == npptui::KeyEventKind::Release) return;
      applyKey(e.key.event, running, transient);
      break;
    case Event::Type::Mouse: {
      const npptui::MouseEvent &m = e.mouse;
      // Menu bar (row 0): a click on a title opens that menu.
      if (m.kind == npptui::MouseKind::Down && m.row == 0) {
        for (size_t i = 0; i < gMenuSpans.size(); i++) {
          if (npptui::PointInRect(gMenuSpans[i], m.column, m.row)) {
            runMenuBar(i, running, transient);
            return;
          }
        }
        return;
      }
      // Tab bar (row 1): a click on a tab switches to it.
      if (m.kind == npptui::MouseKind::Down && m.row == 1) {
        for (size_t i = 0; i < gTabSpans.size(); i++) {
          if (npptui::PointInRect(gTabSpans[i], m.column, m.row)) {
            switchToBuffer(i);
            return;
          }
        }
        return;  // clicks on the empty bar do nothing
      }
      // Docked panel: clicks select + activate; the wheel moves the
      // selection. Clicking into the editor area returns focus there.
      if (gPanelVisible && panelWin && m.column < panelWidth() && m.row >= 2 &&
          m.row < LINES - 1) {
        int rows = 0, cols = 0;
        getmaxyx(panelWin, rows, cols);
        (void)cols;
        if (m.kind == npptui::MouseKind::ScrollUp) gPanel.moveSelection(-3, rows);
        else if (m.kind == npptui::MouseKind::ScrollDown) gPanel.moveSelection(3, rows);
        else if (m.kind == npptui::MouseKind::Down) {
          gPanelFocused = true;
          long idx = (long)gPanel.scrollOffset + (m.row - 3);  // row 2 = title
          if (idx >= 0 && idx < (long)gPanel.rows.size()) {
            gPanel.selected = (size_t)idx;
            panelActivate((size_t)idx);
          }
        }
        return;
      }
      if (gPanelFocused && m.kind == npptui::MouseKind::Down) gPanelFocused = false;
      // Our scrollbar column: presses in the track, and every drag/release
      // while a scrollbar drag is live, belong to the scrollbar.
      if (m.kind == npptui::MouseKind::Down || m.kind == npptui::MouseKind::Drag ||
          m.kind == npptui::MouseKind::Up) {
        size_t newOffset = 0;
        if (m.kind == npptui::MouseKind::Down &&
            gSbMouse.press(editorScrollState(), editorScrollTrack(), m.column, m.row,
                           newOffset)) {
          SSM(SCI_SETFIRSTVISIBLELINE, newOffset, 0);
          return;
        }
        if (gSbMouse.dragging) {
          if (m.kind == npptui::MouseKind::Drag) {
            if (gSbMouse.drag(editorScrollState(), editorScrollTrack(), m.row, newOffset))
              SSM(SCI_SETFIRSTVISIBLELINE, newOffset, 0);
          } else if (m.kind == npptui::MouseKind::Up) {
            gSbMouse.release();
          }
          return;
        }
      }
      int mods = sciMods(m.mods);
      int button = m.button == npptui::MouseButton::Middle ? 2
                   : m.button == npptui::MouseButton::Right ? 3
                                                            : 1;
      switch (m.kind) {
        case npptui::MouseKind::Down:
          scintilla_send_mouse(sci, SCM_PRESS, button, mods, m.row, m.column);
          break;
        case npptui::MouseKind::Up:
          scintilla_send_mouse(sci, SCM_RELEASE, button, mods, m.row, m.column);
          break;
        case npptui::MouseKind::Drag:
          scintilla_send_mouse(sci, SCM_DRAG, 0, mods, m.row, m.column);
          break;
        case npptui::MouseKind::ScrollUp: SSM(SCI_LINESCROLL, 0, -3); break;
        case npptui::MouseKind::ScrollDown: SSM(SCI_LINESCROLL, 0, 3); break;
        default: break;  // Moved / horizontal scroll: nothing yet
      }
      break;
    }
    case Event::Type::Paste:
      if (!e.paste.empty()) SSM(SCI_REPLACESEL, 0, e.paste.c_str());
      break;
    default: break;  // focus events: consumers arrive with Phase 2 chrome
  }
}

// ── main ──────────────────────────────────────────────────────────────────────

// Brace-match highlight (port of EditorView updateBraceHighlight): the char
// AFTER the caret is checked first, then the one before (NPP order); a
// match lights both braces (STYLE_BRACELIGHT), a lone brace goes bad-brace.
static void updateBraceHighlight(void) {
  long caret = SSM(SCI_GETCURRENTPOS, 0, 0);
  long docLen = SSM(SCI_GETLENGTH, 0, 0);
  static const char *kBraces = "()[]{}";
  long bracePos = -1;
  if (caret < docLen) {
    int ch = (int)SSM(SCI_GETCHARAT, caret, 0);
    if (ch && strchr(kBraces, ch)) bracePos = caret;
  }
  if (bracePos < 0 && caret > 0) {
    int ch = (int)SSM(SCI_GETCHARAT, caret - 1, 0);
    if (ch && strchr(kBraces, ch)) bracePos = caret - 1;
  }
  long matchPos = bracePos >= 0 ? SSM(SCI_BRACEMATCH, bracePos, 0) : -1;
  if (bracePos == gLastBracePos && matchPos == gLastMatchPos) return;
  gLastBracePos = bracePos;
  gLastMatchPos = matchPos;
  if (bracePos < 0) SSM(SCI_BRACEHIGHLIGHT, (uptr_t)-1, -1);
  else if (matchPos >= 0) SSM(SCI_BRACEHIGHLIGHT, bracePos, matchPos);
  else SSM(SCI_BRACEBADLIGHT, bracePos, 0);
}

static void notification(void *, int, SCNotification *n, void *) {
  if (!n) return;
  if (n->nmhdr.code == (unsigned int)SCN_UPDATEUI) updateBraceHighlight();
  else if (n->nmhdr.code == (unsigned int)SCN_MODIFIED &&
           (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)))
    gTextVersion++;  // Function List rescans on the next dock repaint
}

// Linux: GLib diagnostics (g_warning from udl.c/stylestore.c on a malformed
// XML, say) would print straight into the curses screen; they are dropped
// unless NPP_TUI_DEBUG_LOG is set, in which case they are appended there.
static GLogWriterOutput quietLogWriter(GLogLevelFlags level, const GLogField *fields,
                                       gsize nFields, gpointer) {
  const char *log = getenv("NPP_TUI_DEBUG_LOG");
  if (!log || !log[0]) return G_LOG_WRITER_HANDLED;
  FILE *f = fopen(log, "a");
  if (!f) return G_LOG_WRITER_HANDLED;
  for (gsize i = 0; i < nFields; i++)
    if (strcmp(fields[i].key, "MESSAGE") == 0)
      fprintf(f, "[%d] %s\n", (int)level, (const char *)fields[i].value);
  fclose(f);
  return G_LOG_WRITER_HANDLED;
}

int main(int argc, char **argv) {
  {
    setlocale(LC_ALL, "");
    // Linux: curses needs a UTF-8 locale for wide-character output; a bare
    // SSH login (LANG unset / POSIX) gets C.UTF-8, which glibc always has.
    const char *codeset = nl_langinfo(CODESET);
    if (!codeset || g_ascii_strcasecmp(codeset, "UTF-8") != 0) setlocale(LC_ALL, "C.UTF-8");
    // Linux: the clipboard tools are fed through popen; a tool that exits
    // early must not take the editor down with it.
    signal(SIGPIPE, SIG_IGN);
    // Machine-readable command inventory (the E2E coverage gate diffs its
    // scenario manifest against this — a new command without a test fails).
    if (argc > 1 && strcmp(argv[1], "--list-commands") == 0) {
      for (const TuiCommand &c : kCommands) printf("%s\n", c.id);
      return 0;
    }
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
      /* GAP-189 (macOS 4c893a8): the TUI carries its own pre-1.0 version.
       * NPP_TUI_VERSION is injected by tui/CMakeLists.txt (fallback keeps
       * ad-hoc compiles working); the host version is branding.h's
       * APP_VERSION — the Linux single source of truth (macOS injects it
       * from CMake because the bundle plist needs the same value). */
#ifndef NPP_TUI_VERSION
#define NPP_TUI_VERSION "0.9.0-dev"
#endif
      printf("npp %s (Nextpad++ %s)\n", NPP_TUI_VERSION, APP_VERSION);
      return 0;
    }
    std::vector<std::string> cliPaths;
    for (int i = 1; i < argc; i++) {
      if (!argv[i][0]) continue;
      // Linux: the GUI's -settingsDir= relocates the whole user data tree
      // (config.xml, langs.xml, themes, session) — paths.c owns it.
      if (g_str_has_prefix(argv[i], "-settingsDir=")) {
        npp_paths_set_override(argv[i] + strlen("-settingsDir="));
        continue;
      }
      cliPaths.push_back(standardizedAbsolutePath(argv[i]));
    }
    g_log_set_writer_func(quietLogWriter, nullptr, nullptr);
    langsmgr_init();
    gColorCaps = npptui::DetectColorCapability();
    // Theme (T2.8): $NPP_TUI_THEME override, else the shared config dir's
    // stylers.xml (the GUI's active theme), else built-in dark defaults.
    gTheme = npptui::DefaultTuiTheme();
    gThemePath = resolveThemePath();  // the GUI's ACTIVE theme, not just stylers.xml
    npptui::LoadTuiTheme(gThemePath, gTheme);
    syncStylestoreToTheme();  // Linux: the UDL pipeline's style source

    signal(SIGTERM, onFatalSignal);
    signal(SIGHUP, onFatalSignal);
    signal(SIGINT, SIG_IGN);  // raw() delivers ^C as a byte; don't die on it
    // SIGWINCH must interrupt poll(): sigaction without SA_RESTART (BSD
    // signal() would restart the syscall and the resize would wait for input).
    struct sigaction sa = {};
    sa.sa_handler = onWinch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, nullptr);
    atexit(restoreTerminal);

    sci = scintilla_new(notification, nullptr);
    initscr();
    raw();
    noecho();
    start_color();
    use_default_colors();
    curs_set(1);

    statusWin = newwin(1, COLS, LINES - 1, 0);
    scrollWin = newwin(LINES > 3 ? LINES - 3 : 1, 1, 2, COLS - 1);
    tabWin = newwin(1, COLS, 1, 0);
    menuWin = newwin(1, COLS, 0, 0);
    layoutWindows();

    SSM(SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    SSM(SCI_SETVSCROLLBAR, 0, 0);  // Scinterm's ACS_CKBOARD bar off; ours draws
    SSM(SCI_SETMARGINWIDTHN, 0, 5);  // line numbers
    SSM(SCI_SETMARGINWIDTHN, 1, 1);  // spacer cell before the fold column;
    SSM(SCI_SETMARGINMASKN, 1, 1 << kBookmarkMarker);  // doubles as bookmark margin
    SSM(SCI_MARKERDEFINE, kBookmarkMarker, SC_MARK_BOOKMARK);  // ⚑ in Scinterm
    SSM(SCI_SETWRAPMODE, SC_WRAP_WORD, 0);

    // Fold margin (Scinterm renders the classic box tree as glyphs: ⊞/⊟
    // boxes, │ guides, └ tails). Click toggles; SHOW re-expands around
    // edits inside a hidden region; CHANGE re-folds on header changes.
    SSM(SCI_SETMARGINWIDTHN, 2, 1);
    SSM(SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
    SSM(SCI_SETMARGINSENSITIVEN, 2, 1);
    SSM(SCI_SETAUTOMATICFOLD,
        SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CHANGE, 0);
    SSM(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
    SSM(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
    SSM(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
    SSM(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
    SSM(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
    SSM(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
    SSM(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);

    // Buffers (Notepad++ hot-exit semantics): the saved session ALWAYS
    // restores first (still-existing files only, with caret/scroll and any
    // hot-exit snapshot), then CLI paths open into it — attaching to the
    // session's buffer when the file is already there, so its snapshot is
    // kept, and taking focus. CLI launches used to bypass the session
    // entirely, which orphaned every snapshot (and the next quit's prune
    // then deleted them): `npp file` after a hot exit silently lost the
    // unsaved changes. The view's initial document backs the first buffer
    // (add-ref'd so ownership is uniform with the SCI_CREATEDOCUMENT ones).
    sptr_t initialDoc = SSM(SCI_GETDOCPOINTER, 0, 0);
    SSM(SCI_ADDREFDOCUMENT, 0, initialDoc);
    size_t startAt = 0;
    {
      npptui::TuiSession session;
      if (npptui::LoadTuiSession(sessionFilePath(), session)) {
        for (size_t i = 0; i < session.buffers.size(); i++) {
          const npptui::SessionBuffer &sb = session.buffers[i];
          bool hasBackup = !sb.backup.empty();
          const std::string &p = sb.path;
          // A vanished file is skipped — unless a hot-exit snapshot exists,
          // in which case the unsaved content is still worth restoring.
          if (!hasBackup && (p.empty() || !fileExists(p))) continue;
          if (i == session.current) startAt = gBuffers.size();
          TuiBuffer b;
          b.path = p;
          if (hasBackup) b.backupPath = sb.backup;
          b.caret = sb.caret;
          b.firstVisible = sb.firstVisible;
          b.doc = gBuffers.empty() ? initialDoc : SSM(SCI_CREATEDOCUMENT, 0, 0);
          gBuffers.push_back(b);
        }
      }
      bool cliFocused = false;
      for (size_t i = 0; i < cliPaths.size(); i++) {
        size_t existing = gBuffers.size();
        for (size_t j = 0; j < gBuffers.size(); j++)
          if (!gBuffers[j].path.empty() && gBuffers[j].path == cliPaths[i]) {
            existing = j;
            break;
          }
        if (existing == gBuffers.size()) {
          TuiBuffer b;
          b.path = cliPaths[i];
          b.doc = gBuffers.empty() ? initialDoc : SSM(SCI_CREATEDOCUMENT, 0, 0);
          gBuffers.push_back(b);
        }
        if (!cliFocused) {
          startAt = existing;
          cliFocused = true;
        }
      }
      if (gBuffers.empty()) {
        TuiBuffer b;
        b.doc = initialDoc;
        b.loaded = true;
        gBuffers.push_back(b);
      }
    }
    gCur = startAt < gBuffers.size() ? startAt : 0;
    // GUI interop (Phase 4): adopt the GUI's editor preferences before the
    // first activation applies tab settings, then watch the theme file and
    // the GUI's defaults plist so a change in the GUI mirrors here live.
    // Linux: the "plist" is config.xml.
    gGuiPrefs = npptui::LoadGuiEditorPrefs(guiDomain());
    applyGuiPrefsToView();
    activateCurrentBuffer();
    gGuiConfigPath = guiDomain();
    resetInteropWatch();
    SSM(SCI_SETFOCUS, 1, 0);

    // T2.1/T2.2b modes layer (after initscr — see tui/term/modes.h): kitty
    // keyboard flags, 1002 drag + SGR-1006 mouse, bracketed paste. Input is
    // decoded by the vtparser; ncurses never reads.
    termModes = npptui::TermModesEnable(npptui::TermModesConfig{}, STDOUT_FILENO);

    refreshAll("");

    bool running = true;
    while (running) {
      Event e;
      if (!nextEvent(e)) break;  // terminal gone
      std::string transient;
      applyEvent(e, running, &transient);
      // Drain whatever else the same read produced before repainting once.
      while (running && !gQueue.empty()) {
        e = std::move(gQueue.front());
        gQueue.pop_front();
        applyEvent(e, running, &transient);
      }
      if (running) refreshAll(transient);
    }

    saveSession();
    npptui::TermModesUndo(termModes, STDOUT_FILENO);
    scintilla_delete(sci);
    if (statusWin) delwin(statusWin);
    if (scrollWin) delwin(scrollWin);
    if (tabWin) delwin(tabWin);
    if (menuWin) delwin(menuWin);
    endwin();
  }
  return 0;
}

// Linux port of tui/shim/interop.h — `NSDictionary *defaults` became the
// plain `GuiDefaults` struct declared below (the raw key/value pairs read
// from the GUI's store, validated by ParseGuiEditorPrefs exactly as the
// NSNumber-typed dictionary was); `NSString *suiteId` became
// `const std::string &configPath` — the GUI's config.xml, since Linux has no
// defaults domain (docs/12 §8.3, interop row). `#import <Foundation>` removed.
// Names unchanged so main.mm:1105/3618 (`LoadGuiEditorPrefs(guiDomain())`)
// transliterate 1:1 with guiDomain() returning the path — $NPP_TUI_GUI_CONFIG
// (was $NPP_TUI_GUI_DOMAIN) or npp_user_file(NULL, "config.xml").
// Key mapping for the phase-2 body (prefs.c line refs in docs/12 §8.3):
//   tabWidth/useTabs   ← <GUIConfig name="TabSetting" size= replaceBySpace=>
//   tabOverrides       ← <GUIConfig name="TabCustom"><Language …/></GUIConfig>
//   wordWrap/showIndentGuides/showWhitespace/showEOL/edgeColumn/edgeMode
//                      ← <GUIConfig name="ScintillaPrimaryView" …>
// FileWatch is pure POSIX and ports verbatim (st_mtimespec → st_mtim).
// Phase 2a ADDITION (Linux-only, no macOS counterpart — main.mm's
// Preferences popup wrote the defaults domain through CFPreferences):
// SetGuiConfigAttr(configPath, guiConfigName, attr, value) is the config.xml
// writer for the Preferences popup — read-modify-write of ONE attribute on
// <GUIConfig name="guiConfigName" …/> (set if present, appended if not; the
// group itself is created before </GUIConfigs> when missing, and a missing
// file becomes a minimal <NotepadPlus><GUIConfigs/> document). Every other
// byte of the file is preserved. Atomic via g_file_set_contents; false on
// write failure. The value is XML-escaped; pass the file's own spellings
// (size="8", replaceBySpace="no", Wrap="yes", whiteSpaceShow="show").

// GUI↔TUI interop (Phase 4 "companion mode"): the two frontends share the
// Nextpad++ user data dir and its config.xml, so the terminal editor can be
// DRIVEN by the GUI app — change the theme or an editor preference in
// Nextpad++ and the running TUI (even over SSH) follows within a couple of
// seconds. Fresh has no second frontend to mirror; this is ours alone.
//
// This header is the pure, testable layer: preference parsing from a plain
// key/value snapshot and an mtime watcher. The config.xml read and all
// Scintilla application live in main.cpp.

#ifndef NPP_TUI_SHIM_INTEROP_H
#define NPP_TUI_SHIM_INTEROP_H

#include <map>
#include <string>
#include <vector>

namespace npptui {

// Editor preferences the GUI persists that make sense in a terminal.
// Tri-state ints: -1 = key absent (the TUI keeps its own default).
struct GuiTabOverride {
  int tabWidth = -1;
  int useTabs = -1;
};

struct GuiEditorPrefs {
  int tabWidth = -1;
  int useTabs = -1;
  int wordWrap = -1;
  int indentGuides = -1;
  int showWhitespace = -1;
  int showEOL = -1;
  long edgeColumn = -1;
  int edgeMode = -1;  // GUI/Scintilla agree: 0=off 1=line 2=background
  // Per-language overrides ({langName: {tabSize, useTabs}}, the GUI's
  // TabCustom group), keyed by the langs.xml internal name both frontends
  // share through langsmgr.
  std::map<std::string, GuiTabOverride> tabOverrides;
};

// The raw GUI store, before validation — the stand-in for the macOS
// NSDictionary. Values are the attribute strings config.xml carries
// ("4", "yes"/"no", "1"/"0", …); absent key = absent. ParseGuiEditorPrefs
// owns the yes/no + range validation, so a hand-edited config.xml can never
// crash the editor (wrong-typed → treated as absent, as before).
struct GuiDefaults {
  std::map<std::string, std::string> values;  // "tabWidth" → "4", "useTabs" → "no", …
  // "cpp" → {"tabSize" → "2", "useTabs" → "yes"}
  std::map<std::string, std::map<std::string, std::string>> tabOverrides;
};

// Parse from a defaults snapshot. Wrong-typed values are treated as
// absent — a hand-edited config.xml must never crash the editor.
GuiEditorPrefs ParseGuiEditorPrefs(const GuiDefaults &defaults);

// Read the GUI's config.xml at `configPath` (a fresh parse on every call, so
// a long-running TUI sees values the GUI wrote after we launched).
GuiEditorPrefs LoadGuiEditorPrefs(const std::string &configPath);

// Set one attribute on <GUIConfig name="guiConfigName"> in the GUI's
// config.xml (see the header comment). Additive Linux declaration, phase 2a.
bool SetGuiConfigAttr(const std::string &configPath, const std::string &guiConfigName,
                      const std::string &attr, const std::string &value);

// Polls a fixed set of files by modification time. poll() returns the
// indices whose mtime changed since construction / the previous poll —
// including appearing and vanishing files (mtime 0 when absent).
class FileWatch {
 public:
  explicit FileWatch(std::vector<std::string> paths);
  std::vector<size_t> poll();

 private:
  std::vector<std::string> paths_;
  std::vector<double> mtimes_;
};

}  // namespace npptui

#endif

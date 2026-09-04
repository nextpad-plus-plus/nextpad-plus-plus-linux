// Shim header contract check: every tui/shim/*.h must compile standalone
// (C++17, no Foundation) so phase 2's transliteration of main.mm has a
// fixed target. Also pins the signatures — a change here is a change to
// the contract, and should be deliberate.

#include "shim/funclist.h"
#include "shim/interop.h"
#include "shim/session.h"
#include "shim/theme.h"
#include "shim/workspace.h"

#include <cstdio>
#include <type_traits>

using namespace npptui;

static_assert(std::is_same_v<decltype(&LoadTuiTheme), bool (*)(const std::string &, TuiTheme &)>);
static_assert(std::is_same_v<decltype(&LoadTuiSession), bool (*)(const std::string &, TuiSession &)>);
static_assert(std::is_same_v<decltype(&SaveTuiSession), bool (*)(const std::string &, const TuiSession &)>);
static_assert(std::is_same_v<decltype(&LoadWorkspace), bool (*)(const std::string &, WsNode &)>);
static_assert(std::is_same_v<decltype(&ListFolderChildren), bool (*)(const std::string &, std::vector<WsNode> &)>);
static_assert(std::is_same_v<decltype(&ParseGuiEditorPrefs), GuiEditorPrefs (*)(const GuiDefaults &)>);
static_assert(std::is_same_v<decltype(&LoadGuiEditorPrefs), GuiEditorPrefs (*)(const std::string &)>);
static_assert(std::is_same_v<decltype(&ScanFunctions),
                             bool (*)(const std::string &, const std::string &, std::vector<FuncItem> &)>);

int main() {
  // Data types must be usable without any .cpp body linked.
  TuiTheme theme = TuiTheme{};
  TuiSession session;
  WsNode root;
  GuiDefaults defaults;
  GuiEditorPrefs prefs;
  FuncItem item;
  (void)theme; (void)session; (void)root; (void)defaults; (void)prefs; (void)item;
  printf("shim headers: contract OK\n");
  return 0;
}

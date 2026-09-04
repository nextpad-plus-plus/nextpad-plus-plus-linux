// Scene layer (T2.4): semantic projections of editor state, computed by the
// app and consumed by renderers. Design discipline learned from Fresh's
// view/scene.rs (consulted under the Nextpad++ LICENSE provision; no code
// ported — these are our own types for our own state): every piece of chrome
// is described as plain serializable data first, so renderers stay dumb, a
// future second presenter (web) consumes the same structs, and tests assert
// on projections and rendered cells instead of terminal state.
//
// Pure data: no curses, no ObjC, no Scintilla. describe() emits a stable
// one-line form for snapshot tests.

#ifndef NPP_TUI_SCENE_SCENE_H
#define NPP_TUI_SCENE_SCENE_H

#include <string>
#include <vector>

namespace npptui {

struct SceneStatus {
  std::string fileName;   // display name (UTF-8); "(untitled)" for unnamed
  bool dirty = false;
  std::string language;   // "text" when no lexer is active
  long line = 1;          // 1-based caret position
  long column = 1;
  std::string transient;  // short-lived note ("— saved"); empty = none
  std::string encoding;   // on-disk encoding when NOT plain UTF-8 ("iso-8859-1",
                          // "utf-8-bom", …); empty = UTF-8, not shown

  std::string describe() const;
};

struct ScenePrompt {
  std::string label;  // e.g. "Go to line: " (no leading space; renderer pads)
  std::string input;  // typed text so far; empty for single-key prompts

  std::string describe() const;
};

struct SceneTabs {
  struct Tab {
    std::string title;  // display name (UTF-8)
    bool dirty = false;
    bool active = false;
  };
  std::vector<Tab> tabs;

  std::string describe() const;
};

}  // namespace npptui

#endif

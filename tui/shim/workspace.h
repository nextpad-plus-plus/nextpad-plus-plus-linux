// Linux port of tui/shim/workspace.h — `NSString *path` / `NSString *dir`
// became `const std::string &`; `#import <Foundation/Foundation.h>` removed.
// Function and struct names are unchanged so main.mm's call sites
// transliterate 1:1 (main.mm:2090 passes a std::string already; :2122 drops
// its stringWithUTF8String: wrapper). Body (phase 2): GMarkup for the
// .workspace XML, GDir + g_utf8_collate_key_for_filename ordering for the
// folder listing — docs/12 §8.3, workspace row. convertPath (workspace.mm:8-17)
// is string-only and ports verbatim.

// Workspace loader (Phase 4.5): parses the GUI's .workspace XML
// (<NotepadPlus><Project name=…> with nested <Folder name=…>/<File
// name=…/>) into a plain tree for the Project dock. Path conversion
// mirrors the GUI's project panel: backslashes normalized, absolute
// paths kept, Windows/UNC paths kept for display, relative paths resolved
// against the workspace file's directory. GLib-only.

#ifndef NPP_TUI_SHIM_WORKSPACE_H
#define NPP_TUI_SHIM_WORKSPACE_H

#include <string>
#include <vector>

namespace npptui {

struct WsNode {
  std::string name;
  std::string path;     // files: absolute (or unresolvable-verbatim) path
  bool isFile = false;  // false = project/folder
  std::vector<WsNode> children;
};

// Parses `path`; on success fills `outRoot` (children = the projects) and
// returns true. False: missing/malformed file.
bool LoadWorkspace(const std::string &path, WsNode &outRoot);

// Folder-as-Workspace listing (folder-tree panel parity): the immediate
// children of `dir` — hidden files skipped, directories first, each group
// case-insensitively alphabetical. Directories carry their full path too
// (lazy expansion lists them on demand). False: unreadable directory.
bool ListFolderChildren(const std::string &dir, std::vector<WsNode> &out);

}  // namespace npptui

#endif

// Linux port of tui/shim/session.h — no signature change: the macOS header
// was already Foundation-free. The file is tui-session.xml (GMarkup) instead
// of tui-session.plist, with the SAME keys — version / current /
// buffers[path, backup, caret, firstVisible] — so session_test ports 1:1
// (docs/12 §8.3, session row). Default path: npp_user_file(NULL,
// "tui-session.xml") from src/paths.c; $NPP_TUI_SESSION still overrides.

// Session shim (T2.9): the TUI's own session file (tui-session.xml).
// Deliberately a SEPARATE file from the GUI's session.xml — two writers on
// one session store is how sessions get lost (host issues #215/#285); the
// TUI never reads or writes the GUI's. Saved on clean quit, restored when
// launched with no file arguments; $NPP_TUI_SESSION overrides the path.

#ifndef NPP_TUI_SHIM_SESSION_H
#define NPP_TUI_SHIM_SESSION_H

#include <cstddef>
#include <string>
#include <vector>

namespace npptui {

struct SessionBuffer {
  std::string path;    // absolute; empty for an unnamed buffer with a backup
  long caret = 0;
  long firstVisible = 0;
  // Hot exit: absolute path of the snapshot holding this buffer's UNSAVED
  // content. Non-empty = restore the snapshot as modified text (the real
  // file stays untouched until the user saves). Empty = buffer was clean.
  std::string backup;
};

struct TuiSession {
  std::vector<SessionBuffer> buffers;
  size_t current = 0;  // index into buffers
};

// Load `path` (XML). false — leaving `out` untouched — when the file is
// missing or malformed. Existence of the listed files is the caller's
// business (checked at restore time, not here).
bool LoadTuiSession(const std::string &path, TuiSession &out);

// Atomically write the session as XML. false on write failure.
bool SaveTuiSession(const std::string &path, const TuiSession &session);

}  // namespace npptui

#endif

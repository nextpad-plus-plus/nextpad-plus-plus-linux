// E2E pty driver (Phase 4.0): spawns the real `npp` binary on a pty,
// pumps its output through the ScreenGrid model, and gives tests
// condition-driven waits — "send keys, await screen state" — instead of
// sleep pacing. File/system assertions run in-process alongside.

#ifndef NPP_TUI_TESTS_E2E_DRIVER_H
#define NPP_TUI_TESTS_E2E_DRIVER_H

#include <sys/types.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../../term/screen.h"

namespace npptui {

class E2ESession {
 public:
  // argv[0] is the npp binary path; env entries are ADDED to a minimal
  // base (TERM=xterm-256color, HOME/PATH inherited).
  E2ESession(const std::vector<std::string> &argv,
             const std::map<std::string, std::string> &env, int cols = 100,
             int rows = 30);
  ~E2ESession();

  void send(const std::string &bytes);
  // Pump output until pred(grid) holds; false on timeout.
  bool await(const std::function<bool(const ScreenGrid &)> &pred, int timeoutMs = 8000);
  bool awaitText(const std::string &needle, int timeoutMs = 8000);
  // "Gone" assertions must SURVIVE A QUIET WINDOW: a modal frame repaints
  // the editor first and the popup after, so a raw predicate can sample the
  // mid-frame erase and pass while the popup is still open. This waits for
  // pred to hold with no further output for quietMs.
  bool awaitStable(const std::function<bool(const ScreenGrid &)> &pred,
                   int timeoutMs = 8000, int quietMs = 150);
  bool awaitGone(const std::string &needle, int timeoutMs = 8000);
  void pump(int ms);  // read output for a fixed duration (settling)

  void resize(int cols, int rows);  // TIOCSWINSZ → child gets SIGWINCH
  // Waits for exit (sends SIGTERM first if kill). Returns exit status,
  // -1 on timeout.
  int waitExit(int timeoutMs = 8000, bool kill = false);

  ScreenGrid grid;
  bool alive() const { return pid_ > 0; }

 private:
  size_t drain(void);  // nonblocking read; returns bytes consumed
  int master_ = -1;
  pid_t pid_ = -1;
};

// Shared check plumbing for the e2e binaries.
extern int e2eChecks, e2eFailures;
void e2eCheck(bool ok, const char *name);
// mkdtemp under TMPDIR; returns absolute path.
std::string e2eTempDir(const char *tag);
void e2eWriteFile(const std::string &path, const std::string &content);
std::string e2eReadFile(const std::string &path);
int e2eReport(const char *suite);  // prints summary, returns exit code

// Runs a palette command: ^P, wait for the input row, type `query`, Enter.
// The query must filter the wanted command to the top (exact titles do).
bool e2ePalette(E2ESession &s, const std::string &query);
// Prints the grid to stderr (debugging failed scenarios).
void e2eDump(const ScreenGrid &g);

}  // namespace npptui

#endif

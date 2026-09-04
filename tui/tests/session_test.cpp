// Tests for tui/shim/session (T2.9): XML round-trip, unnamed-buffer
// filtering, current clamping, and failure modes. Exit 0 = all pass.
//
// Linux port of tui/tests/session_test.mm (plist → tui-session.xml;
// NSTemporaryDirectory → g_dir_make_tmp). Assertions unchanged.

#include "../shim/session.h"

#include <glib.h>

#include <cstdio>
#include <string>

using namespace npptui;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *name) {
  checks++;
  if (!ok) {
    failures++;
    fprintf(stderr, "FAIL %s\n", name);
  }
}

static std::string g_tmp;

static std::string tmpPath(const char *name) {
  gchar *p = g_build_filename(g_tmp.c_str(), name, nullptr);
  std::string out = p;
  g_free(p);
  return out;
}

int main() {
  gchar *tmp = g_dir_make_tmp("tui-session-test-XXXXXX", nullptr);
  if (!tmp) {
    fprintf(stderr, "cannot create temp dir\n");
    return 1;
  }
  g_tmp = tmp;
  g_free(tmp);

  std::string path = tmpPath("tui_session_test.xml");

  // Round trip.
  TuiSession s;
  s.buffers.push_back({"/tmp/a.cpp", 120, 4, ""});
  s.buffers.push_back({"/tmp/b.md", 7, 0, ""});
  s.current = 1;
  check(SaveTuiSession(path, s), "save succeeds");

  TuiSession r;
  check(LoadTuiSession(path, r), "load succeeds");
  check(r.buffers.size() == 2, "buffer count round-trips");
  check(r.buffers[0].path == "/tmp/a.cpp" && r.buffers[0].caret == 120 &&
            r.buffers[0].firstVisible == 4,
        "first buffer fields round-trip");
  check(r.buffers[1].path == "/tmp/b.md" && r.buffers[1].caret == 7,
        "second buffer fields round-trip");
  check(r.current == 1, "current round-trips");

  // Hot-exit backups round-trip; an unnamed buffer WITH a snapshot is
  // restorable, an unnamed buffer without one is not.
  TuiSession withUnnamed;
  withUnnamed.buffers.push_back({"", 5, 0, ""});                    // dropped
  withUnnamed.buffers.push_back({"", 3, 1, "/tmp/bk/snap-u.txt"});  // kept
  withUnnamed.buffers.push_back({"/tmp/c.txt", 9, 2, "/tmp/bk/snap-c.txt"});
  check(SaveTuiSession(path, withUnnamed), "save with backups succeeds");
  TuiSession r2;
  check(LoadTuiSession(path, r2) && r2.buffers.size() == 2,
        "backup-less unnamed dropped, snapshot ones kept");
  check(r2.buffers.size() == 2 && r2.buffers[0].path.empty() &&
            r2.buffers[0].backup == "/tmp/bk/snap-u.txt" && r2.buffers[0].caret == 3,
        "unnamed+backup round-trips");
  check(r2.buffers.size() == 2 && r2.buffers[1].path == "/tmp/c.txt" &&
            r2.buffers[1].backup == "/tmp/bk/snap-c.txt",
        "named+backup round-trips");

  // Out-of-range current clamps to 0 on load.
  TuiSession badCurrent;
  badCurrent.buffers.push_back({"/tmp/d.txt", 0, 0, ""});
  badCurrent.current = 7;
  check(SaveTuiSession(path, badCurrent), "save odd current");
  TuiSession r3;
  check(LoadTuiSession(path, r3) && r3.current == 0, "out-of-range current clamps");

  // Failure modes leave the output untouched.
  TuiSession untouched;
  untouched.current = 42;
  check(!LoadTuiSession("/nonexistent/tui-session.xml", untouched), "missing file fails");
  std::string bad = tmpPath("tui_bad.xml");
  g_file_set_contents(bad.c_str(), "not a session", -1, nullptr);
  check(!LoadTuiSession(bad, untouched), "malformed XML fails");
  std::string wrongRoot = tmpPath("tui_wrong_root.xml");
  g_file_set_contents(wrongRoot.c_str(), "<NotepadPlus><Buffer path=\"/x\"/></NotepadPlus>", -1,
                      nullptr);
  check(!LoadTuiSession(wrongRoot, untouched), "foreign root element fails");
  check(untouched.current == 42, "failed loads leave output untouched");

  // Empty session round-trips as empty.
  check(SaveTuiSession(path, TuiSession{}), "empty session saves");
  TuiSession r4;
  check(LoadTuiSession(path, r4) && r4.buffers.empty() && r4.current == 0,
        "empty session loads empty");

  // Linux addition: paths carrying XML-special characters survive the
  // escape/unescape round trip.
  TuiSession odd;
  odd.buffers.push_back({"/tmp/a&b <c>'\".txt", 1, 0, "/tmp/bk/x&y.txt"});
  check(SaveTuiSession(path, odd), "odd path saves");
  TuiSession r5;
  check(LoadTuiSession(path, r5) && r5.buffers.size() == 1 &&
            r5.buffers[0].path == "/tmp/a&b <c>'\".txt" && r5.buffers[0].backup == "/tmp/bk/x&y.txt",
        "XML-special characters round-trip");

  fprintf(stderr, "%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}

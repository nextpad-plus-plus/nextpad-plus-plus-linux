// E2E pty driver (Phase 4.0) — see driver.h.

#include "driver.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pty.h>  // forkpty (glibc; macOS: <util.h>)

namespace npptui {

int e2eChecks = 0;
int e2eFailures = 0;

void e2eCheck(bool ok, const char *name) {
  e2eChecks++;
  if (!ok) {
    e2eFailures++;
    fprintf(stderr, "FAIL %s\n", name);
  }
}

std::string e2eTempDir(const char *tag) {
  const char *base = getenv("TMPDIR");
  std::string tmpl = std::string(base && base[0] ? base : "/tmp");
  if (tmpl.back() != '/') tmpl += '/';
  tmpl += std::string("npp-e2e-") + tag + "-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char *dir = mkdtemp(buf.data());
  return dir ? dir : "/tmp";
}

void e2eWriteFile(const std::string &path, const std::string &content) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return;
  fwrite(content.data(), 1, content.size(), f);
  fclose(f);
}

std::string e2eReadFile(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

int e2eReport(const char *suite) {
  printf("%s: %d checks, %d failure(s)\n", suite, e2eChecks, e2eFailures);
  return e2eFailures ? 1 : 0;
}

bool e2ePalette(E2ESession &s, const std::string &query) {
  s.send("\x10");  // ^P
  if (!s.awaitText("> ")) return false;
  s.send(query);
  // Wait until the typed query is visible in the input row, so Enter can't
  // race ahead of the filter.
  if (!s.awaitText("> " + query)) return false;
  s.send("\r");
  return true;
}

void e2eDump(const ScreenGrid &g) {
  fprintf(stderr, "── screen %dx%d cursor(%d,%d) ──\n", g.cols(), g.rows(),
          g.cursorX(), g.cursorY());
  for (int y = 0; y < g.rows(); y++) {
    std::string r = g.rowText(y);
    while (!r.empty() && r.back() == ' ') r.pop_back();
    fprintf(stderr, "%2d|%s\n", y, r.c_str());
  }
}

static double nowMs(void) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

E2ESession::E2ESession(const std::vector<std::string> &argv,
                       const std::map<std::string, std::string> &env, int cols,
                       int rows)
    : grid(cols, rows) {
  struct winsize ws = {};
  ws.ws_col = (unsigned short)cols;
  ws.ws_row = (unsigned short)rows;

  pid_t pid = forkpty(&master_, nullptr, nullptr, &ws);
  if (pid < 0) {
    fprintf(stderr, "forkpty failed: %s\n", strerror(errno));
    return;
  }
  if (pid == 0) {
    // Child: minimal, deterministic environment.
    setenv("TERM", "xterm-256color", 1);
    setenv("NPP_TUI_NO_CLIPBOARD", "1", 1);  // never touch the user's clipboard
    unsetenv("NPP_TUI_THEME");
    unsetenv("NPP_TUI_GUI_DOMAIN");
    for (const auto &kv : env) {
      if (kv.second.empty()) unsetenv(kv.first.c_str());  // "" = remove
      else setenv(kv.first.c_str(), kv.second.c_str(), 1);
    }
    std::vector<char *> av;
    for (const std::string &a : argv) av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);
    execv(av[0], av.data());
    _exit(127);
  }
  pid_ = pid;
  fcntl(master_, F_SETFL, O_NONBLOCK);
}

E2ESession::~E2ESession() {
  if (pid_ > 0) waitExit(2000, /*kill=*/true);
  if (master_ >= 0) close(master_);
}

void E2ESession::send(const std::string &bytes) {
  if (master_ < 0) return;
  size_t off = 0;
  while (off < bytes.size()) {
    ssize_t n = write(master_, bytes.data() + off, bytes.size() - off);
    if (n > 0) off += (size_t)n;
    else if (errno != EAGAIN) break;
    else usleep(2000);
  }
}

size_t E2ESession::drain(void) {
  uint8_t buf[16384];
  size_t total = 0;
  while (true) {
    ssize_t n = read(master_, buf, sizeof buf);
    if (n <= 0) break;
    grid.feed(buf, (size_t)n);
    total += (size_t)n;
  }
  return total;
}

bool E2ESession::await(const std::function<bool(const ScreenGrid &)> &pred,
                       int timeoutMs) {
  double deadline = nowMs() + timeoutMs;
  while (true) {
    drain();
    if (pred(grid)) return true;
    if (nowMs() >= deadline) return false;
    struct pollfd pfd = {master_, POLLIN, 0};
    poll(&pfd, 1, 25);
  }
}

bool E2ESession::awaitText(const std::string &needle, int timeoutMs) {
  return await([&](const ScreenGrid &g) { return g.contains(needle); }, timeoutMs);
}

bool E2ESession::awaitStable(const std::function<bool(const ScreenGrid &)> &pred,
                             int timeoutMs, int quietMs) {
  double deadline = nowMs() + timeoutMs;
  while (true) {
    drain();
    if (pred(grid)) {
      // Hold through a quiet window: any new output restarts the wait.
      double quietEnd = nowMs() + quietMs;
      bool disturbed = false;
      while (nowMs() < quietEnd) {
        struct pollfd pfd = {master_, POLLIN, 0};
        poll(&pfd, 1, 10);
        if (drain() > 0) {
          disturbed = true;
          break;
        }
      }
      if (!disturbed && pred(grid)) return true;
    }
    if (nowMs() >= deadline) return false;
    struct pollfd pfd = {master_, POLLIN, 0};
    poll(&pfd, 1, 25);
  }
}

bool E2ESession::awaitGone(const std::string &needle, int timeoutMs) {
  return awaitStable([&](const ScreenGrid &g) { return !g.contains(needle); },
                     timeoutMs);
}

void E2ESession::pump(int ms) {
  double deadline = nowMs() + ms;
  while (nowMs() < deadline) {
    drain();
    struct pollfd pfd = {master_, POLLIN, 0};
    poll(&pfd, 1, 20);
  }
}

void E2ESession::resize(int cols, int rows) {
  grid.resize(cols, rows);
  struct winsize ws = {};
  ws.ws_col = (unsigned short)cols;
  ws.ws_row = (unsigned short)rows;
  ioctl(master_, TIOCSWINSZ, &ws);
}

int E2ESession::waitExit(int timeoutMs, bool kill) {
  if (pid_ <= 0) return -1;
  if (kill) ::kill(pid_, SIGTERM);
  double deadline = nowMs() + timeoutMs;
  int status = 0;
  while (true) {
    drain();  // keep the pty from filling while the app shuts down
    pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == pid_) {
      pid_ = -1;
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    if (nowMs() >= deadline) {
      if (!kill) return waitExit(2000, /*kill=*/true);
      ::kill(pid_, SIGKILL);
      waitpid(pid_, &status, 0);
      pid_ = -1;
      return -1;
    }
    usleep(10000);
  }
}

}  // namespace npptui

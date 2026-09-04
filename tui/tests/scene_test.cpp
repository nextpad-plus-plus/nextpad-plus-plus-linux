// Tests for the Scene layer + status-row chrome renderer (T2.4).
// Snapshot-style: assert on describe() projections and on the exact
// (x, text) paint calls a recording surface receives. Exit 0 = all pass.

#include "../chrome/statusbar.h"
#include "../scene/scene.h"

#include <clocale>
#include <cstdio>
#include <string>
#include <vector>

using namespace npptui;

static int failures = 0;
static int checks = 0;

static void expectEq(const std::string &got, const std::string &want, const char *name) {
  checks++;
  if (got != want) {
    failures++;
    fprintf(stderr, "FAIL %s\n  want: %s\n  got:  %s\n", name, want.c_str(), got.c_str());
  }
}

static void check(bool ok, const char *name) {
  checks++;
  if (!ok) {
    failures++;
    fprintf(stderr, "FAIL %s\n", name);
  }
}

// Records paint calls; snapshot form "x:'text'|x:'text'".
struct RecordingSurface : RowSurface {
  int w;
  std::vector<std::pair<int, std::string>> calls;
  explicit RecordingSurface(int width) : w(width) {}
  int width() const override { return w; }
  void putText(int x, const std::string &utf8) override { calls.emplace_back(x, utf8); }
  std::string snapshot() const {
    std::string s;
    for (const auto &c : calls) {
      if (!s.empty()) s += "|";
      s += std::to_string(c.first) + ":'" + c.second + "'";
    }
    return s;
  }
};

static SceneStatus sampleStatus() {
  SceneStatus s;
  s.fileName = "hello.cpp";
  s.dirty = true;
  s.language = "cpp";
  s.line = 3;
  s.column = 7;
  s.transient = "— saved";
  return s;
}

int main() {
  setlocale(LC_ALL, "");

  // ── projections ────────────────────────────────────────────────────────────
  expectEq(sampleStatus().describe(),
           "status{file='hello.cpp', dirty=1, lang=cpp, ln=3, col=7, transient='— saved'}",
           "SceneStatus describe (full)");
  {
    SceneStatus s;
    s.fileName = "(untitled)";
    s.language = "text";
    expectEq(s.describe(), "status{file='(untitled)', dirty=0, lang=text, ln=1, col=1}",
             "SceneStatus describe (defaults, no transient)");
  }
  {
    ScenePrompt p;
    p.label = "Go to line: ";
    p.input = "150";
    expectEq(p.describe(), "prompt{label='Go to line: ', input='150'}", "ScenePrompt describe");
  }
  {
    SceneTabs t;
    t.tabs.push_back({"a.cpp", true, true});
    t.tabs.push_back({"b.md", false, false});
    expectEq(t.describe(), "tabs{'a.cpp'*!, 'b.md'}", "SceneTabs describe");
  }

  // ── text halves ────────────────────────────────────────────────────────────
  expectEq(StatusLeftText(sampleStatus()), " hello.cpp •  — saved", "left text with dirty+transient");
  {
    SceneStatus s;
    s.fileName = "a.txt";
    s.language = "text";
    expectEq(StatusLeftText(s), " a.txt", "left text plain");
    expectEq(StatusRightText(s), "text  Ln 1, Col 1 ", "right text");
  }

  // ── encoding segment (T3.4) ────────────────────────────────────────────────
  {
    SceneStatus s;
    s.fileName = "legacy.txt";
    s.language = "text";
    s.encoding = "iso-8859-1";
    expectEq(StatusRightText(s), "iso-8859-1  text  Ln 1, Col 1 ",
             "right text shows non-UTF-8 encoding");
    expectEq(s.describe(),
             "status{file='legacy.txt', dirty=0, lang=text, ln=1, col=1, enc=iso-8859-1}",
             "describe includes encoding");
    s.encoding.clear();
    expectEq(StatusRightText(s), "text  Ln 1, Col 1 ",
             "plain UTF-8 shows no encoding segment");
  }

  // ── display width ──────────────────────────────────────────────────────────
  check(DisplayWidthUtf8("abc") == 3, "ascii width");
  check(DisplayWidthUtf8("中文") == 4, "CJK width (2 cells each)");
  check(DisplayWidthUtf8("中a文") == 5, "mixed width");
  check(DisplayWidthUtf8("— saved") == 7, "em dash is narrow");
  check(DisplayWidthUtf8("") == 0, "empty width");

  // ── rendering ──────────────────────────────────────────────────────────────
  {
    SceneStatus s;
    s.fileName = "a.txt";
    s.language = "cpp";
    s.line = 12;
    s.column = 4;
    RecordingSurface out(40);
    RenderStatusRow(s, out);
    // right = "cpp  Ln 12, Col 4 " -> width 18 -> x = 22.
    expectEq(out.snapshot(), "0:' a.txt'|22:'cpp  Ln 12, Col 4 '", "status row layout");
  }
  {
    // CJK filename must not disturb the right edge (it is positioned by the
    // right text's own width); and the right text pins by DISPLAY width, so a
    // CJK language name still lands flush at the edge.
    SceneStatus s;
    s.fileName = "中文.txt";
    s.language = "中文";
    s.line = 1;
    s.column = 1;
    RecordingSurface out(30);
    RenderStatusRow(s, out);
    // right = "中文  Ln 1, Col 1 " -> display width 4+2+10 = wait: "中文" 4 +
    // "  Ln 1, Col 1 " 14 = 18 -> x = 12.
    expectEq(out.snapshot(), "0:' 中文.txt'|12:'中文  Ln 1, Col 1 '",
             "CJK-aware right alignment");
  }
  {
    // A row too narrow for the right half drops it instead of wrapping.
    SceneStatus s;
    s.fileName = "long-name.cpp";
    s.language = "cpp";
    RecordingSurface out(10);
    RenderStatusRow(s, out);
    expectEq(out.snapshot(), "0:' long-name.cpp'", "narrow row drops the right half");
  }
  {
    ScenePrompt p;
    p.label = "Save as: ";
    p.input = "notes.md";
    RecordingSurface out(40);
    RenderPromptRow(p, out);
    expectEq(out.snapshot(), "0:' Save as: notes.md'", "prompt row layout");
  }

  fprintf(stderr, "%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}

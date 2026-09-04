// Tests for tui/shim/funclist (Phase 4.5): the XML engine (via flxml_scan)
// against fixture rules — classRange nesting, top-level functions,
// className grouping, \K, comment exclusion, line numbers — and the
// hardcoded-regex fallback for languages without rules.
//
// Linux port of tui/tests/funclist_test.mm. flxml resolves rules through
// src/paths.c (<user dir>/functionList/<lang>.xml, then RESOURCES_DIR), so
// the $NPP_TUI_FUNCLIST_DIR hook became npp_paths_set_override(tmpdir) —
// the `-settingsDir=` mechanism — with fixtures under tmpdir/functionList/.
// The shipped-rules smoke is deterministic here: RESOURCES_DIR points at
// the in-tree resources/ for a dev build. Assertions unchanged.

#include "../shim/funclist.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <string>

extern "C" {
#include "paths.h"
}

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

static std::string g_rules;

static std::string writeRules(const char *lang, const char *xml) {
  gchar *leaf = g_strdup_printf("%s.xml", lang);
  gchar *p = g_build_filename(g_rules.c_str(), leaf, nullptr);
  g_file_set_contents(p, xml, -1, nullptr);
  std::string out = p;
  g_free(p);
  g_free(leaf);
  return out;
}

int main() {
  gchar *tmp = g_dir_make_tmp("tui-funclist-test-XXXXXX", nullptr);
  if (!tmp) {
    fprintf(stderr, "cannot create temp dir\n");
    return 1;
  }
  std::string dir = tmp;
  g_free(tmp);
  gchar *rules = g_build_filename(dir.c_str(), "functionList", nullptr);
  g_mkdir_with_parents(rules, 0700);
  g_rules = rules;
  g_free(rules);
  npp_paths_set_override(dir.c_str());

  {  // No XML rules → the GUI's generic hardcoded fallback still runs.
    std::vector<FuncItem> items;
    check(ScanFunctions("int f() {\n}\n", "nosuchlang", items), "missing rules → hardcoded fallback");
    check(items.size() == 1 && items[0].name == "f", "generic C-style pattern finds the function");
  }
  {  // Swift (user report): no swift.xml ships — hardcoded parity path,
     // methods nested into their type via brace ranges.
    std::vector<FuncItem> items;
    std::string swift =
        "import Foundation\n"
        "class Camera {\n"
        "    override func viewDidLoad() {\n"
        "    }\n"
        "    private func setupUI() {}\n"
        "}\n"
        "func freestanding() {}\n";
    check(ScanFunctions(swift, "swift", items), "swift scans via fallback");
    bool shape = items.size() == 4;
    for (size_t i = 0; shape && i < items.size(); i++) {
      if (items[i].name == "Camera") shape = items[i].isNode;
      else if (items[i].name == "viewDidLoad" || items[i].name == "setupUI")
        shape = items[i].depth == 1;
      else if (items[i].name == "freestanding") shape = items[i].depth == 0;
      else shape = false;
    }
    check(shape, "swift: class node + nested methods + top-level func");
    long vdl = 0;
    for (const FuncItem &it : items)
      if (it.name == "viewDidLoad") vdl = it.line;
    check(vdl == 3, "swift: method line number");
  }
  {  // Top-level functions with \K + nameExpr, comments excluded.
    writeRules("toy",
               "<NotepadPlus><functionList>"
               "<parser id=\"toy\" displayName=\"Toy\" "
               "commentExpr=\"(?s)/\\*.*?\\*/\">"
               "<function mainExpr=\"^func\\s+\\K\\w+\">"
               "<functionName><nameExpr expr=\"\\s\\K\\w+\"/></functionName>"
               "</function></parser></functionList></NotepadPlus>");
    std::string text =
        "func alpha() {}\n"
        "/* func hidden() {} */\n"
        "func beta() {}\n";
    std::vector<FuncItem> items;
    check(ScanFunctions(text, "toy", items), "toy rules parse");
    check(items.size() == 2, "two functions (comment excluded)");
    check(items.size() == 2 && items[0].name == "alpha" && items[0].line == 1,
          "first name via \\K mainExpr + nameExpr, line 1");
    check(items.size() == 2 && items[1].name == "beta" && items[1].line == 3,
          "second function on line 3");
  }
  {  // classRange with nested functions.
    writeRules("toyclass",
               "<NotepadPlus><functionList><parser id=\"tc\" displayName=\"TC\">"
               "<classRange mainExpr=\"(?s)class\\s+\\w+\\s*\\{.*?^\\}\">"
               "<className><nameExpr expr=\"class\\s+\\K\\w+\"/>"
               "<nameExpr expr=\"\\w+$\"/></className>"
               "<function mainExpr=\"^\\s+def\\s+\\w+\">"
               "<functionName><nameExpr expr=\"def\\s+\\K\\w+\"/>"
               "<nameExpr expr=\"\\w+$\"/></functionName>"
               "</function></classRange>"
               "</parser></functionList></NotepadPlus>");
    std::string text =
        "class Widget {\n"
        "  def draw\n"
        "  def hide\n"
        "}\n"
        "class Gadget {\n"
        "  def spin\n"
        "}\n";
    std::vector<FuncItem> items;
    check(ScanFunctions(text, "toyclass", items), "class rules parse");
    check(items.size() == 5, "2 classes + 3 methods");
    bool shape = items.size() == 5 && items[0].isNode && items[0].name == "Widget" &&
                 items[0].depth == 0 && items[1].name == "draw" && items[1].depth == 1 &&
                 items[2].name == "hide" && items[3].isNode && items[3].name == "Gadget" &&
                 items[4].name == "spin" && items[4].depth == 1;
    check(shape, "tree flattened as class → methods");
    check(items.size() == 5 && items[3].line == 5 && items[4].line == 6, "nested lines are absolute");
  }
  {  // Top-level function grouped by className expr.
    writeRules("toygroup",
               "<NotepadPlus><functionList><parser id=\"tg\" displayName=\"TG\">"
               "<function mainExpr=\"^\\w+::\\w+\">"
               "<functionName><nameExpr expr=\"::\\K\\w+\"/>"
               "<nameExpr expr=\"\\w+$\"/></functionName>"
               "<className><nameExpr expr=\"^\\w+\"/></className>"
               "</function></parser></functionList></NotepadPlus>");
    std::string text = "Foo::bar\nFoo::baz\nQux::quux\n";
    std::vector<FuncItem> items;
    check(ScanFunctions(text, "toygroup", items), "group rules parse");
    check(items.size() == 5, "2 groups + 3 members");
    check(items.size() == 5 && items[0].isNode && items[0].name == "Foo" && items[1].name == "bar" &&
              items[2].name == "baz" && items[3].name == "Qux" && items[4].name == "quux",
          "members grouped under their class rows");
  }
  {  // UDL-style mixed-case rules file: flxml tries the exact name first,
     // so a user's MyLang.xml is found for lang "MyLang" (Linux FS is
     // case-sensitive; macOS lowercased and relied on its FS).
    writeRules("MyLang",
               "<NotepadPlus><functionList><parser id=\"ml\" displayName=\"ML\">"
               "<function mainExpr=\"^proc\\s+\\K\\w+\"/>"
               "</parser></functionList></NotepadPlus>");
    std::vector<FuncItem> items;
    check(ScanFunctions("proc one\nproc two\n", "MyLang", items) && items.size() == 2 &&
              items[1].name == "two" && items[1].line == 2,
          "mixed-case rules file resolves by exact name");
    std::vector<FuncItem> udl;
    check(ScanFunctions("proc one\n", "udl:MyLang", udl) && udl.size() == 1,
          "udl: prefix stripped for the rules lookup");
  }
  {  // Invalid UTF-8 cannot be scanned (an NSString could not be built).
    std::vector<FuncItem> items;
    check(!ScanFunctions(std::string("int f() {\n\xff\xfe}\n"), "c", items) && items.empty(),
          "invalid UTF-8 → false");
    check(!ScanFunctions("int f() {}", "", items), "empty language → false");
  }
  {  // Real shipped rules smoke: the engine must parse cpp.xml (in-tree
     // resources/functionList for a dev build — RESOURCES_DIR).
    std::vector<FuncItem> items;
    bool ok = ScanFunctions("void helper(int x) {\n  return;\n}\n", "cpp", items);
    check(ok, "shipped cpp.xml loads and scans");
    bool found = false;
    for (const FuncItem &it : items)
      if (it.name.find("helper") != std::string::npos) found = true;
    check(found, "shipped cpp.xml finds a plain function");
    std::vector<FuncItem> py;
    check(ScanFunctions("class A:\n    def m(self):\n        pass\ndef top():\n    pass\n",
                        "python", py) &&
              py.size() >= 2,
          "shipped python.xml scans");
  }
  printf("%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}

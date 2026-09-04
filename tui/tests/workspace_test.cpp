// Tests for tui/shim/workspace (Phase 4.5): the GUI .workspace format —
// projects/folders/files, path conversion, malformed input.
//
// Linux port of tui/tests/workspace_test.mm (NSFileManager → GLib;
// NSTemporaryDirectory → g_dir_make_tmp). Assertions unchanged; the
// /private/var tail-compare is kept as written (harmless on Linux).

#include "../shim/workspace.h"

#include <glib.h>
#include <glib/gstdio.h>

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

static std::string join(const std::string &a, const char *b) {
  gchar *p = g_build_filename(a.c_str(), b, nullptr);
  std::string out = p;
  g_free(p);
  return out;
}

int main() {
  gchar *tmp = g_dir_make_tmp("tui-ws-test-XXXXXX", nullptr);
  if (!tmp) {
    fprintf(stderr, "cannot create temp dir\n");
    return 1;
  }
  std::string dir = tmp;
  g_free(tmp);

  std::string ws = join(dir, "demo.workspace");
  const char *xml =
      "<NotepadPlus>"
      "<Project name=\"Main\">"
      "<File name=\"src/a.cpp\"/>"
      "<Folder name=\"Docs\"><File name=\"readme.md\"/>"
      "<Folder name=\"Deep\"><File name=\"/abs/path.txt\"/></Folder></Folder>"
      "<File name=\"C:\\win\\style.h\"/>"
      "</Project>"
      "<Project name=\"Second\"><File name=\"b.txt\"/></Project>"
      "</NotepadPlus>";
  g_file_set_contents(ws.c_str(), xml, -1, nullptr);

  WsNode root;
  check(LoadWorkspace(ws, root), "workspace loads");
  check(root.children.size() == 2, "two projects");
  const WsNode &main0 = root.children[0];
  check(main0.name == "Main" && !main0.isFile, "project node");
  check(main0.children.size() == 3, "project children: file + folder + winfile");
  check(main0.children[0].isFile && main0.children[0].name == "a.cpp" &&
            main0.children[0].path == dir + "/src/a.cpp",
        "relative path resolved against the workspace dir");
  const WsNode &docs = main0.children[1];
  check(!docs.isFile && docs.name == "Docs" && docs.children.size() == 2, "nested folder");
  check(docs.children[1].children.size() == 1 &&
            docs.children[1].children[0].path == "/abs/path.txt",
        "absolute path kept; deep nesting works");
  check(main0.children[2].path == "C:/win/style.h", "Windows path normalized but kept unresolved");
  check(root.children[1].name == "Second", "second project");
  check(root.name == "demo.workspace", "root carries the workspace file name");

  {  // Folder-as-Workspace listing: hidden skipped, dirs first, ci-sorted.
    std::string fdir = join(dir, "ftree");
    g_mkdir_with_parents(join(fdir, "zeta").c_str(), 0700);
    g_mkdir_with_parents(join(fdir, "Alpha").c_str(), 0700);
    for (const char *f : {"beta.txt", "AAA.txt", ".hidden"})
      g_file_set_contents(join(fdir, f).c_str(), "x", -1, nullptr);
    std::vector<WsNode> kids;
    check(ListFolderChildren(fdir, kids), "folder lists");
    check(kids.size() == 4, "hidden file skipped");
    check(kids.size() == 4 && !kids[0].isFile && kids[0].name == "Alpha" && !kids[1].isFile &&
              kids[1].name == "zeta",
          "directories first, case-insensitive order");
    check(kids.size() == 4 && kids[2].isFile && kids[2].name == "AAA.txt" &&
              kids[3].name == "beta.txt",
          "files after, case-insensitive order");
    // NSURL may resolve /var → /private/var: compare the tail.
    check(kids.size() == 4 && kids[0].path.size() > 12 &&
              kids[0].path.substr(kids[0].path.size() - 12) == "/ftree/Alpha",
          "directories carry their full path");
    std::vector<WsNode> none;
    check(!ListFolderChildren(join(fdir, "nope"), none), "unreadable directory → false");

    // Linux addition: case-insensitivity across the case boundary that the
    // C locale's byte order would get wrong ("Zed" must follow "beta").
    g_file_set_contents(join(fdir, "Zed.txt").c_str(), "x", -1, nullptr);
    std::vector<WsNode> kids2;
    check(ListFolderChildren(fdir, kids2) && kids2.size() == 5 && kids2[3].name == "beta.txt" &&
              kids2[4].name == "Zed.txt",
          "case-insensitive across upper/lower boundary");
  }

  WsNode bad;
  check(!LoadWorkspace(join(dir, "missing.workspace"), bad), "missing file → false");
  std::string junk = join(dir, "junk.workspace");
  g_file_set_contents(junk.c_str(), "<oops>", -1, nullptr);
  check(!LoadWorkspace(junk, bad), "malformed XML → false");
  std::string foreign = join(dir, "foreign.workspace");
  g_file_set_contents(foreign.c_str(), "<Other><Project name=\"X\"/></Other>", -1, nullptr);
  check(!LoadWorkspace(foreign, bad), "wrong root element → false");
  std::string empty = join(dir, "empty.workspace");
  g_file_set_contents(empty.c_str(), "<NotepadPlus/>", -1, nullptr);
  check(!LoadWorkspace(empty, bad), "no projects → false");

  printf("%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}

// Tests for tui/shim/interop (Phase 4): GUI preference parsing (typed,
// tri-state, per-language overrides, garbage tolerance), the config.xml
// reader/writer, and the mtime FileWatch. Exit 0 = all pass.
//
// Linux port of tui/tests/interop_test.mm: the NSDictionary literals became
// GuiDefaults snapshots (string-valued, as config.xml carries them); the
// two macOS-only shapes — a non-dictionary "tabOverrides" and a non-string
// override key — cannot be expressed against the typed struct and are
// replaced by an unparseable override body. NSTemporaryDirectory →
// g_dir_make_tmp. Adds LoadGuiEditorPrefs over a scratch config.xml
// (prefs.c's attribute spellings) and SetGuiConfigAttr round-trips.

#include "../shim/interop.h"

#include <sys/time.h>
#include <unistd.h>

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

static std::string tmpFile(const char *name, const char *content) {
  gchar *p = g_build_filename(g_tmp.c_str(), name, nullptr);
  g_file_set_contents(p, content, -1, nullptr);
  std::string out = p;
  g_free(p);
  return out;
}

static std::string readAll(const std::string &path) {
  gchar *data = nullptr;
  gsize len = 0;
  if (!g_file_get_contents(path.c_str(), &data, &len, nullptr)) return "";
  std::string out(data, len);
  g_free(data);
  return out;
}

// Push a file's mtime around without sleeping across seconds.
static void setMtime(const std::string &path, long sec) {
  struct timeval tv[2] = {{sec, 0}, {sec, 0}};
  utimes(path.c_str(), tv);
}

int main() {
  gchar *tmp = g_dir_make_tmp("tui-interop-test-XXXXXX", nullptr);
  if (!tmp) {
    fprintf(stderr, "cannot create temp dir\n");
    return 1;
  }
  g_tmp = tmp;
  g_free(tmp);

  // ── ParseGuiEditorPrefs ───────────────────────────────────────────────────
  {
    GuiEditorPrefs p = ParseGuiEditorPrefs(GuiDefaults{});
    check(p.tabWidth == -1 && p.useTabs == -1 && p.wordWrap == -1 && p.indentGuides == -1 &&
              p.showWhitespace == -1 && p.showEOL == -1 && p.edgeColumn == -1 &&
              p.edgeMode == -1 && p.tabOverrides.empty(),
          "empty snapshot: everything unset");
  }
  {
    GuiDefaults d;
    d.values = {
        {"tabWidth", "8"},     {"useTabs", "yes"},         {"wordWrap", "no"},
        {"showIndentGuides", "show"}, {"showWhitespace", "1"}, {"showEOL", "hide"},
        {"edgeColumn", "100"}, {"edgeMode", "1"},
    };
    GuiEditorPrefs p = ParseGuiEditorPrefs(d);
    check(p.tabWidth == 8, "tabWidth parsed");
    check(p.useTabs == 1, "useTabs yes → 1");
    check(p.wordWrap == 0, "wordWrap no → 0");
    check(p.indentGuides == 1, "indent guides parsed (show)");
    check(p.showWhitespace == 1 && p.showEOL == 0, "whitespace/EOL parsed (1 / hide)");
    check(p.edgeColumn == 100 && p.edgeMode == 1, "edge column/mode parsed");
  }
  {
    // Wrong types and out-of-range values are treated as absent.
    GuiDefaults d;
    d.values = {
        {"tabWidth", "eight"}, {"useTabs", "maybe"}, {"edgeMode", "7"}, {"edgeColumn", "-3"},
        {"wordWrap", ""},
    };
    d.tabOverrides["python"] = {{"tabSize", "two"}, {"useTabs", "sometimes"}};
    GuiEditorPrefs p = ParseGuiEditorPrefs(d);
    check(p.tabWidth == -1 && p.useTabs == -1 && p.edgeMode == -1 && p.edgeColumn == -1 &&
              p.wordWrap == -1 && p.tabOverrides.empty(),
          "garbage values fall back to unset");
  }
  {
    GuiDefaults d;
    d.values = {{"tabWidth", "4"}};
    d.tabOverrides["python"] = {{"tabSize", "2"}, {"useTabs", "no"}};
    d.tabOverrides["makefile"] = {{"useTabs", "yes"}};
    d.tabOverrides["broken"] = {{"tabSize", "0"}};  // out of range AND no useTabs → dropped
    d.tabOverrides[""] = {{"tabSize", "3"}};        // empty key → dropped
    GuiEditorPrefs p = ParseGuiEditorPrefs(d);
    check(p.tabOverrides.size() == 2, "only well-formed overrides kept");
    check(p.tabOverrides.at("python").tabWidth == 2 && p.tabOverrides.at("python").useTabs == 0,
          "python override parsed");
    check(p.tabOverrides.at("makefile").tabWidth == -1 &&
              p.tabOverrides.at("makefile").useTabs == 1,
          "partial override keeps unset fields");
  }

  // ── LoadGuiEditorPrefs over a scratch config.xml ─────────────────────────
  {
    std::string cfg = tmpFile(
        "config.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
        "<NotepadPlus>\n"
        "    <GUIConfigs>\n"
        "        <GUIConfig name=\"TabBar\" closeButton=\"yes\" />\n"
        "        <GUIConfig name=\"TabSetting\" replaceBySpace=\"no\" size=\"8\" />\n"
        "        <GUIConfig name=\"TabCustom\">\n"
        "            <Language name=\"makefile\" tabSize=\"8\" useTabs=\"yes\" />\n"
        "            <Language name=\"python\" tabSize=\"2\" useTabs=\"no\" />\n"
        "        </GUIConfig>\n"
        "        <GUIConfig name=\"ScintillaPrimaryView\" lineNumberMargin=\"show\" "
        "Wrap=\"yes\" indentGuideLine=\"hide\" whiteSpaceShow=\"show\" eolShow=\"hide\" "
        "eolMode=\"0\" edgeMultiColumnPos=\"100\" isEdgeBgMode=\"no\" "
        "lineHeightMultiplier=\"1\" edgeMode=\"1\" />\n"
        "    </GUIConfigs>\n"
        "</NotepadPlus>\n");
    GuiEditorPrefs p = LoadGuiEditorPrefs(cfg);
    check(p.tabWidth == 8 && p.useTabs == 1, "config.xml: TabSetting size + inverted replaceBySpace");
    check(p.wordWrap == 1 && p.indentGuides == 0, "config.xml: Wrap / indentGuideLine");
    check(p.showWhitespace == 1 && p.showEOL == 0, "config.xml: whiteSpaceShow / eolShow");
    check(p.edgeColumn == 100 && p.edgeMode == 1, "config.xml: edge column + Linux edgeMode attr");
    check(p.tabOverrides.size() == 2 && p.tabOverrides.at("python").tabWidth == 2 &&
              p.tabOverrides.at("python").useTabs == 0 &&
              p.tabOverrides.at("makefile").tabWidth == 8 &&
              p.tabOverrides.at("makefile").useTabs == 1,
          "config.xml: TabCustom overrides");

    // prefs.c's finalise rules: bg flag → 2; empty column → OFF; a bare
    // column without any mode attribute stays OFF (as the GUI reads it).
    std::string bg = tmpFile("config_bg.xml",
                             "<NotepadPlus><GUIConfigs><GUIConfig name=\"ScintillaPrimaryView\" "
                             "edgeMultiColumnPos=\"80\" isEdgeBgMode=\"yes\" /></GUIConfigs></NotepadPlus>");
    GuiEditorPrefs pb = LoadGuiEditorPrefs(bg);
    check(pb.edgeColumn == 80 && pb.edgeMode == 2, "config.xml: isEdgeBgMode=yes → background");
    std::string off = tmpFile("config_off.xml",
                              "<NotepadPlus><GUIConfigs><GUIConfig name=\"ScintillaPrimaryView\" "
                              "edgeMultiColumnPos=\"\" isEdgeBgMode=\"no\" edgeMode=\"1\" /></GUIConfigs></NotepadPlus>");
    GuiEditorPrefs po = LoadGuiEditorPrefs(off);
    check(po.edgeColumn == 0 && po.edgeMode == 0, "config.xml: empty column forces OFF");
    std::string bare = tmpFile("config_bare.xml",
                               "<NotepadPlus><GUIConfigs><GUIConfig name=\"ScintillaPrimaryView\" "
                               "edgeMultiColumnPos=\"80\" isEdgeBgMode=\"no\" /></GUIConfigs></NotepadPlus>");
    GuiEditorPrefs pr = LoadGuiEditorPrefs(bare);
    check(pr.edgeColumn == 80 && pr.edgeMode == -1, "config.xml: bare column leaves mode unset");

    GuiEditorPrefs none = LoadGuiEditorPrefs(g_tmp + "/nope/config.xml");
    check(none.tabWidth == -1 && none.tabOverrides.empty(), "missing config.xml: everything unset");
    std::string trunc = tmpFile("config_trunc.xml",
                                "<NotepadPlus><GUIConfigs><GUIConfig name=\"TabSetting\" "
                                "replaceBySpace=\"yes\" size=\"3\" /><GUIConfig name=\"Tab");
    GuiEditorPrefs pt = LoadGuiEditorPrefs(trunc);
    check(pt.tabWidth == 3 && pt.useTabs == 0, "truncated config.xml keeps what parsed");

    // ── SetGuiConfigAttr ────────────────────────────────────────────────────
    check(SetGuiConfigAttr(cfg, "TabSetting", "size", "2"), "set existing attribute");
    check(SetGuiConfigAttr(cfg, "TabSetting", "replaceBySpace", "yes"), "set second attribute");
    check(SetGuiConfigAttr(cfg, "ScintillaPrimaryView", "Wrap", "no"), "set attr in long tag");
    check(SetGuiConfigAttr(cfg, "ScintillaPrimaryView", "newAttr", "a&b"), "append new attribute");
    check(SetGuiConfigAttr(cfg, "Brand New", "flag", "yes"), "create missing group");
    GuiEditorPrefs q = LoadGuiEditorPrefs(cfg);
    check(q.tabWidth == 2 && q.useTabs == 0, "written TabSetting reads back");
    check(q.wordWrap == 0 && q.showWhitespace == 1, "written Wrap reads back, siblings intact");
    std::string body = readAll(cfg);
    check(body.find("newAttr=\"a&amp;b\" />") != std::string::npos, "appended attr escaped, self-close kept");
    check(body.find("<GUIConfig name=\"Brand New\" flag=\"yes\" />\n    </GUIConfigs>") !=
              std::string::npos,
          "new group inserted before </GUIConfigs>");
    check(body.find("closeButton=\"yes\"") != std::string::npos &&
              body.find("<Language name=\"python\"") != std::string::npos,
          "unrelated content preserved byte-for-byte");
    check(q.tabOverrides.size() == 2, "nested groups still parse after edits");
    std::string fresh = g_tmp + "/fresh.xml";
    check(SetGuiConfigAttr(fresh, "TabSetting", "size", "6"), "missing file is created");
    check(LoadGuiEditorPrefs(fresh).tabWidth == 6, "created file reads back");
    check(!SetGuiConfigAttr(g_tmp + "/nope/x.xml", "A", "b", "c"), "unwritable path → false");
  }

  // ── FileWatch ─────────────────────────────────────────────────────────────
  {
    std::string a = tmpFile("tui_interop_a.txt", "a");
    std::string b = tmpFile("tui_interop_b.txt", "b");
    std::string ghost = g_tmp + "/tui_interop_ghost.txt";
    unlink(ghost.c_str());
    setMtime(a, 1000000);
    setMtime(b, 1000000);

    FileWatch w({a, b, ghost});
    check(w.poll().empty(), "no change right after construction");

    setMtime(a, 1000010);
    std::vector<size_t> ch = w.poll();
    check(ch.size() == 1 && ch[0] == 0, "touched file reported once");
    check(w.poll().empty(), "reported change is consumed");

    setMtime(a, 1000020);
    setMtime(b, 1000020);
    ch = w.poll();
    check(ch.size() == 2 && ch[0] == 0 && ch[1] == 1, "both changes reported");

    tmpFile("tui_interop_ghost.txt", "now exists");
    ch = w.poll();
    check(ch.size() == 1 && ch[0] == 2, "appearing file is a change");

    unlink(ghost.c_str());
    ch = w.poll();
    check(ch.size() == 1 && ch[0] == 2, "vanishing file is a change");
    check(w.poll().empty(), "steady state after vanish");

    unlink(a.c_str());
    unlink(b.c_str());
  }
  printf("%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}

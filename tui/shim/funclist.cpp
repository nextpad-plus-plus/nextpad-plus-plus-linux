// Function-list engine (Phase 4.5) — see funclist.h. Linux body: an adapter
// over src/funclist_xml.c (npp_core), the GUI's own port of the macOS XML
// engine (FunctionListPanel.mm → funclist.mm → funclist_xml.c share one
// pipeline: classRange/function/nameExpr, commentExpr exclusion, className
// grouping). GRegex is PCRE, so funclist.mm's PCRE→ICU rewriter and its
// (?(DEFINE)/(?& bail-outs do not exist here. flxml resolves the rules
// itself — <user dir>/functionList/<lang>.xml, then RESOURCES_DIR/
// functionList/<lang>.xml, exact case then lowercased (funclist_xml.h) —
// through src/paths.c, so `-settingsDir=` / npp_paths_set_override() is the
// test hook (flxml has no $NPP_TUI_FUNCLIST_DIR; see funclist_test.cpp).
//
// The hardcoded-regex fallback (funclist.mm:319-531, itself the GUI's
// phases 1–2) IS ported below: flxml_scan returns FALSE for languages with
// no parser XML (Swift among them — stock N++ ships none), and the GTK
// panel's fallback (src/funclist.c do_parse) writes straight into its
// GtkTreeStore, so it cannot be reused headless. Offsets are UTF-8 bytes
// end to end (macOS: UTF-16 units) — line numbers are identical, name
// truncation counts characters.
//
// Dropped from the fallback: userFuncPattern (funclist.mm:325-340), the
// user-override read in the SIMPLIFIED format. On Linux it is dead code —
// a <user dir>/functionList/<lang>.xml with a compilable mainExpr is
// already consumed by flxml_scan (same regex engine), so the fallback can
// never observe one it would compile.

#include "funclist.h"

#include <glib.h>

#include <algorithm>
#include <cstring>
#include <set>

extern "C" {
#include "funclist_xml.h"  // plain C header (no extern "C" guard of its own)
}

namespace npptui {

namespace {

struct FLTreeItem {
  std::string name;
  long line = 1;
  bool isNode = false;
  std::vector<FLTreeItem> children;
};

// 1-based line for a byte offset (funclist.mm lineForPos, over bytes).
long lineForPos(const std::vector<size_t> &offsets, size_t pos) {
  size_t lo = 0, hi = offsets.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (offsets[mid] <= pos) lo = mid + 1;
    else hi = mid;
  }
  return (long)lo;
}

bool contains(std::initializer_list<const char *> list, const std::string &lang) {
  for (const char *l : list)
    if (lang == l) return true;
  return false;
}

// Per-language function pattern (FunctionListPanel.mm _funcPatternForLanguage,
// verbatim; the generic C-style pattern at the end means there is ALWAYS one).
const char *funcPatternForLanguage(const std::string &lang) {
  if (lang == "python") return "(?m)^[ \\t]*(?:async\\s+)?def\\s+(\\w+)\\s*\\(";
  if (lang == "ruby") return "(?m)^[ \\t]*def\\s+(\\w+)";
  if (lang == "bash") return "(?m)^[ \\t]*(\\w+)\\s*\\(\\s*\\)";
  if (contains({"javascript", "javascript.js", "typescript"}, lang))
    return "(?m)(?:function\\s+(\\w+)\\s*\\(|(?:async\\s+)?(\\w+)\\s*\\([^)]*\\)\\s*[:{])";
  if (lang == "swift")
    return "(?m)^[ \\t]*(?:@\\w+\\s+)*(?:(?:public|private|internal|fileprivate|open|"
           "static|class|override|mutating|final)\\s+)*func\\s+(\\w+)";
  if (contains({"c", "cpp", "objc", "java", "cs", "go", "d", "actionscript", "rc"}, lang))
    return "(?m)^[\\t ]*(?:[\\w\\*<>\\[\\],&:\\s]+\\s+)(\\w+)\\s*\\([^)]*\\)\\s*"
           "(?:const\\s*)?(?:override\\s*)?(?:noexcept\\s*)?\\{";
  if (lang == "php")
    return "(?m)^[ \\t]*(?:public\\s+|private\\s+|protected\\s+|static\\s+)*function\\s+(\\w+)\\s*\\(";
  if (lang == "go") return "(?m)^func\\s+(?:\\([^)]+\\)\\s+)?(\\w+)\\s*\\(";
  if (lang == "rust") return "(?m)^\\s*(?:pub(?:\\([^)]*\\))?\\s+)?(?:async\\s+)?fn\\s+(\\w+)";
  if (lang == "lua")
    return "(?m)(?:function\\s+(\\w[\\w.:]*)\\s*\\(|local\\s+function\\s+(\\w+)\\s*\\()";
  if (lang == "perl") return "(?m)^\\s*sub\\s+(\\w+)";
  if (lang == "haskell") return "(?m)^(\\w+)\\s+::";
  if (lang == "r") return "(?m)(\\w+)\\s*<-\\s*function\\s*\\(";
  if (lang == "powershell") return "(?m)^\\s*function\\s+(\\w[\\w-]*)";
  if (lang == "pascal") return "(?m)^\\s*(?:procedure|function)\\s+(\\w+)";
  if (contains({"fortran", "fortran77"}, lang))
    return "(?mi)^\\s*(?:(?:integer|real|double\\s+precision|complex|logical|character|"
           "subroutine|function|program)\\s+)(\\w+)";
  if (lang == "ada") return "(?m)^\\s*(?:procedure|function)\\s+(\\w+)";
  if (lang == "vb")
    return "(?mi)^\\s*(?:public\\s+|private\\s+|friend\\s+)?(?:sub|function|property)\\s+(\\w+)";
  if (lang == "sql" || lang == "mssql")
    return "(?mi)^\\s*create\\s+(?:or\\s+replace\\s+)?(?:function|procedure|trigger)\\s+(\\w+)";
  if (lang == "latex") return "(?m)\\\\(?:section|subsection|subsubsection|chapter|part)\\{([^}]+)\\}";
  if (lang == "makefile") return "(?m)^([\\w][\\w.-]*)\\s*:";
  if (lang == "cmake") return "(?mi)^\\s*(?:function|macro)\\s*\\(\\s*(\\w+)";
  if (lang == "nim") return "(?m)^\\s*(?:proc|func|method|iterator|template|macro)\\s+(\\w+)";
  if (lang == "erlang") return "(?m)^(\\w+)\\s*\\(";
  if (lang == "asm") return "(?m)^(\\w+):";
  if (lang == "vhdl") return "(?mi)^\\s*(?:procedure|function)\\s+(\\w+)";
  if (lang == "verilog") return "(?m)^\\s*(?:module|task|function)\\s+(\\w+)";
  if (lang == "css") return "(?m)^([.#]?[\\w][\\w.#>~+\\-\\s,:]*)\\s*\\{";
  if (lang == "ini" || lang == "props") return "(?m)^\\[([^\\]]+)\\]";
  if (lang == "yaml") return "(?m)^(\\w[\\w-]*)\\s*:";
  if (lang == "toml") return "(?m)^\\[([^\\]]+)\\]";
  if (lang == "batch") return "(?mi)^\\s*:(\\w+)";
  if (lang == "coffeescript") return "(?m)^\\s*(\\w+)\\s*[=:]\\s*(?:\\([^)]*\\))?\\s*[-=]>";
  if (lang == "xml" || lang == "html" || lang == "asp")
    return "(?m)(<\\w+(?:\\s+[\\w:-]+\\s*=\\s*\"[^\"]*\")*\\s*/?>)";
  if (lang == "json") return "(?m)^\\s*\"(\\w[\\w\\s]*)\"\\s*:";
  return "(?m)^[\\t ]*[\\w\\*]+(?:[\\s\\*]+)(\\w+)\\s*\\([^)]*\\)\\s*\\{";
}

const char *classPatternForLanguage(const std::string &lang) {
  if (contains({"c", "cpp", "objc", "swift", "java", "cs", "typescript", "javascript",
                "javascript.js", "go", "d", "rust", "kotlin"},
               lang))
    return "(?m)^[ \\t]*(?:public\\s+|private\\s+|protected\\s+|internal\\s+|"
           "abstract\\s+|final\\s+|static\\s+)*(?:class|struct|protocol|interface|enum)"
           "\\s+(\\w+)";
  if (lang == "python") return "(?m)^class\\s+(\\w+)";
  if (lang == "ruby") return "(?m)^[ \\t]*(?:class|module)\\s+(\\w+)";
  if (lang == "php") return "(?m)^[ \\t]*(?:abstract\\s+|final\\s+)?class\\s+(\\w+)";
  return nullptr;
}

size_t findBraceOpen(const std::string &text, size_t start) {
  size_t len = text.size();
  for (size_t i = start; i < len; i++) {
    char c = text[i];
    if (c == '{') return i;
    if (c == ';') return len;  // declaration only, no body
  }
  return len;
}

size_t findBraceClose(const std::string &text, size_t start) {
  size_t len = text.size();
  long depth = 0;
  for (size_t i = start; i < len; i++) {
    char c = text[i];
    if (c == '{') depth++;
    else if (c == '}') {
      depth--;
      if (depth <= 0) return i;
    }
  }
  return len;
}

GRegex *compileFallback(const char *pattern) {
  return g_regex_new(pattern, (GRegexCompileFlags)(G_REGEX_MULTILINE | G_REGEX_OPTIMIZE),
                     (GRegexMatchFlags)0, nullptr);
}

// Capture group `n` of the current match, or false when it did not
// participate (NSNotFound in the .mm).
bool groupRange(GMatchInfo *mi, int n, size_t &start, size_t &end) {
  gint s = -1, e = -1;
  if (!g_match_info_fetch_pos(mi, n, &s, &e) || s < 0 || e < s) return false;
  start = (size_t)s;
  end = (size_t)e;
  return true;
}

// Phases 1–2 of the GUI's hardcoded scan: classes by pattern + brace range,
// functions by pattern, methods nested into the class whose body contains
// them; classes without children are dropped; everything document-ordered.
void scanHardcoded(const std::string &text, const std::string &lang,
                   const std::vector<size_t> &offsets, std::vector<FLTreeItem> &roots) {
  struct ClassRange {
    size_t start, end;
    size_t nodeIdx;
  };
  std::vector<FLTreeItem> classNodes;
  std::vector<ClassRange> classRanges;
  if (const char *classPattern = classPatternForLanguage(lang)) {
    GRegex *classRE = compileFallback(classPattern);
    GMatchInfo *mi = nullptr;
    if (classRE && g_regex_match_full(classRE, text.c_str(), (gssize)text.size(), 0,
                                      (GRegexMatchFlags)0, &mi, nullptr)) {
      do {
        size_t ms, me, ns, ne;
        if (!groupRange(mi, 0, ms, me) || !groupRange(mi, 1, ns, ne)) continue;
        FLTreeItem node;
        node.name = text.substr(ns, ne - ns);
        node.line = lineForPos(offsets, ms);
        node.isNode = true;
        size_t bodyStart = findBraceOpen(text, me);
        size_t bodyEnd = findBraceClose(text, bodyStart);
        classRanges.push_back({bodyStart, bodyEnd, classNodes.size()});
        classNodes.push_back(std::move(node));
      } while (g_match_info_next(mi, nullptr));
    }
    if (mi) g_match_info_free(mi);
    if (classRE) g_regex_unref(classRE);
  }

  GRegex *funcRE = compileFallback(funcPatternForLanguage(lang));
  if (!funcRE) return;
  std::vector<FLTreeItem> topLeaves;
  std::set<std::string> added;
  GMatchInfo *mi = nullptr;
  if (g_regex_match_full(funcRE, text.c_str(), (gssize)text.size(), 0, (GRegexMatchFlags)0, &mi,
                         nullptr)) {
    do {
      size_t ms, me, ns, ne;
      if (!groupRange(mi, 0, ms, me)) continue;
      bool have = groupRange(mi, 1, ns, ne);
      if (!have && g_match_info_get_match_count(mi) > 2) have = groupRange(mi, 2, ns, ne);
      if (!have) continue;
      std::string name = text.substr(ns, ne - ns);
      if (g_utf8_strlen(name.c_str(), -1) > 50) {
        gchar *head = g_utf8_substring(name.c_str(), 0, 50);
        name = std::string(head) + "…";
        g_free(head);
      }
      std::replace(name.begin(), name.end(), '\n', ' ');
      std::replace(name.begin(), name.end(), '\r', ' ');
      std::string key = name + "_" + std::to_string(ms);
      if (!added.insert(key).second) continue;
      size_t pos = ns;
      FLTreeItem leaf;
      leaf.name = name;
      leaf.line = lineForPos(offsets, pos);
      bool nested = false;
      for (const ClassRange &cr : classRanges) {
        if (pos > cr.start && pos < cr.end) {
          classNodes[cr.nodeIdx].children.push_back(std::move(leaf));
          nested = true;
          break;
        }
      }
      if (!nested) topLeaves.push_back(std::move(leaf));
    } while (g_match_info_next(mi, nullptr));
  }
  if (mi) g_match_info_free(mi);
  g_regex_unref(funcRE);

  for (FLTreeItem &leaf : topLeaves) roots.push_back(std::move(leaf));
  for (FLTreeItem &node : classNodes) {
    if (node.children.empty()) continue;  // classes without methods are dropped
    std::stable_sort(node.children.begin(), node.children.end(),
                     [](const FLTreeItem &a, const FLTreeItem &b) { return a.line < b.line; });
    roots.push_back(std::move(node));
  }
  std::stable_sort(roots.begin(), roots.end(),
                   [](const FLTreeItem &a, const FLTreeItem &b) { return a.line < b.line; });
}

void flattenXml(const GPtrArray *items, std::vector<FuncItem> &out) {
  for (guint i = 0; items && i < items->len; i++) {
    const FlXmlItem *r = static_cast<const FlXmlItem *>(g_ptr_array_index(items, i));
    out.push_back({r->name ? r->name : "", r->line, 0, r->is_node != FALSE});
    for (guint j = 0; r->children && j < r->children->len; j++) {
      const FlXmlItem *c = static_cast<const FlXmlItem *>(g_ptr_array_index(r->children, j));
      out.push_back({c->name ? c->name : "", c->line, 1, false});
    }
  }
}

}  // namespace

bool ScanFunctions(const std::string &text, const std::string &lang, std::vector<FuncItem> &out) {
  out.clear();
  if (lang.empty()) return false;
  // An NSString could not be built from invalid UTF-8 (→ false in the .mm);
  // GRegex refuses it the same way.
  if (!g_utf8_validate(text.c_str(), (gssize)text.size(), nullptr)) return false;

  // The GTK panel's "udl:<name>" buffer tag → the bare UDL name both ports
  // use for functionList/<name>.xml.
  std::string xmlLang = lang.compare(0, 4, "udl:") == 0 ? lang.substr(4) : lang;

  // XML rules first (GUI parity). flxml tries the exact-case name and then
  // the lowercased one itself, so the original spelling goes in.
  GPtrArray *items = nullptr;
  if (flxml_scan(text.c_str(), (gssize)text.size(), xmlLang.c_str(), &items)) {
    flattenXml(items, out);
    flxml_items_free(items);
    return true;
  }

  // No XML rules (or none usable): the GUI's hardcoded-regex fallback,
  // which always has at least the generic C-style pattern.
  gchar *lower = g_ascii_strdown(xmlLang.c_str(), -1);
  std::string lang_lc = lower;
  g_free(lower);

  // Line offset table (offset of each line start; index 0 = line 1).
  std::vector<size_t> offsets;
  offsets.push_back(0);
  for (size_t i = 0; i < text.size(); i++)
    if (text[i] == '\n') offsets.push_back(i + 1);

  std::vector<FLTreeItem> roots;
  scanHardcoded(text, lang_lc, offsets, roots);

  // Flatten: class nodes with their children indented one level.
  for (const FLTreeItem &r : roots) {
    out.push_back({r.name, r.line, 0, r.isNode});
    for (const FLTreeItem &c : r.children) out.push_back({c.name, c.line, 1, false});
  }
  return true;
}

}  // namespace npptui

// GUI↔TUI interop (Phase 4) — see interop.h. Linux body: the GUI store is
// config.xml, read by a GMarkup SAX pass into the GuiDefaults snapshot
// (attribute strings, exactly as the file carries them) that
// ParseGuiEditorPrefs validates the way interop.mm validated the
// NSNumber-typed dictionary. FileWatch is interop.mm:63-84 with st_mtim.
//
// config.xml → GuiDefaults key mapping (src/prefs.c reader/writer):
//   TabSetting            size → tabWidth; replaceBySpace (inverted) → useTabs
//   TabCustom/Language    name → tabOverrides[name]{tabSize, useTabs}
//   ScintillaPrimaryView  Wrap → wordWrap; indentGuideLine → showIndentGuides;
//                         whiteSpaceShow → showWhitespace; eolShow → showEOL;
//                         edgeMultiColumnPos → edgeColumn ("" = 0);
//                         edgeMode / isEdgeBgMode / edgeMultiColumnPos →
//                         edgeMode, resolved like prefs.c's finalise step
//                         (column ≤ 0 → 0; isEdgeBgMode=yes → 2; a Linux
//                         edgeMode attr wins; a bare column with no mode
//                         attr stays OFF, as the GUI reads it).
// Booleans keep the file's spelling (yes/no, show/hide, 1/0, true/false);
// ParseGuiEditorPrefs accepts all of them, anything else is "absent".

#include "interop.h"

#include <sys/stat.h>

#include <cstdlib>
#include <cstring>

#include <glib.h>

namespace npptui {

namespace {

// The NSNumber-typed lookups of interop.mm, over attribute strings.
// -1 = absent or not a boolean spelling.
int triState(const std::map<std::string, std::string> &d, const char *key) {
  auto it = d.find(key);
  if (it == d.end()) return -1;
  const char *v = it->second.c_str();
  if (!g_ascii_strcasecmp(v, "yes") || !g_ascii_strcasecmp(v, "true") ||
      !g_ascii_strcasecmp(v, "show"))
    return 1;
  if (!g_ascii_strcasecmp(v, "no") || !g_ascii_strcasecmp(v, "false") ||
      !g_ascii_strcasecmp(v, "hide"))
    return 0;
  char *end = nullptr;
  long n = strtol(v, &end, 10);
  if (end && end != v && *end == '\0') return n != 0 ? 1 : 0;  // NSNumber boolValue
  return -1;
}

long intValue(const std::map<std::string, std::string> &d, const char *key, long absent) {
  auto it = d.find(key);
  if (it == d.end()) return absent;
  const char *v = it->second.c_str();
  char *end = nullptr;
  long n = strtol(v, &end, 10);
  if (!end || end == v || *end != '\0') return absent;  // "eight" → wrong type
  return n;
}

}  // namespace

GuiEditorPrefs ParseGuiEditorPrefs(const GuiDefaults &defaults) {
  GuiEditorPrefs p;
  const auto &d = defaults.values;
  long tw = intValue(d, "tabWidth", -1);
  if (tw >= 1 && tw <= 64) p.tabWidth = (int)tw;
  p.useTabs = triState(d, "useTabs");
  p.wordWrap = triState(d, "wordWrap");
  p.indentGuides = triState(d, "showIndentGuides");
  p.showWhitespace = triState(d, "showWhitespace");
  p.showEOL = triState(d, "showEOL");
  long edge = intValue(d, "edgeColumn", -1);
  if (edge >= 0) p.edgeColumn = edge;
  long mode = intValue(d, "edgeMode", -1);
  if (mode >= 0 && mode <= 2) p.edgeMode = (int)mode;

  for (const auto &kv : defaults.tabOverrides) {
    if (kv.first.empty()) continue;
    GuiTabOverride o;
    long size = intValue(kv.second, "tabSize", -1);
    if (size >= 1 && size <= 64) o.tabWidth = (int)size;
    o.useTabs = triState(kv.second, "useTabs");
    if (o.tabWidth != -1 || o.useTabs != -1) p.tabOverrides[kv.first] = o;
  }
  return p;
}

// ── config.xml SAX ───────────────────────────────────────────────────────────

namespace {

struct ConfigState {
  GuiDefaults out;
  std::string group;  // current <GUIConfig name=…>
  // ScintillaPrimaryView edge pieces, resolved at the end of the element.
  std::string edgeCol, edgeMode, edgeBg;
  bool sawEdgeCol = false, sawEdgeMode = false, sawEdgeBg = false;
};

const char *attrOf(const gchar **names, const gchar **values, const char *key) {
  for (int i = 0; names[i]; i++)
    if (strcmp(names[i], key) == 0) return values[i];
  return nullptr;
}

bool isYes(const char *v) {
  return v && (!g_ascii_strcasecmp(v, "yes") || !g_ascii_strcasecmp(v, "true") ||
               !strcmp(v, "1"));
}
bool isNo(const char *v) {
  return v && (!g_ascii_strcasecmp(v, "no") || !g_ascii_strcasecmp(v, "false") ||
               !strcmp(v, "0"));
}

void onStart(GMarkupParseContext *, const gchar *element, const gchar **names,
             const gchar **values, gpointer user, GError **) {
  auto *st = static_cast<ConfigState *>(user);
  if (strcmp(element, "GUIConfig") == 0) {
    const char *name = attrOf(names, values, "name");
    st->group = name ? name : "";
    if (st->group == "TabSetting") {
      if (const char *v = attrOf(names, values, "size")) st->out.values["tabWidth"] = v;
      if (const char *v = attrOf(names, values, "replaceBySpace")) {
        // useTabs = !replaceBySpace (prefs.c:331); unrecognised → absent.
        if (isYes(v)) st->out.values["useTabs"] = "no";
        else if (isNo(v)) st->out.values["useTabs"] = "yes";
      }
    } else if (st->group == "ScintillaPrimaryView") {
      if (const char *v = attrOf(names, values, "Wrap")) st->out.values["wordWrap"] = v;
      if (const char *v = attrOf(names, values, "indentGuideLine"))
        st->out.values["showIndentGuides"] = v;
      if (const char *v = attrOf(names, values, "whiteSpaceShow"))
        st->out.values["showWhitespace"] = v;
      if (const char *v = attrOf(names, values, "eolShow")) st->out.values["showEOL"] = v;
      if (const char *v = attrOf(names, values, "edgeMultiColumnPos")) {
        st->sawEdgeCol = true;
        st->edgeCol = v;
      }
      if (const char *v = attrOf(names, values, "edgeMode")) {
        st->sawEdgeMode = true;
        st->edgeMode = v;
      }
      if (const char *v = attrOf(names, values, "isEdgeBgMode")) {
        st->sawEdgeBg = true;
        st->edgeBg = v;
      }
    }
    return;
  }
  if (strcmp(element, "Language") == 0 && st->group == "TabCustom") {
    const char *lang = attrOf(names, values, "name");
    if (!lang || !*lang) return;
    std::map<std::string, std::string> o;
    if (const char *v = attrOf(names, values, "tabSize")) o["tabSize"] = v;
    if (const char *v = attrOf(names, values, "useTabs")) o["useTabs"] = v;
    st->out.tabOverrides[lang] = std::move(o);
  }
}

void onEnd(GMarkupParseContext *, const gchar *element, gpointer user, GError **) {
  auto *st = static_cast<ConfigState *>(user);
  if (strcmp(element, "GUIConfig") != 0) return;
  if (st->group == "ScintillaPrimaryView") {
    // prefs.c:503 — an empty column attribute reads as 0.
    long col = -1;
    if (st->sawEdgeCol) {
      col = st->edgeCol.empty() ? 0 : strtol(st->edgeCol.c_str(), nullptr, 10);
      st->out.values["edgeColumn"] = std::to_string(col);
    }
    // prefs.c:518-522 + the finalise step at :734-741.
    int mode = -1;
    if (st->sawEdgeMode) mode = (int)strtol(st->edgeMode.c_str(), nullptr, 10);
    else if (st->sawEdgeBg && isYes(st->edgeBg.c_str())) mode = 2;
    if (st->sawEdgeCol && col <= 0) mode = 0;
    else if (mode > 0 && mode != 2) mode = 1;
    if (mode >= 0) st->out.values["edgeMode"] = std::to_string(mode);
    st->sawEdgeCol = st->sawEdgeMode = st->sawEdgeBg = false;
  }
  st->group.clear();
}

}  // namespace

GuiEditorPrefs LoadGuiEditorPrefs(const std::string &configPath) {
  gchar *data = nullptr;
  gsize len = 0;
  if (!g_file_get_contents(configPath.c_str(), &data, &len, nullptr))
    return ParseGuiEditorPrefs(GuiDefaults{});

  ConfigState st;
  GMarkupParser parser = {onStart, onEnd, nullptr, nullptr, nullptr};
  GMarkupParseContext *ctx =
      g_markup_parse_context_new(&parser, G_MARKUP_DEFAULT_FLAGS, &st, nullptr);
  // A truncated file still yields whatever parsed before the error — the
  // GUI reads it the same way (prefs.c keeps the partial state).
  if (g_markup_parse_context_parse(ctx, data, (gssize)len, nullptr))
    g_markup_parse_context_end_parse(ctx, nullptr);
  g_markup_parse_context_free(ctx);
  g_free(data);
  return ParseGuiEditorPrefs(st.out);
}

// ── config.xml writer (Preferences popup) ────────────────────────────────────

bool SetGuiConfigAttr(const std::string &configPath, const std::string &guiConfigName,
                      const std::string &attr, const std::string &value) {
  if (guiConfigName.empty() || attr.empty()) return false;

  std::string doc;
  {
    gchar *data = nullptr;
    gsize len = 0;
    if (g_file_get_contents(configPath.c_str(), &data, &len, nullptr)) {
      doc.assign(data, len);
      g_free(data);
    }
  }
  if (doc.empty()) {
    doc = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<NotepadPlus>\n    <GUIConfigs>\n"
          "    </GUIConfigs>\n</NotepadPlus>\n";
  }

  gchar *escVal = g_markup_escape_text(value.c_str(), -1);
  std::string escaped = escVal;
  g_free(escVal);

  // Locate the <GUIConfig name="X" …> start tag. The name attribute may use
  // either quote; the search is textual — config.xml is machine-written
  // (prefs.c) with one GUIConfig per line and no CDATA/comments in tags.
  size_t tagStart = std::string::npos, tagEnd = std::string::npos;
  for (size_t pos = doc.find("<GUIConfig"); pos != std::string::npos;
       pos = doc.find("<GUIConfig", pos + 10)) {
    char after = pos + 10 < doc.size() ? doc[pos + 10] : '\0';
    if (after != ' ' && after != '\t' && after != '\n' && after != '\r') continue;  // <GUIConfigs>
    size_t end = doc.find('>', pos);
    if (end == std::string::npos) break;
    std::string tag = doc.substr(pos, end - pos);
    for (const char *q : {"name=\"", "name='"}) {
      size_t n = tag.find(q);
      if (n == std::string::npos) continue;
      size_t vs = n + 6;
      size_t ve = tag.find(q[5], vs);
      if (ve != std::string::npos && tag.compare(vs, ve - vs, guiConfigName) == 0) {
        tagStart = pos;
        tagEnd = end;
      }
    }
    if (tagStart != std::string::npos) break;
  }

  if (tagStart == std::string::npos) {
    // No such group: append one before </GUIConfigs> (else </NotepadPlus>).
    gchar *escName = g_markup_escape_text(guiConfigName.c_str(), -1);
    std::string line = "        <GUIConfig name=\"" + std::string(escName) + "\" " + attr + "=\"" +
                       escaped + "\" />\n";
    g_free(escName);
    size_t at = doc.find("</GUIConfigs>");
    if (at == std::string::npos) at = doc.find("</NotepadPlus>");
    if (at == std::string::npos) return false;
    // Keep the closing tag's own indentation: insert at the line start.
    size_t lineStart = doc.rfind('\n', at);
    lineStart = lineStart == std::string::npos ? at : lineStart + 1;
    if (doc.compare(lineStart, at - lineStart, std::string(at - lineStart, ' ')) != 0)
      lineStart = at;
    doc.insert(lineStart, line);
  } else {
    std::string tag = doc.substr(tagStart, tagEnd - tagStart);  // without '>'
    size_t a = std::string::npos;
    for (size_t p = tag.find(attr); p != std::string::npos; p = tag.find(attr, p + 1)) {
      if (p == 0) continue;
      char before = tag[p - 1];
      size_t q = p + attr.size();
      if ((before == ' ' || before == '\t' || before == '\n' || before == '\r') && q < tag.size() &&
          tag[q] == '=' && q + 1 < tag.size() && (tag[q + 1] == '"' || tag[q + 1] == '\'')) {
        a = p;
        break;
      }
    }
    if (a != std::string::npos) {
      size_t vs = a + attr.size() + 2;
      char quote = tag[vs - 1];
      size_t ve = tag.find(quote, vs);
      if (ve == std::string::npos) return false;
      tag.replace(vs, ve - vs, escaped);
    } else {
      size_t ins = tag.size();
      while (ins > 0 && (tag[ins - 1] == ' ' || tag[ins - 1] == '\t' || tag[ins - 1] == '\n' ||
                         tag[ins - 1] == '\r'))
        ins--;
      bool selfClosing = ins > 0 && tag[ins - 1] == '/';
      if (selfClosing) ins--;
      while (ins > 0 && (tag[ins - 1] == ' ' || tag[ins - 1] == '\t')) ins--;
      std::string piece = " " + attr + "=\"" + escaped + "\"" + (selfClosing ? " /" : "");
      tag.replace(ins, tag.size() - ins, piece);
    }
    doc.replace(tagStart, tagEnd - tagStart, tag);
  }

  return g_file_set_contents(configPath.c_str(), doc.data(), (gssize)doc.size(), nullptr);
}

// ── FileWatch (interop.mm verbatim, st_mtimespec → st_mtim) ─────────────────

static double mtimeOf(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  return (double)st.st_mtim.tv_sec + (double)st.st_mtim.tv_nsec * 1e-9;
}

FileWatch::FileWatch(std::vector<std::string> paths) : paths_(std::move(paths)) {
  mtimes_.reserve(paths_.size());
  for (const std::string &p : paths_) mtimes_.push_back(mtimeOf(p));
}

std::vector<size_t> FileWatch::poll() {
  std::vector<size_t> changed;
  for (size_t i = 0; i < paths_.size(); i++) {
    double m = mtimeOf(paths_[i]);
    if (m != mtimes_[i]) {
      mtimes_[i] = m;
      changed.push_back(i);
    }
  }
  return changed;
}

}  // namespace npptui

// Workspace loader (Phase 4.5) — see workspace.h. Linux body: GMarkup SAX
// over the .workspace XML (workspace.mm walked an NSXMLDocument; the element
// set — NotepadPlus/Project/Folder/File — and the node shape are the same),
// GDir for the folder listing. convertPath is workspace.mm:8-17 verbatim.
//
// Ordering: workspace.mm sorted with localizedCaseInsensitiveCompare; here
// each name is case-folded and run through g_utf8_collate_key_for_filename
// (docs/12 §8.3), which additionally sorts embedded numbers naturally
// ("file2" < "file10") — the GUI's folder tree uses the same key.

#include "workspace.h"

#include <glib.h>

#include <algorithm>
#include <cstring>

namespace npptui {

namespace {

// Path conversion, ported from ProjectPanel.mm _convertPath:wsDir:.
std::string convertPath(const std::string &rawPath, const std::string &wsDir) {
  std::string path = rawPath;
  std::replace(path.begin(), path.end(), '\\', '/');
  if (!path.empty() && path[0] == '/') return path;
  if (path.size() >= 3 && path[1] == ':') return path;  // Windows drive path: unresolvable, keep for display
  if (path.compare(0, 2, "//") == 0) return path;       // UNC: keep for display
  gchar *joined = g_build_filename(wsDir.c_str(), path.c_str(), nullptr);
  std::string out = joined;
  g_free(joined);
  return out;
}

std::string baseName(const std::string &path) {
  gchar *b = g_path_get_basename(path.c_str());
  std::string out = b;
  g_free(b);
  return out;
}

const char *attrOf(const gchar **names, const gchar **values, const char *key) {
  for (int i = 0; names[i]; i++)
    if (strcmp(names[i], key) == 0) return values[i];
  return nullptr;
}

struct ParseState {
  WsNode *root = nullptr;
  std::string wsDir;
  // Ancestor chain; nullptr = inside an element the walker ignores (an
  // unknown tag, or a Folder/File outside any Project). Appending to the
  // top's children never moves an ancestor: each lives in ITS parent's
  // vector, which only grows when that parent is the top.
  std::vector<WsNode *> stack;
  bool bad = false;
};

void onStart(GMarkupParseContext *, const gchar *element, const gchar **names,
             const gchar **values, gpointer user, GError **) {
  auto *st = static_cast<ParseState *>(user);
  if (st->stack.empty()) {
    if (strcmp(element, "NotepadPlus") != 0) {
      st->bad = true;
      st->stack.push_back(nullptr);
      return;
    }
    st->stack.push_back(st->root);
    return;
  }
  WsNode *top = st->stack.back();
  if (!top) {
    st->stack.push_back(nullptr);
    return;
  }
  if (top == st->root) {
    if (strcmp(element, "Project") == 0) {
      WsNode proj;
      const char *n = attrOf(names, values, "name");
      proj.name = n ? n : "(project)";
      top->children.push_back(std::move(proj));
      st->stack.push_back(&top->children.back());
    } else {
      st->stack.push_back(nullptr);
    }
    return;
  }
  if (strcmp(element, "Folder") == 0) {
    WsNode folder;
    const char *n = attrOf(names, values, "name");
    folder.name = n ? n : "(folder)";
    top->children.push_back(std::move(folder));
    st->stack.push_back(&top->children.back());
    return;
  }
  if (strcmp(element, "File") == 0) {
    const char *raw = attrOf(names, values, "name");
    if (raw && *raw) {
      std::string abs = convertPath(raw, st->wsDir);
      WsNode file;
      file.name = baseName(abs);
      file.path = abs;
      file.isFile = true;
      top->children.push_back(std::move(file));
    }
    st->stack.push_back(nullptr);  // File carries no children
    return;
  }
  st->stack.push_back(nullptr);
}

void onEnd(GMarkupParseContext *, const gchar *, gpointer user, GError **) {
  auto *st = static_cast<ParseState *>(user);
  if (!st->stack.empty()) st->stack.pop_back();
}

}  // namespace

bool LoadWorkspace(const std::string &path, WsNode &outRoot) {
  outRoot = WsNode{};
  if (path.empty()) return false;
  gchar *data = nullptr;
  gsize len = 0;
  if (!g_file_get_contents(path.c_str(), &data, &len, nullptr)) return false;

  ParseState st;
  st.root = &outRoot;
  gchar *dir = g_path_get_dirname(path.c_str());
  st.wsDir = dir;
  g_free(dir);
  outRoot.name = baseName(path);

  GMarkupParser parser = {onStart, onEnd, nullptr, nullptr, nullptr};
  GMarkupParseContext *ctx =
      g_markup_parse_context_new(&parser, G_MARKUP_DEFAULT_FLAGS, &st, nullptr);
  bool ok = g_markup_parse_context_parse(ctx, data, (gssize)len, nullptr) &&
            g_markup_parse_context_end_parse(ctx, nullptr);
  g_markup_parse_context_free(ctx);
  g_free(data);
  if (!ok || st.bad) {
    outRoot = WsNode{};
    return false;
  }
  return !outRoot.children.empty();
}

bool ListFolderChildren(const std::string &dir, std::vector<WsNode> &out) {
  out.clear();
  GDir *d = g_dir_open(dir.c_str(), 0, nullptr);
  if (!d) return false;

  struct Entry {
    std::string name, path, key;
  };
  std::vector<Entry> dirs, files;
  const gchar *name;
  while ((name = g_dir_read_name(d)) != nullptr) {
    if (name[0] == '.') continue;  // NSDirectoryEnumerationSkipsHiddenFiles
    gchar *full = g_build_filename(dir.c_str(), name, nullptr);
    gchar *folded = g_utf8_casefold(name, -1);
    gchar *key = g_utf8_collate_key_for_filename(folded, -1);
    Entry e{name, full, key};
    (g_file_test(full, G_FILE_TEST_IS_DIR) ? dirs : files).push_back(std::move(e));
    g_free(key);
    g_free(folded);
    g_free(full);
  }
  g_dir_close(d);

  auto byName = [](const Entry &a, const Entry &b) {
    int c = a.key.compare(b.key);
    return c != 0 ? c < 0 : a.name < b.name;
  };
  std::sort(dirs.begin(), dirs.end(), byName);
  std::sort(files.begin(), files.end(), byName);
  for (Entry &e : dirs) {
    WsNode n;
    n.name = std::move(e.name);
    n.path = std::move(e.path);
    out.push_back(std::move(n));
  }
  for (Entry &e : files) {
    WsNode n;
    n.name = std::move(e.name);
    n.path = std::move(e.path);
    n.isFile = true;
    out.push_back(std::move(n));
  }
  return true;
}

}  // namespace npptui

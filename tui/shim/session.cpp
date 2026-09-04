// Session shim (T2.9) — see session.h. Linux body: tui-session.xml via
// GMarkup, the plist's keys kept as element/attribute names so the ported
// session_test asserts the same things:
//
//   <?xml version="1.0" encoding="UTF-8"?>
//   <TuiSession version="1" current="1">
//     <Buffer path="/abs/a.cpp" backup="/abs/bk/snap.txt" caret="120" firstVisible="4"/>
//   </TuiSession>
//
// path / backup are omitted when empty, exactly as session.mm omitted the
// dictionary keys; a Buffer with neither is not restorable and is skipped
// on both save and load. Writes go through g_file_set_contents (temp file +
// rename — the atomically:YES of NSData writeToFile).

#include "session.h"

#include <glib.h>

#include <cstdlib>
#include <cstring>

namespace npptui {

namespace {

struct ParseState {
  TuiSession session;
  bool sawRoot = false;
  bool bad = false;
  int depth = 0;
};

const char *attrOf(const gchar **names, const gchar **values, const char *key) {
  for (int i = 0; names[i]; i++)
    if (strcmp(names[i], key) == 0) return values[i];
  return nullptr;
}

long longAttr(const gchar **names, const gchar **values, const char *key) {
  const char *v = attrOf(names, values, key);
  if (!v) return 0;
  char *end = nullptr;
  long n = strtol(v, &end, 10);
  return (end && end != v) ? n : 0;  // NSNumber longValue of a non-number → 0
}

void onStart(GMarkupParseContext *, const gchar *element, const gchar **names,
             const gchar **values, gpointer user, GError **) {
  auto *st = static_cast<ParseState *>(user);
  st->depth++;
  if (st->depth == 1) {
    if (strcmp(element, "TuiSession") != 0) {
      st->bad = true;
      return;
    }
    st->sawRoot = true;
    const char *cur = attrOf(names, values, "current");
    size_t current = 0;
    if (cur) {
      char *end = nullptr;
      unsigned long long n = strtoull(cur, &end, 10);
      if (end && end != cur && *cur != '-') current = (size_t)n;
    }
    st->session.current = current;
    return;
  }
  if (st->depth == 2 && strcmp(element, "Buffer") == 0) {
    const char *p = attrOf(names, values, "path");
    const char *bk = attrOf(names, values, "backup");
    bool hasPath = p && *p;
    bool hasBackup = bk && *bk;
    if (!hasPath && !hasBackup) return;  // nothing restorable
    SessionBuffer b;
    if (hasPath) b.path = p;
    if (hasBackup) b.backup = bk;
    b.caret = longAttr(names, values, "caret");
    b.firstVisible = longAttr(names, values, "firstVisible");
    st->session.buffers.push_back(std::move(b));
  }
}

void onEnd(GMarkupParseContext *, const gchar *, gpointer user, GError **) {
  static_cast<ParseState *>(user)->depth--;
}

}  // namespace

bool LoadTuiSession(const std::string &path, TuiSession &out) {
  gchar *data = nullptr;
  gsize len = 0;
  if (!g_file_get_contents(path.c_str(), &data, &len, nullptr)) return false;

  ParseState st;
  GMarkupParser parser = {onStart, onEnd, nullptr, nullptr, nullptr};
  GMarkupParseContext *ctx =
      g_markup_parse_context_new(&parser, G_MARKUP_DEFAULT_FLAGS, &st, nullptr);
  bool ok = g_markup_parse_context_parse(ctx, data, (gssize)len, nullptr) &&
            g_markup_parse_context_end_parse(ctx, nullptr);
  g_markup_parse_context_free(ctx);
  g_free(data);
  if (!ok || st.bad || !st.sawRoot) return false;

  TuiSession s = std::move(st.session);
  if (s.current >= s.buffers.size()) s.current = 0;
  out = std::move(s);
  return true;
}

bool SaveTuiSession(const std::string &path, const TuiSession &session) {
  GString *xml = g_string_new("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  g_string_append_printf(xml, "<TuiSession version=\"1\" current=\"%lu\">\n",
                         (unsigned long)session.current);
  for (const SessionBuffer &b : session.buffers) {
    if (b.path.empty() && b.backup.empty()) continue;  // nothing restorable
    g_string_append(xml, "  <Buffer");
    if (!b.path.empty()) {
      gchar *esc = g_markup_escape_text(b.path.c_str(), -1);
      g_string_append_printf(xml, " path=\"%s\"", esc);
      g_free(esc);
    }
    if (!b.backup.empty()) {
      gchar *esc = g_markup_escape_text(b.backup.c_str(), -1);
      g_string_append_printf(xml, " backup=\"%s\"", esc);
      g_free(esc);
    }
    g_string_append_printf(xml, " caret=\"%ld\" firstVisible=\"%ld\"/>\n", b.caret,
                           b.firstVisible);
  }
  g_string_append(xml, "</TuiSession>\n");

  bool ok = g_file_set_contents(path.c_str(), xml->str, (gssize)xml->len, nullptr);
  g_string_free(xml, TRUE);
  return ok;
}

}  // namespace npptui

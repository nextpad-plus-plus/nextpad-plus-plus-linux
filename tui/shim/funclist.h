// Linux port of tui/shim/funclist.h — `NSString *text, NSString *lang`
// became `const std::string &`; `#import <Foundation/Foundation.h>` removed.
// Name unchanged so main.mm:679 transliterates 1:1. Body (phase 2): an
// adapter over src/funclist_xml.c `flxml_scan(text, len, lang, &items)`
// (npp_core; GLib-only, and GRegex IS PCRE, so funclist.mm's 40-line
// PCRE→ICU rewriter and its bail-outs vanish) flattening FlXmlItem trees
// into FuncItem rows — docs/12 §8.3, funclist row. Rules resolution is
// flxml's: $NPP_TUI_FUNCLIST_DIR (tests), <user dir>/functionList/<lang>.xml,
// then RESOURCES_DIR/functionList/<lang>.xml (exact case, then lowercased).
// The hardcoded-regex fallback lives in the GTK panel (funclist.c) and is
// a separate port if wanted.

// Function-list engine (Phase 4.5): the GUI's functionList XML parser
// engine — the same functionList/<lang>.xml rules, the same
// classRange/function/nameExpr semantics — feeding the TUI's dock instead
// of a tree view. Languages without an XML parser report "no parser"
// honestly (the GUI's XML path is its primary path too).

#ifndef NPP_TUI_SHIM_FUNCLIST_H
#define NPP_TUI_SHIM_FUNCLIST_H

#include <string>
#include <vector>

namespace npptui {

struct FuncItem {
  std::string name;
  long line = 1;   // 1-based
  int depth = 0;   // 0 = top level / class node, 1 = method inside a class
  bool isNode = false;  // class/container row
};

// Scans `text` with the functionList rules for `lang` (langs.xml internal
// name, matched lowercased like the GUI). Returns false when no usable
// parser XML exists for the language; true otherwise (even with 0 items).
bool ScanFunctions(const std::string &text, const std::string &lang, std::vector<FuncItem> &out);

}  // namespace npptui

#endif

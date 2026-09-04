// Adapted from Fresh (crates/fresh-editor/src/view/dimming.rs, v0.4.10),
// under the Nextpad++ provision in Fresh's LICENSE ("Permission explicitly
// granted to Nextpad++ to read and adapt the Fresh notepad code to work with
// Nextpad++"). Original copyright (c) Noam Lewis.
//
// Dim-behind-modal color math. DimColor returns false for the terminal
// default (Fresh's Reset) — the user's default background may be light or
// dark, so there is no honest RGB substitute; those cells get the terminal's
// own DIM attribute instead (Fresh issue #2982). The curses application of
// the pass lives with the curses code (main.mm): Scinterm repaints its
// window fully on every refresh, so attribute-dimming its cells is transient
// and safe.

#ifndef NPP_TUI_CHROME_DIMMING_H
#define NPP_TUI_CHROME_DIMMING_H

#include "cellsurface.h"

namespace npptui {

// The dimmed replacement for a color (~60% darker); false for the terminal
// default, which must be dimmed by the terminal (DIM attribute) instead.
bool DimColor(const Color &in, Color &out);

// Apply dimming to one cell style: fg/bg dim numerically when resolvable;
// an unresolvable fg sets the dim attribute so the terminal handles it.
void DimStyle(CellStyle &style);

}  // namespace npptui

#endif

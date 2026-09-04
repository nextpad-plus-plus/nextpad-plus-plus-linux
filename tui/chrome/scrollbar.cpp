// Adapted from Fresh (crates/fresh-editor/src/view/ui/scrollbar.rs, v0.4.10),
// under the Nextpad++ provision in Fresh's LICENSE. Original copyright
// (c) Noam Lewis. See scrollbar.h.

#include "scrollbar.h"

#include <cmath>
#include <cstdlib>

namespace npptui {

static size_t satSub(size_t a, size_t b) { return a > b ? a - b : 0; }

void ScrollbarState::thumbGeometry(size_t trackHeight, size_t &thumbStart,
                                   size_t &thumbSize) const {
  thumbStart = 0;
  thumbSize = 0;
  if (trackHeight == 0 || totalItems == 0) return;

  size_t maxScroll = satSub(totalItems, visibleItems);
  if (maxScroll == 0) {
    thumbSize = trackHeight;  // content fits: fill the whole track
    return;
  }

  size_t raw = (size_t)std::ceil((double)visibleItems / (double)totalItems *
                                 (double)trackHeight);
  size_t maxThumb = (size_t)std::floor((double)trackHeight * 0.8);
  size_t size = raw > 0 ? raw : 1;
  if (size > maxThumb) size = maxThumb;
  if (size > trackHeight) size = trackHeight;
  if (size == 0) size = 1;

  double ratio = (double)(scrollOffset < maxScroll ? scrollOffset : maxScroll) /
                 (double)maxScroll;
  size_t maxThumbStart = satSub(trackHeight, size);
  thumbStart = (size_t)(ratio * (double)maxThumbStart);
  thumbSize = size;
}

bool ScrollbarState::isThumbRow(size_t trackHeight, size_t row) const {
  size_t start = 0, size = 0;
  thumbGeometry(trackHeight, start, size);
  return row >= start && row < start + size;
}

size_t ScrollbarState::offsetForThumbTop(size_t trackHeight, size_t targetThumbTop) const {
  size_t maxScroll = satSub(totalItems, visibleItems);
  if (trackHeight == 0 || maxScroll == 0) return 0;
  size_t start = 0, thumbSize = 0;
  thumbGeometry(trackHeight, start, thumbSize);
  size_t maxThumbTop = satSub(trackHeight, thumbSize);
  if (maxThumbTop == 0) return 0;
  size_t clamped = targetThumbTop < maxThumbTop ? targetThumbTop : maxThumbTop;
  // Ceiling division: the smallest offset that reaches the row (thumbGeometry
  // floors); keep the offset below as a candidate for unreachable rows.
  size_t hi = (clamped * maxScroll + maxThumbTop - 1) / maxThumbTop;
  if (hi > maxScroll) hi = maxScroll;
  size_t lo = satSub(hi, 1);
  auto thumbTopOf = [&](size_t offset) {
    size_t s = 0, z = 0;
    ScrollbarState(totalItems, visibleItems, offset).thumbGeometry(trackHeight, s, z);
    return s;
  };
  size_t dLo = (size_t)std::llabs((long long)thumbTopOf(lo) - (long long)clamped);
  size_t dHi = (size_t)std::llabs((long long)thumbTopOf(hi) - (long long)clamped);
  return dLo < dHi ? lo : hi;
}

size_t ScrollbarState::dragToOffset(size_t trackHeight, size_t dragStartRow,
                                    size_t dragStartOffset, size_t currentRow) const {
  size_t maxScroll = satSub(totalItems, visibleItems);
  if (trackHeight == 0 || maxScroll == 0)
    return dragStartOffset < maxScroll ? dragStartOffset : maxScroll;

  long long deltaRows = (long long)currentRow - (long long)dragStartRow;
  if (deltaRows == 0) return dragStartOffset < maxScroll ? dragStartOffset : maxScroll;

  size_t start = 0, thumbSize = 0;
  ScrollbarState(totalItems, visibleItems, dragStartOffset)
      .thumbGeometry(trackHeight, start, thumbSize);
  size_t maxThumbTop = satSub(trackHeight, thumbSize);
  if (maxThumbTop == 0) return dragStartOffset < maxScroll ? dragStartOffset : maxScroll;

  double offsetDelta = (double)deltaRows * ((double)maxScroll / (double)maxThumbTop);
  double newOffset = std::round((double)dragStartOffset + offsetDelta);
  if (newOffset < 0) return 0;
  if (newOffset > (double)maxScroll) return maxScroll;
  return (size_t)newOffset;
}

bool ScrollbarMouse::press(const ScrollbarState &state, const Rect &track, int col,
                           int row, size_t &newOffset) {
  if (!PointInRect(track, col, row)) return false;
  size_t trackHeight = (size_t)track.height;
  size_t clickRow = (size_t)(row > track.y ? row - track.y : 0);
  if (clickRow > trackHeight) clickRow = trackHeight;

  if (state.isThumbRow(trackHeight, clickRow)) {
    newOffset = state.scrollOffset;
  } else {
    size_t start = 0, thumbSize = 0;
    state.thumbGeometry(trackHeight, start, thumbSize);
    size_t aimTop = satSub(clickRow, thumbSize / 2);
    newOffset = state.offsetForThumbTop(trackHeight, aimTop);
  }
  dragging = true;
  dragState = ScrollbarDrag{clickRow, newOffset};
  return true;
}

bool ScrollbarMouse::drag(const ScrollbarState &state, const Rect &track, int row,
                          size_t &newOffset) {
  if (!dragging) return false;
  size_t trackHeight = (size_t)track.height;
  size_t currentRow = (size_t)(row > track.y ? row - track.y : 0);
  if (currentRow > trackHeight) currentRow = trackHeight;
  newOffset = state.dragToOffset(trackHeight, dragState.startRow, dragState.startOffset,
                                 currentRow);
  return true;
}

void RenderScrollbar(CellSurface &out, const Rect &area, const ScrollbarState &state,
                     const ScrollbarColors &colors) {
  if (area.height <= 0 || area.width <= 0) return;
  size_t start = 0, size = 0;
  state.thumbGeometry((size_t)area.height, start, size);
  for (int row = 0; row < area.height; row++) {
    CellStyle st;
    st.bg = ((size_t)row >= start && (size_t)row < start + size) ? colors.thumb
                                                                 : colors.track;
    out.put(area.x, area.y + row, " ", st);
  }
}

}  // namespace npptui

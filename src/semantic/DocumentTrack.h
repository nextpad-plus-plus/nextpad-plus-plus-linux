/*
 * DocumentTrack
 *
 * Semantic search follows one Scintilla document, identified by
 * SCI_GETDOCPOINTER, even when two views show it. Bookmarks this
 * feature adds are marker handles, so an edit that moves lines does
 * not make a later delete hit the wrong line.
 */
#ifndef NPP_DOCUMENT_TRACK_H
#define NPP_DOCUMENT_TRACK_H

#include <cstdint>
#include <functional>
#include <set>
#include <map>
#include <vector>

struct DocView {
    std::uintptr_t view = 0;
    std::uintptr_t doc = 0;
};

enum class DocAttach {
    SameView,
    SameDocument,
    SwitchDocument
};

enum class DocClose {
    Unrelated,
    Keep,
    Retarget,
    ClearLast
};

struct DocCloseResult {
    DocClose action = DocClose::Unrelated;
    std::uintptr_t view = 0;
};

class DocumentTrack {
public:
    std::uintptr_t view() const { return view_; }
    std::uintptr_t doc() const { return doc_; }
    bool active() const { return doc_ != 0; }

    const std::vector<int> &handles() const { return handles_; }
    std::vector<int> &handles() { return handles_; }

    DocAttach classify_attach(std::uintptr_t view, std::uintptr_t doc) const;
    void retarget(std::uintptr_t view);
    void switch_to(std::uintptr_t view, std::uintptr_t doc);

    /* True when `doc` is the document currently being searched. */
    bool accepts_modification(std::uintptr_t doc) const;

    /* `open` includes the view that is closing. Closing the other clone
     * keeps the bookmarks. Closing the tracked view retargets to a
     * remaining clone, or clears when it was the last view. */
    DocCloseResult classify_close(std::uintptr_t closed_view,
                                  std::uintptr_t closed_doc,
                                  const std::vector<DocView> &open) const;
    void apply_close(const DocCloseResult &result);

private:
    std::uintptr_t view_ = 0;
    std::uintptr_t doc_ = 0;
    std::vector<int> handles_;
};

struct MarkerSyncState {
    std::map<int, int> last_lines;
    std::set<int> dismissed_lines;
};

struct MarkerFns {
    std::function<int(int handle)> line_from_handle;
    std::function<void(int handle)> delete_handle;
    std::function<unsigned(int line)> mask_on_line;
    std::function<int(int line)> add_on_line;
};

/* Keep `handles` aligned with `want_lines`. A handle whose line moved
 * stays put when that line is still wanted. A handle whose line was
 * deleted may move to its neighbour in Scintilla. Delete only owned handles.
 * Remember explicitly removed markers until the selection is cleared. */
void sync_marker_handles(std::vector<int> &handles,
                         const std::set<int> &want_lines,
                         unsigned bookmark_bit,
                         const MarkerFns &fns, MarkerSyncState *state = nullptr);

#endif

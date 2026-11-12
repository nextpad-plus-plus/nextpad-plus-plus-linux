#include "DocumentTrack.h"

DocAttach DocumentTrack::classify_attach(std::uintptr_t view, std::uintptr_t doc) const
{
    if (view == view_ && doc == doc_) return DocAttach::SameView;
    if (doc != 0 && doc == doc_) return DocAttach::SameDocument;
    return DocAttach::SwitchDocument;
}

void DocumentTrack::retarget(std::uintptr_t view)
{
    view_ = view;
}

void DocumentTrack::switch_to(std::uintptr_t view, std::uintptr_t doc)
{
    view_ = view;
    doc_ = doc;
    handles_.clear();
}

bool DocumentTrack::accepts_modification(std::uintptr_t doc) const
{
    return doc_ != 0 && doc == doc_;
}

DocCloseResult DocumentTrack::classify_close(std::uintptr_t closed_view,
                                            std::uintptr_t closed_doc,
                                            const std::vector<DocView> &open) const
{
    DocCloseResult result;
    if (!active()) return result;
    bool same_view = closed_view != 0 && closed_view == view_;
    bool same_doc = closed_doc != 0 && closed_doc == doc_;
    if (!same_view && !same_doc) return result;

    std::uintptr_t replacement = 0;
    for (const DocView &candidate : open) {
        if (candidate.view == 0 || candidate.view == closed_view) continue;
        if (candidate.doc != doc_) continue;
        replacement = candidate.view;
        break;
    }
    if (!same_view) {
        result.action = DocClose::Keep;
        return result;
    }
    if (replacement != 0) {
        result.action = DocClose::Retarget;
        result.view = replacement;
        return result;
    }
    result.action = DocClose::ClearLast;
    return result;
}

void DocumentTrack::apply_close(const DocCloseResult &result)
{
    if (result.action == DocClose::Retarget) {
        view_ = result.view;
        return;
    }
    if (result.action == DocClose::ClearLast) {
        view_ = 0;
        doc_ = 0;
        handles_.clear();
    }
}

void sync_marker_handles(std::vector<int> &handles,
                         const std::set<int> &want_lines,
                         unsigned bookmark_bit,
                         const MarkerFns &fns, MarkerSyncState *state)
{
    if (state && want_lines.empty()) *state = {};
    std::map<int, int> next_lines;
    std::vector<int> keep;
    std::set<int> have;
    keep.reserve(handles.size());
    for (int handle : handles) {
        int line = fns.line_from_handle ? fns.line_from_handle(handle) : -1;
        if (state && line < 0 && !want_lines.empty()) {
            auto old = state->last_lines.find(handle);
            if (old != state->last_lines.end()) state->dismissed_lines.insert(old->second);
        }
        if (line >= 0 && want_lines.count(line)) {
            next_lines[handle] = line;
            keep.push_back(handle);
            have.insert(line);
        } else if (line >= 0 && fns.delete_handle) {
            fns.delete_handle(handle);
        }
    }
    if (fns.add_on_line) {
        for (int line : want_lines) {
            if (have.count(line) || (state && state->dismissed_lines.count(line))) continue;
            unsigned mask = fns.mask_on_line ? fns.mask_on_line(line) : 0u;
            if (mask & bookmark_bit) continue;
            int handle = fns.add_on_line(line);
            if (handle < 0) continue;
            next_lines[handle] = line;
            keep.push_back(handle);
            have.insert(line);
        }
    }
    handles.swap(keep);
    if (state) state->last_lines = std::move(next_lines);
}

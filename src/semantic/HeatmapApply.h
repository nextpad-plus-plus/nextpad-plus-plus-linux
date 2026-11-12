/*
 * HeatmapApply
 *
 * Decides whether a batch of sentence scores may change the colours on
 * screen. The check runs on the UI thread, using the build and query
 * generations captured when the batch was scored and the generations
 * at the moment of painting.
 *
 * Skip leaves the screen alone. That is the right answer for a result
 * that belongs to an older document or an older query: the newer edit
 * or query has already cleared or replaced the colours.
 * Clear removes this feature's colours and bookmarks.
 * Paint applies the scores in the result.
 */
#ifndef NPP_HEATMAP_APPLY_H
#define NPP_HEATMAP_APPLY_H

#include <cstdint>

enum class HeatmapApply {
    Skip,
    Clear,
    Paint
};

inline HeatmapApply heatmap_apply(bool dead,
                                 uint64_t result_build, uint64_t build_now,
                                 uint64_t result_query, uint64_t query_now,
                                 bool query_empty, bool hits_empty)
{
    if (dead) return HeatmapApply::Skip;
    if (result_build != build_now || result_query != query_now) return HeatmapApply::Skip;
    if (query_empty || hits_empty) return HeatmapApply::Clear;
    return HeatmapApply::Paint;
}

#endif

/*
 * EmbedFallback
 *
 * Runs embedding batches. If a batch fails while a GPU provider is in
 * use, the caller falls back to CPU, discards every vector from this
 * call, and the same batches run again from the start. A second
 * failure is returned as-is so GPU and CPU vectors are not mixed.
 */
#ifndef NPP_EMBED_FALLBACK_H
#define NPP_EMBED_FALLBACK_H

#include "SemanticInterfaces.h"

#include <functional>
#include <string>

/* `run_batch` fills `original_indexes` for a successful slice.
 * `fallback` switches the session to CPU and drops partial output.
 * `progress` receives an empty list when earlier batches of this call
 * must be discarded. `using_gpu` is cleared after a successful fallback. */
bool run_embed_batches(
    size_t count,
    size_t batch_size,
    bool &using_gpu,
    const std::function<bool(size_t base, size_t n,
                             std::vector<size_t> &original_indexes,
                             std::string &error)> &run_batch,
    const std::function<bool(std::string &error)> &fallback,
    const SemanticBatchProgress &progress,
    std::string &error);

#endif

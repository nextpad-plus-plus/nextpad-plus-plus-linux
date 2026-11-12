#include "EmbedFallback.h"

#include <algorithm>

bool run_embed_batches(
    size_t count,
    size_t batch_size,
    bool &using_gpu,
    const std::function<bool(size_t base, size_t n,
                             std::vector<size_t> &original_indexes,
                             std::string &error)> &run_batch,
    const std::function<bool(std::string &error)> &fallback,
    const SemanticBatchProgress &progress,
    std::string &error)
{
    if (batch_size == 0) {
        error = "empty batch size";
        return false;
    }
    bool restarted = false;
    for (size_t base = 0; base < count;) {
        size_t n = std::min(batch_size, count - base);
        std::vector<size_t> indexes;
        if (!run_batch(base, n, indexes, error)) {
            if (!using_gpu || restarted || !fallback) return false;
            if (!fallback(error)) return false;
            using_gpu = false;
            restarted = true;
            if (progress && !progress({})) {
                error = "cancelled";
                return false;
            }
            base = 0;
            continue;
        }
        if (progress) {
            if (!progress(indexes)) {
                error = "cancelled";
                return false;
            }
        }
        base += n;
    }
    return true;
}

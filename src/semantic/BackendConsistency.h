#ifndef NPP_BACKEND_CONSISTENCY_H
#define NPP_BACKEND_CONSISTENCY_H

#include "SemanticInterfaces.h"
#include <unordered_map>

// Called after embedding, including failure: a failed CPU restart still
// invalidates vectors produced by the GPU session it replaced.
inline bool invalidate_changed_backend(const EmbeddingProvider &provider,
                                       uint64_t before, VectorIndex &index,
                                       std::unordered_map<std::string, std::vector<float>> &cache)
{
    if (provider.backend_generation() == before) return false;
    cache.clear();
    index.reset(static_cast<size_t>(provider.dimension()));
    return true;
}
#endif

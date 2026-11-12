#include "SemanticInterfaces.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

class CpuSimilarityEngine final : public SimilarityEngine {
public:
    std::string engine_name() const override { return "CPU dot product"; }

    bool scores(const float *query, const float *vectors, size_t count,
                size_t dimension, float *out_scores) const override
    {
        if (!query || (!vectors && count) || !out_scores) return false;
        for (size_t row = 0; row < count; ++row) {
            const float *v = vectors + row * dimension;
            double dot = 0.0;
            for (size_t i = 0; i < dimension; ++i)
                dot += static_cast<double>(query[i]) * static_cast<double>(v[i]);
            out_scores[row] = static_cast<float>(dot);
        }
        return true;
    }
};

class MemoryVectorIndex final : public VectorIndex {
public:
    explicit MemoryVectorIndex(SimilarityEngine &engine) : engine_(engine) {}

    size_t dimension() const override { return dim_; }
    size_t count() const override { return ids_.size(); }

    void reset(size_t dimension) override
    {
        dim_ = dimension;
        data_.clear();
        ids_.clear();
    }

    bool add(const float *vector, int64_t sentence_id) override
    {
        if (!vector || dim_ == 0) return false;
        data_.insert(data_.end(), vector, vector + dim_);
        ids_.push_back(sentence_id);
        return true;
    }

    size_t score_all(const float *query, SemanticHit *hits, size_t capacity) override
    {
        if (ids_.empty()) return 0;
        if (!query || !hits || capacity == 0) return 0;
        std::vector<float> sc(ids_.size());
        if (!engine_.scores(query, data_.data(), ids_.size(), dim_, sc.data()))
            return 0;
        size_t n = std::min(capacity, ids_.size());
        for (size_t i = 0; i < n; ++i) {
            hits[i].sentence_id = ids_[i];
            hits[i].score = sc[i];
        }
        return n;
    }

    size_t search(const float *query, size_t k, SemanticHit *hits) override
    {
        if (k == 0 || ids_.empty() || !hits) return 0;
        std::vector<SemanticHit> all(ids_.size());
        size_t n = score_all(query, all.data(), all.size());
        if (n == 0 && !ids_.empty()) return 0;
        std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(std::min(k, n)),
                          all.begin() + static_cast<std::ptrdiff_t>(n),
                          [](const SemanticHit &a, const SemanticHit &b) {
                              return a.score > b.score;
                          });
        size_t keep = std::min(k, n);
        for (size_t i = 0; i < keep; ++i) hits[i] = all[i];
        return keep;
    }

private:
    SimilarityEngine &engine_;
    size_t dim_ = 0;
    std::vector<float> data_;
    std::vector<int64_t> ids_;
};

} /* namespace */

std::unique_ptr<SimilarityEngine> make_cpu_similarity_engine()
{
    return std::make_unique<CpuSimilarityEngine>();
}

std::unique_ptr<VectorIndex> make_memory_vector_index(SimilarityEngine &engine)
{
    return std::make_unique<MemoryVectorIndex>(engine);
}

/*
 * SemanticInterfaces.h
 *
 * Three swappable pieces, matching the macOS semantic search split:
 * an embedding provider, a similarity engine, and a vector index.
 * Callers depend on these types only, so a GPU embedding backend or an
 * approximate index can replace the CPU implementations without touching
 * the heatmap controller.
 *
 * Vectors are L2-normalized. Dot product is cosine similarity.
 */
#ifndef NPP_SEMANTIC_INTERFACES_H
#define NPP_SEMANTIC_INTERFACES_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct SemanticHit {
    int64_t sentence_id;
    float   score;
};

/* Called after each length-sorted batch. `original_indexes` are positions
 * in the text list passed to embed_batch. An empty list means the GPU
 * backend failed and every vector from this embed_batch call must be
 * discarded; later callbacks are the CPU retry. Return false to cancel. */
using SemanticBatchProgress = std::function<bool(const std::vector<size_t> &original_indexes)>;

class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;

    virtual int dimension() const = 0;
    virtual int max_tokens() const = 0;
    virtual bool available() const = 0;

    /* Status text, for example "ONNX Runtime CPU". */
    virtual std::string backend_name() const = 0;

    /* Changes whenever vectors from an earlier backend become invalid. */
    virtual uint64_t backend_generation() const { return 0; }

    /* Token count including the special tokens the model expects. */
    virtual size_t token_count(const std::string &text) const = 0;

    /* Write one L2-normalized vector per text into `out` (row-major).
     * Batches are sorted by token length. `progress` may be empty. */
    virtual bool embed_batch(const std::vector<std::string> &texts,
                             std::vector<float> &out,
                             std::string &error,
                             const SemanticBatchProgress &progress) = 0;
};

class SimilarityEngine {
public:
    virtual ~SimilarityEngine() = default;
    virtual std::string engine_name() const = 0;

    /* `vectors` is row-major, count x dimension. Writes `count` scores. */
    virtual bool scores(const float *query,
                        const float *vectors,
                        size_t count,
                        size_t dimension,
                        float *out_scores) const = 0;
};

class VectorIndex {
public:
    virtual ~VectorIndex() = default;
    virtual size_t dimension() const = 0;
    virtual size_t count() const = 0;
    virtual void reset(size_t dimension) = 0;
    virtual bool add(const float *vector, int64_t sentence_id) = 0;

    /* Score every stored vector. Writes min(count, capacity) hits.
     * Order is unspecified. Returns 0 on failure. */
    virtual size_t score_all(const float *query, SemanticHit *hits, size_t capacity) = 0;

    /* Top-k, highest score first. */
    virtual size_t search(const float *query, size_t k, SemanticHit *hits) = 0;
};

std::unique_ptr<SimilarityEngine> make_cpu_similarity_engine();
std::unique_ptr<VectorIndex> make_memory_vector_index(SimilarityEngine &engine);

#endif

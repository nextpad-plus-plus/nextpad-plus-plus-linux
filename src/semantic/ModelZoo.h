/*
 * ModelZoo.h
 *
 * Manifest-driven registry of ONNX sentence-embedding models.
 * Each entry names dimension, pooling, max tokens, and the vocab plus
 * model files (URL and SHA-256). The model file may differ by CPU
 * architecture. Downloads land in a caller-chosen directory and are
 * kept only when the checksum matches.
 */
#ifndef NPP_MODEL_ZOO_H
#define NPP_MODEL_ZOO_H

#include "SemanticHeatmapColors.h"
#include "CancellableSpawn.h"
#include <optional>
#include <functional>

#include <string>
#include <vector>

struct ModelFile {
    std::string filename;
    std::string url;
    std::string sha256;
};

struct ModelSpec {
    std::string id;
    std::string name;
    int dimension = 0;
    std::string pooling; /* "mean" */
    int max_tokens = 256;
    SemanticHeatmap::Profile heatmap_profile = SemanticHeatmap::Profile::MiniLM;
    ModelFile vocab;
    ModelFile model;
};

class ModelZoo {
public:
    bool load_file(const std::string &path, std::string &error);
    const std::string &default_id() const { return default_id_; }
    const std::vector<ModelSpec> &models() const { return models_; }
    const ModelSpec *find(const std::string &id) const;

    /* Create dest_dir and make sure spec.model and spec.vocab are present
     * there under their filenames, with matching SHA-256. A file that is
     * already correct is not downloaded again. When spawn is set, an
     * in-flight download can be cancelled or the spawner can be retired
     * so shutdown does not wait for curl's full timeout. Queued jobs pass
     * a ticket captured before enqueueing; both downloads use that ticket.
     * on_download fires only when a transfer is needed, before starting curl. */
    bool ensure(const ModelSpec &spec, const std::string &dest_dir, std::string &error,
                CancellableSpawn *spawn = nullptr,
                std::optional<CancellableSpawn::Ticket> ticket = std::nullopt,
                const std::function<void()> &on_download = {}) const;

    static std::string host_arch();
    static bool file_sha256(const std::string &path, std::string &hex, std::string &error);

    /* Concise download failure text, classified by curl exit code. */
    static std::string download_failure_message(const std::string &model_id, bool curl_missing, int exit_code = -1);

private:
    std::string default_id_;
    std::vector<ModelSpec> models_;
};

#endif

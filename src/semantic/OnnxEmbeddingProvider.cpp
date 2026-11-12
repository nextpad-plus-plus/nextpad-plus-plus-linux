#include "ExecutionProviders.h"
#include "EmbedFallback.h"
#include "OrtRuntime.h"
#include "WordPieceTokenizer.h"

#include <glib.h>

#if NPP_HAVE_ONNXRUNTIME
#include <onnxruntime_c_api.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace {

constexpr int kIntraOpThreads = 2;
constexpr size_t kBatchSize = 8;

std::string lower_copy(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

struct KnownEp {
    const char *id;
    const char *ort_name;
};

constexpr KnownEp kKnown[] = {
    {"cpu", "CPUExecutionProvider"},
    {"cuda", "CUDAExecutionProvider"},
    {"openvino", "OpenVINOExecutionProvider"},
};

const KnownEp *known_by_id(const std::string &id)
{
    std::string key = lower_copy(id);
    for (const KnownEp &ep : kKnown)
        if (key == ep.id) return &ep;
    return nullptr;
}

struct TokenItem {
    size_t orig;
    std::vector<int64_t> ids;
};

#if NPP_HAVE_ONNXRUNTIME

class OrtSessionOwner {
public:
    const OrtApi *api = nullptr;
    OrtEnv *env = nullptr;
    OrtSessionOptions *opts = nullptr;
    OrtSession *session = nullptr;
    OrtMemoryInfo *memory = nullptr;
    std::vector<std::string> input_names;
    std::string output_name;

    ~OrtSessionOwner()
    {
        if (!api) return;
        if (session) api->ReleaseSession(session);
        if (opts) api->ReleaseSessionOptions(opts);
        if (memory) api->ReleaseMemoryInfo(memory);
        if (env) api->ReleaseEnv(env);
    }

    bool check(OrtStatus *st, std::string &error)
    {
        if (!st) return true;
        error = api->GetErrorMessage(st);
        api->ReleaseStatus(st);
        return false;
    }
};

std::vector<std::string> available_ort_names(const OrtApi *api)
{
    std::vector<std::string> names;
    char **out = nullptr;
    int n = 0;
    OrtStatus *st = api->GetAvailableProviders(&out, &n);
    if (st) {
        api->ReleaseStatus(st);
        return names;
    }
    for (int i = 0; i < n; ++i)
        if (out[i]) names.emplace_back(out[i]);
    st = api->ReleaseAvailableProviders(out, n);
    if (st) api->ReleaseStatus(st);
    return names;
}

bool name_present(const std::vector<std::string> &names, const char *want)
{
    for (const std::string &n : names)
        if (g_ascii_strcasecmp(n.c_str(), want) == 0) return true;
    return false;
}

class OnnxEmbeddingProvider final : public EmbeddingProvider {
public:
    int dimension() const override { return dim_; }
    int max_tokens() const override { return max_tokens_; }
    bool available() const override { return ready_; }
    std::string backend_name() const override { return backend_; }
    uint64_t backend_generation() const override { return backend_generation_; }

    size_t token_count(const std::string &text) const override
    {
        std::vector<int32_t> ids;
        if (!tok_.encode(text, max_tokens_, ids)) return 0;
        return ids.size();
    }

    bool embed_batch(const std::vector<std::string> &texts,
                     std::vector<float> &out,
                     std::string &error,
                     const SemanticBatchProgress &progress) override;

    bool open(const OnnxProviderConfig &config, std::string &error);

private:
    bool collect_io(std::string &error);
    bool fallback_cpu(std::string &error);
    bool make_options(bool try_gpu, const KnownEp *want, std::string &error);
    bool run_items(const std::vector<TokenItem> &items, size_t base, size_t n,
                   std::vector<float> &out, std::string &error);

    WordPieceTokenizer tok_;
    OrtSessionOwner ort_;
    int dim_ = 0;
    int max_tokens_ = 256;
    bool ready_ = false;
    std::string backend_;
    std::string model_path_;
    bool using_gpu_ = false;
    uint64_t backend_generation_ = 0;
};

bool OnnxEmbeddingProvider::make_options(bool try_gpu, const KnownEp *want, std::string &error)
{
    const OrtApi *api = ort_.api;
    if (ort_.opts) {
        api->ReleaseSessionOptions(ort_.opts);
        ort_.opts = nullptr;
    }
    if (!ort_.check(api->CreateSessionOptions(&ort_.opts), error)) return false;
    if (!ort_.check(api->SetIntraOpNumThreads(ort_.opts, kIntraOpThreads), error)) return false;
    if (!ort_.check(api->SetInterOpNumThreads(ort_.opts, 1), error)) return false;
    if (!ort_.check(api->SetSessionExecutionMode(ort_.opts, ORT_SEQUENTIAL), error)) return false;
    if (!ort_.check(api->SetSessionGraphOptimizationLevel(ort_.opts,
            try_gpu && want && std::strcmp(want->id, "openvino") == 0
                ? ORT_DISABLE_ALL : ORT_ENABLE_ALL), error))
        return false;
    /* Drop spinning waits so the editor thread keeps getting CPU time. */
    OrtStatus *spin = api->AddSessionConfigEntry(ort_.opts, "session.intra_op.allow_spinning", "0");
    if (spin) api->ReleaseStatus(spin);
    spin = api->AddSessionConfigEntry(ort_.opts, "session.inter_op.allow_spinning", "0");
    if (spin) api->ReleaseStatus(spin);
    if (try_gpu && want) {
        if (std::strcmp(want->id, "cuda") == 0) {
            OrtCUDAProviderOptionsV2 *options = nullptr;
            if (!ort_.check(api->CreateCUDAProviderOptions(&options), error)) return false;
            // Keep the same fp32 scoring/colour behaviour as CPU and Intel.
            const char *keys[] = {"device_id", "use_tf32"};
            const char *values[] = {"0", "0"};
            bool ok = ort_.check(api->UpdateCUDAProviderOptions(options, keys, values, 2), error) &&
                      ort_.check(api->SessionOptionsAppendExecutionProvider_CUDA_V2(ort_.opts, options), error);
            api->ReleaseCUDAProviderOptions(options);
            return ok;
        }
        const char *keys[] = {"device_type", "precision"};
        const char *vals[] = {"GPU", "FP32"};
        return ort_.check(api->SessionOptionsAppendExecutionProvider(
            ort_.opts, "OpenVINO", keys, vals, 2), error);
    }
    return true;
}

bool OnnxEmbeddingProvider::open(const OnnxProviderConfig &config, std::string &error)
{
    model_path_ = config.model_path;
    dim_ = config.dimension;
    max_tokens_ = config.max_tokens;
    if (config.pooling != "mean") {
        error = "only mean pooling is implemented";
        return false;
    }
    ort_.api = npp_ort_api(error);
    if (!ort_.api) {
        if (error.empty()) error = npp_ort_missing_message();
        return false;
    }
    if (!tok_.load(config.vocab_path, error)) return false;
    const OrtApi *api = ort_.api;
    if (!ort_.check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "nextpad-semantic", &ort_.env), error))
        return false;

    std::vector<std::string> present = available_ort_names(api);
    std::string requested = config.execution_provider.empty() ? "auto"
                                                              : lower_copy(config.execution_provider);
    const bool automatic = requested == "auto";
    const KnownEp *want = automatic ? nullptr : known_by_id(requested);
    if (!automatic && !want) {
        error = "unknown execution provider " + requested;
        return false;
    }
    bool used_gpu = false;
    for (const KnownEp &candidate : kKnown) {
        if (std::strcmp(candidate.id, "cpu") == 0) continue;
        if (!automatic && want != &candidate) continue;
        if (!name_present(present, candidate.ort_name)) continue;
        if (make_options(true, &candidate, error) &&
            ort_.check(api->CreateSession(ort_.env, model_path_.c_str(), ort_.opts, &ort_.session), error)) {
            want = &candidate;
            used_gpu = true;
            break;
        }
        if (ort_.session) api->ReleaseSession(ort_.session);
        ort_.session = nullptr;
        error.clear();
    }
    if (!used_gpu) {
        if (!make_options(false, nullptr, error)) return false;
        if (!ort_.check(api->CreateSession(ort_.env, model_path_.c_str(), ort_.opts, &ort_.session), error))
            return false;
    }
    if (!ort_.check(api->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &ort_.memory), error))
        return false;
    if (!collect_io(error)) return false;

    using_gpu_ = used_gpu;
    backend_ = onnx_backend_label(used_gpu, requested, used_gpu && want ? want->id : nullptr);
    ready_ = true;
    // Session creation alone does not prove that dynamic GPU inference works.
    if (using_gpu_) {
        std::vector<float> probe;
        if (!embed_batch({"Semantic search.", "A longer sentence to test padding."}, probe, error, {}))
            return false;
    }
    return true;
}

bool OnnxEmbeddingProvider::fallback_cpu(std::string &error)
{
    using_gpu_ = false;
    ++backend_generation_;
    if (ort_.session) ort_.api->ReleaseSession(ort_.session);
    ort_.session = nullptr;
    ort_.input_names.clear();
    ort_.output_name.clear();
    error.clear();
    if (!make_options(false, nullptr, error) ||
        !ort_.check(ort_.api->CreateSession(ort_.env, model_path_.c_str(), ort_.opts, &ort_.session), error) ||
        !collect_io(error)) {
        ready_ = false;
        return false;
    }
    backend_ = onnx_cpu_fallback_label();
    return true;
}

bool OnnxEmbeddingProvider::collect_io(std::string &error)
{
    const OrtApi *api = ort_.api;
    OrtAllocator *alloc = nullptr;
    if (!ort_.check(api->GetAllocatorWithDefaultOptions(&alloc), error)) return false;
    size_t n_in = 0;
    if (!ort_.check(api->SessionGetInputCount(ort_.session, &n_in), error)) return false;
    for (size_t i = 0; i < n_in; ++i) {
        char *name = nullptr;
        if (!ort_.check(api->SessionGetInputName(ort_.session, i, alloc, &name), error)) return false;
        ort_.input_names.emplace_back(name);
        alloc->Free(alloc, name);
    }
    size_t n_out = 0;
    if (!ort_.check(api->SessionGetOutputCount(ort_.session, &n_out), error)) return false;
    if (n_out == 0) {
        error = "model has no outputs";
        return false;
    }
    std::vector<std::string> outs;
    for (size_t i = 0; i < n_out; ++i) {
        char *name = nullptr;
        if (!ort_.check(api->SessionGetOutputName(ort_.session, i, alloc, &name), error)) return false;
        outs.emplace_back(name);
        alloc->Free(alloc, name);
    }
    ort_.output_name = outs[0];
    for (const std::string &n : outs)
        if (n == "last_hidden_state") ort_.output_name = n;
    return true;
}

bool OnnxEmbeddingProvider::embed_batch(const std::vector<std::string> &texts,
                                       std::vector<float> &out,
                                       std::string &error,
                                       const SemanticBatchProgress &progress)
{
    if (!ready_) {
        error = "embedding model is not loaded";
        return false;
    }
    std::vector<TokenItem> items;
    items.reserve(texts.size());
    for (size_t i = 0; i < texts.size(); ++i) {
        std::vector<int32_t> ids32;
        if (!tok_.encode(texts[i], max_tokens_, ids32)) {
            error = "tokenizer failed";
            return false;
        }
        TokenItem item;
        item.orig = i;
        item.ids.assign(ids32.begin(), ids32.end());
        items.push_back(std::move(item));
    }
    /* Shorter sequences first, then groups of kBatchSize. Each group is
     * padded to its longest member. The int8 MiniLM graph is sensitive to
     * that padding (a single unpadded run will not match bitwise). fp32 is
     * not. Callers that need a stable vector should pass one text. */
    std::sort(items.begin(), items.end(),
              [](const TokenItem &a, const TokenItem &b) { return a.ids.size() < b.ids.size(); });
    out.assign(texts.size() * static_cast<size_t>(dim_), 0.f);
    return run_embed_batches(
        items.size(), kBatchSize, using_gpu_,
        [&](size_t base, size_t n, std::vector<size_t> &indexes, std::string &err) {
            if (!run_items(items, base, n, out, err)) return false;
            indexes.clear();
            indexes.reserve(n);
            for (size_t k = 0; k < n; ++k) indexes.push_back(items[base + k].orig);
            return true;
        },
        [&](std::string &err) {
            /* Drop vectors already written by earlier GPU batches. */
            out.assign(texts.size() * static_cast<size_t>(dim_), 0.f);
            return fallback_cpu(err);
        },
        progress, error);
}

bool OnnxEmbeddingProvider::run_items(const std::vector<TokenItem> &items, size_t base, size_t n,
                                     std::vector<float> &out, std::string &error)
{
    size_t seq = 0;
    for (size_t k = 0; k < n; ++k) seq = std::max(seq, items[base + k].ids.size());
    if (seq == 0) {
        error = "empty token batch";
        return false;
    }
    std::vector<int64_t> input_ids(n * seq, tok_.pad_id());
    std::vector<int64_t> attn(n * seq, 0);
    std::vector<int64_t> types(n * seq, 0);
    for (size_t k = 0; k < n; ++k) {
        const auto &ids = items[base + k].ids;
        for (size_t t = 0; t < ids.size(); ++t) {
            input_ids[k * seq + t] = ids[t];
            attn[k * seq + t] = 1;
        }
    }

    const OrtApi *api = ort_.api;
    int64_t shape[2] = {static_cast<int64_t>(n), static_cast<int64_t>(seq)};
    auto make_tensor = [&](std::vector<int64_t> &data, OrtValue **value) {
        return ort_.check(api->CreateTensorWithDataAsOrtValue(
                              ort_.memory, data.data(), data.size() * sizeof(int64_t),
                              shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, value),
                          error);
    };

    bool need_ids = false, need_mask = false, need_types = false;
    for (const std::string &name : ort_.input_names) {
        if (name == "input_ids") need_ids = true;
        else if (name == "attention_mask") need_mask = true;
        else if (name == "token_type_ids") need_types = true;
        else {
            error = "unsupported model input " + name;
            return false;
        }
    }
    if (!need_ids) {
        error = "model has no input_ids";
        return false;
    }

    OrtValue *id_val = nullptr, *mask_val = nullptr, *type_val = nullptr;
    if (!make_tensor(input_ids, &id_val)) return false;
    if (need_mask && !make_tensor(attn, &mask_val)) {
        api->ReleaseValue(id_val);
        return false;
    }
    if (need_types && !make_tensor(types, &type_val)) {
        api->ReleaseValue(id_val);
        if (mask_val) api->ReleaseValue(mask_val);
        return false;
    }

    std::vector<const char *> in_names;
    std::vector<OrtValue *> in_vals;
    for (const std::string &name : ort_.input_names) {
        in_names.push_back(name.c_str());
        if (name == "input_ids") in_vals.push_back(id_val);
        else if (name == "attention_mask") in_vals.push_back(mask_val);
        else in_vals.push_back(type_val);
    }
    const char *out_name = ort_.output_name.c_str();
    OrtValue *result = nullptr;
    bool ran = ort_.check(api->Run(ort_.session, nullptr, in_names.data(), in_vals.data(),
                                   in_names.size(), &out_name, 1, &result),
                          error);
    api->ReleaseValue(id_val);
    if (mask_val) api->ReleaseValue(mask_val);
    if (type_val) api->ReleaseValue(type_val);
    if (!ran) return false;

    float *hidden = nullptr;
    if (!ort_.check(api->GetTensorMutableData(result, reinterpret_cast<void **>(&hidden)), error)) {
        api->ReleaseValue(result);
        return false;
    }
    OrtTensorTypeAndShapeInfo *info = nullptr;
    if (!ort_.check(api->GetTensorTypeAndShape(result, &info), error)) {
        api->ReleaseValue(result);
        return false;
    }
    size_t nd = 0;
    if (!ort_.check(api->GetDimensionsCount(info, &nd), error)) {
        api->ReleaseTensorTypeAndShapeInfo(info);
        api->ReleaseValue(result);
        return false;
    }
    std::vector<int64_t> dims(nd);
    if (nd && !ort_.check(api->GetDimensions(info, dims.data(), nd), error)) {
        api->ReleaseTensorTypeAndShapeInfo(info);
        api->ReleaseValue(result);
        return false;
    }
    ONNXTensorElementDataType elem = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    if (!ort_.check(api->GetTensorElementType(info, &elem), error)) {
        api->ReleaseTensorTypeAndShapeInfo(info);
        api->ReleaseValue(result);
        return false;
    }
    api->ReleaseTensorTypeAndShapeInfo(info);
    if (elem != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        api->ReleaseValue(result);
        error = "model output is not float32";
        return false;
    }

    auto normalize_into = [&](size_t orig, const float *src_or_null, const std::vector<double> *mean) {
        float *dst = out.data() + orig * static_cast<size_t>(dim_);
        double ss = 0.0;
        for (int i = 0; i < dim_; ++i) {
            float v = mean ? static_cast<float>((*mean)[static_cast<size_t>(i)]) : src_or_null[i];
            dst[i] = v;
            ss += static_cast<double>(v) * v;
        }
        float inv = ss > 0.0 ? static_cast<float>(1.0 / std::sqrt(ss)) : 0.f;
        for (int i = 0; i < dim_; ++i) dst[i] *= inv;
    };

    if (nd == 3) {
        int64_t b = dims[0], s = dims[1], d = dims[2];
        if (b != static_cast<int64_t>(n) || s != static_cast<int64_t>(seq) || d != dim_) {
            api->ReleaseValue(result);
            error = "model output shape does not match the manifest dimension";
            return false;
        }
        for (size_t k = 0; k < n; ++k) {
            std::vector<double> acc(static_cast<size_t>(dim_), 0.0);
            int64_t count = 0;
            for (int64_t t = 0; t < s; ++t) {
                if (!attn[k * seq + static_cast<size_t>(t)]) continue;
                const float *row = hidden + (static_cast<size_t>(k) * static_cast<size_t>(s) +
                                             static_cast<size_t>(t)) *
                                                static_cast<size_t>(d);
                for (int i = 0; i < dim_; ++i) acc[static_cast<size_t>(i)] += row[i];
                ++count;
            }
            if (count <= 0) count = 1;
            for (int i = 0; i < dim_; ++i)
                acc[static_cast<size_t>(i)] /= static_cast<double>(count);
            normalize_into(items[base + k].orig, nullptr, &acc);
        }
    } else if (nd == 2) {
        if (dims[0] != static_cast<int64_t>(n) || dims[1] != dim_) {
            api->ReleaseValue(result);
            error = "pooled output shape does not match the manifest";
            return false;
        }
        for (size_t k = 0; k < n; ++k)
            normalize_into(items[base + k].orig, hidden + k * static_cast<size_t>(dim_), nullptr);
    } else {
        api->ReleaseValue(result);
        error = "unexpected model output rank";
        return false;
    }
    api->ReleaseValue(result);
    for (size_t k = 0; k < n; ++k) {
        const float *row = out.data() + items[base + k].orig * static_cast<size_t>(dim_);
        double norm = 0;
        for (int j = 0; j < dim_; ++j) {
            if (!std::isfinite(row[j])) {
                error = "model produced non-finite embeddings";
                return false;
            }
            norm += static_cast<double>(row[j]) * row[j];
        }
        if (norm < 0.5 || norm > 1.5) {
            error = "model produced invalid embedding norm";
            return false;
        }
    }
    return true;
}

#endif /* NPP_HAVE_ONNXRUNTIME */

} /* namespace */

std::string onnx_backend_label(bool used_gpu, const std::string &requested, const char *gpu_id)
{
    std::string key = requested;
    for (char &ch : key)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    if (key.empty()) key = "auto";
    const bool automatic = key == "auto";
    std::string backend = (used_gpu && gpu_id) ? std::string("ONNX Runtime ") + gpu_id + " GPU"
                                               : "ONNX Runtime CPU";
    if (!used_gpu && !automatic && key != "cpu") backend += " (GPU unavailable)";
    return backend;
}

std::vector<ExecProviderInfo> execution_provider_catalog()
{
    std::vector<ExecProviderInfo> out;
#if NPP_HAVE_ONNXRUNTIME
    std::string ort_error;
    const OrtApi *api = npp_ort_api(ort_error);
    out.push_back({"auto", "Automatic", api != nullptr});
    std::vector<std::string> present = api ? available_ort_names(api) : std::vector<std::string>{};
    for (const KnownEp &ep : kKnown) {
        ExecProviderInfo info;
        info.id = ep.id;
        info.ort_name = ep.ort_name;
        if (std::strcmp(ep.id, "cpu") == 0) info.runtime_present = api != nullptr;
        else info.runtime_present = api && name_present(present, ep.ort_name);
        /* CPU stays in the list so the menu has a choice when the
         * runtime library is not installed. GPU rows appear only when
         * the loaded runtime advertises them. */
        if (std::strcmp(ep.id, "cpu") == 0 || info.runtime_present)
            out.push_back(std::move(info));
    }
#else
    out.push_back({"auto", "Automatic", false});
    ExecProviderInfo cpu;
    cpu.id = "cpu";
    cpu.ort_name = "CPUExecutionProvider";
    cpu.runtime_present = false;
    out.push_back(std::move(cpu));
#endif
    return out;
}

std::unique_ptr<EmbeddingProvider> make_onnx_provider(const OnnxProviderConfig &config, std::string &error)
{
#if NPP_HAVE_ONNXRUNTIME
    auto provider = std::make_unique<OnnxEmbeddingProvider>();
    if (!provider->open(config, error)) return nullptr;
    return provider;
#else
    (void)config;
    error = npp_ort_missing_message();
    return nullptr;
#endif
}

std::unique_ptr<EmbeddingProvider> make_turboembed_provider(std::string &error)
{
    error = "TurboEmbed is not linked";
    return nullptr;
}

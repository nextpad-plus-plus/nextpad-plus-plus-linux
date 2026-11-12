/*
 * Tokenizer, heatmap, sentence, index, manifest, and embedding checks.
 * Embedding numbers run only when the ONNX Runtime loader is compiled
 * in and the model files named below are present. Pass --bench to print
 * int8 throughput.
 */
#include "CancellableSpawn.h"
#include "BackendConsistency.h"
#include "DocumentTrack.h"
#include "EmbedFallback.h"
#include "ExecutionProviders.h"
#include "OrtRuntime.h"
#include "ModelZoo.h"
#include "SemanticHeatmapColors.h"
#include "SemanticInterfaces.h"
#include "SentenceSplitter.h"
#include "WordPieceTokenizer.h"
#include "indicator_alloc.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef SEMANTIC_FIXTURE_DIR
#define SEMANTIC_FIXTURE_DIR "tests/fixtures/semantic"
#endif
#ifndef SEMANTIC_MANIFEST
#define SEMANTIC_MANIFEST "resources/semantic/models.json"
#endif
#ifndef SEMANTIC_MODEL_DIR
#define SEMANTIC_MODEL_DIR ""
#endif

namespace {

int g_fails = 0;

void expect(bool ok, const std::string &msg)
{
    if (ok) return;
    std::cerr << "FAIL " << msg << "\n";
    ++g_fails;
}

struct Record {
    std::string text;
    std::vector<int> ids;
    std::vector<float> vec;
};

bool read_records(const std::string &path, std::vector<Record> &out, bool want_vec, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("TEXT ", 0) != 0) {
            error = "expected TEXT in " + path + ": " + line;
            return false;
        }
        size_t nbytes = static_cast<size_t>(std::stoul(line.substr(5)));
        Record rec;
        rec.text.resize(nbytes);
        if (nbytes && !in.read(rec.text.data(), static_cast<std::streamsize>(nbytes))) {
            error = "short TEXT payload in " + path;
            return false;
        }
        int sep = in.get();
        if (sep != '\n') {
            error = "missing newline after TEXT payload in " + path;
            return false;
        }
        if (!std::getline(in, line)) {
            error = "missing IDS/VEC in " + path;
            return false;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (want_vec) {
            if (line.rfind("VEC ", 0) != 0) {
                error = "expected VEC in " + path;
                return false;
            }
            int n = std::stoi(line.substr(4));
            if (!std::getline(in, line)) {
                error = "missing vector line";
                return false;
            }
            std::istringstream ss(line);
            float v;
            while (ss >> v) rec.vec.push_back(v);
            if (static_cast<int>(rec.vec.size()) != n) {
                error = "vector width mismatch";
                return false;
            }
        } else {
            if (line.rfind("IDS ", 0) != 0) {
                error = "expected IDS in " + path;
                return false;
            }
            int n = std::stoi(line.substr(4));
            if (!std::getline(in, line)) {
                error = "missing id line";
                return false;
            }
            std::istringstream ss(line);
            int id;
            while (ss >> id) rec.ids.push_back(id);
            if (static_cast<int>(rec.ids.size()) != n) {
                error = "id count mismatch";
                return false;
            }
        }
        out.push_back(std::move(rec));
    }
    if (out.empty()) {
        error = "no records in " + path;
        return false;
    }
    return true;
}

double cosine(const std::vector<float> &a, const std::vector<float> &b)
{
    double dot = 0, na = 0, nb = 0;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    if (na <= 0 || nb <= 0) return 0;
    return dot / std::sqrt(na * nb);
}

void test_tokens()
{
    WordPieceTokenizer tok;
    std::string error;
    expect(tok.load(std::string(SEMANTIC_FIXTURE_DIR) + "/vocab.txt", error), error);
    std::vector<Record> recs;
    expect(read_records(std::string(SEMANTIC_FIXTURE_DIR) + "/golden_tokens.txt", recs, false, error), error);
    for (size_t i = 0; i < recs.size(); ++i) {
        std::vector<int32_t> ids;
        bool ok = tok.encode(recs[i].text, 256, ids);
        expect(ok, "encode failed on record " + std::to_string(i));
        if (!ok) continue;
        bool same = ids.size() == recs[i].ids.size();
        for (size_t k = 0; same && k < ids.size(); ++k)
            same = ids[k] == recs[i].ids[k];
        if (!same) {
            std::ostringstream msg;
            msg << "token mismatch on record " << i << " text bytes " << recs[i].text.size()
                << " got";
            for (int id : ids) msg << " " << id;
            expect(false, msg.str());
        }
    }
}

void test_colors()
{
    uint32_t low = SemanticHeatmap::colorBGR(0.0);
    uint32_t high = SemanticHeatmap::colorBGR(1.0);
    expect(low == 0x4145D6u, "score 0 color");
    expect(high == 0x458A0Bu, "score 1 color");
    expect((low & 255) > (high & 255), "low score is redder");
    expect(((high >> 8) & 255) > ((low >> 8) & 255), "high score is greener");
    uint32_t strict = SemanticHeatmap::colorBGR(0.40, -1);
    uint32_t standard = SemanticHeatmap::colorBGR(0.40, 0);
    uint32_t broad = SemanticHeatmap::colorBGR(0.40, 1);
    expect(strict != standard && standard != broad && strict != broad,
           "sensitivity changes the ramp");
    uint32_t unrelated = SemanticHeatmap::colorBGR(0.05);
    uint32_t related = SemanticHeatmap::colorBGR(0.32);
    uint32_t paraphrase = SemanticHeatmap::colorBGR(0.50);
    expect(unrelated != related && related != paraphrase && unrelated != paraphrase,
           "ramp is graded across the model range");
    expect((unrelated & 255) > (paraphrase & 255), "paraphrase is less red");
    expect(((paraphrase >> 8) & 255) > ((unrelated >> 8) & 255), "paraphrase is greener");
    expect(SemanticHeatmap::bandForScore(0.10) == 0, "band 0.10");
    expect(SemanticHeatmap::bandForScore(0.28) == 2, "band 0.28");
    expect(SemanticHeatmap::bandForScore(0.58) == 5, "band 0.58");
    expect(SemanticHeatmap::bandForScore(std::nan("")) == -1, "nan band");
    expect(SemanticHeatmap::colorBGR(std::nan("")) == 0x8E8E8Eu, "nan color");
}

void test_model_color_profiles()
{
    using namespace SemanticHeatmap;
    expect(colorBGR(0.14, 0, Profile::AppleSentence) == 0x4145D6, "Apple Sentence red stop");
    expect(colorBGR(0.22, 0, Profile::AppleSentence) == 0x6469B2, "Apple Sentence brown stop");
    expect(colorBGR(0.30, 0, Profile::AppleSentence) == 0x8E8E8E, "Apple Sentence grey start");
    expect(colorBGR(0.42, 0, Profile::AppleSentence) == 0x8E8E8E, "Apple Sentence grey end");
    expect(colorBGR(0.50, 0, Profile::AppleSentence) == 0x71CC2E, "Apple Sentence green stop");
    expect(colorBGR(0.68, 0, Profile::AppleSentence) == 0x458A0B, "Apple Sentence deep green");
    expect(colorBGR(0.82, 0, Profile::AppleContextual) == 0x4145D6, "Apple Contextual red stop");
    expect(colorBGR(0.95, 0, Profile::AppleContextual) == 0x458A0B, "Apple Contextual deep green");
    for (auto profile : {Profile::MiniLM, Profile::AppleSentence, Profile::AppleContextual}) {
        unsigned count;
        const auto *stops = stopsFor(profile, count);
        for (int sensitivity : {-1, 0, 1}) {
            double exponent = sensitivity < 0 ? 1.4 : sensitivity > 0 ? 0.7 : 1.0;
            for (unsigned i = 0; i < count; ++i) {
                double input = std::pow(stops[i].score, 1.0 / exponent);
                uint32_t actual = colorBGR(input, sensitivity, profile);
                expect(std::abs(int(actual & 255) - stops[i].r) <= 1 &&
                       std::abs(int((actual >> 8) & 255) - stops[i].g) <= 1 &&
                       std::abs(int((actual >> 16) & 255) - stops[i].b) <= 1,
                       "calibrated stops at every sensitivity");
            }
            expect(colorBGR(std::nan(""), sensitivity, profile) == 0x8E8E8E, "invalid score grey");
        }
        uint32_t previous = 0;
        for (int i = 0; i < 6; ++i) {
            double sample = legendScore(profile, i);
            expect(bandForScore(sample, profile) == i, "model legend maps to its band");
            uint32_t color = colorBGR(sample, 0, profile);
            expect(i == 0 || color != previous, "legend has distinct swatches");
            previous = color;
        }
    }
    Profile profile;
    expect(!profileFromID("cuda", profile), "hardware is not a heatmap profile");
}

void test_sentences()
{
    auto spans = split_sentences("One sentence. Two sentences.");
    expect(spans.size() >= 2, "two sentences");
    int64_t prev = -1;
    for (const SentenceSpan &s : spans) {
        expect(s.byte_start >= 0, "start");
        expect(s.byte_length > 0, "length");
        expect(s.byte_start > prev, "ordered");
        expect(static_cast<size_t>(s.byte_start + s.byte_length) <= std::strlen("One sentence. Two sentences."),
               "in range");
        prev = s.byte_start;
    }
    expect(split_sentences("   \n\t  ").empty(), "blank");
    expect(split_sentences("").empty(), "empty");
    expect(split_sentences("\xff").empty(), "bad utf-8");
    expect(!segment_has_content("1."), "bare number");
    expect(!segment_has_content("- "), "list dash");
    expect(!segment_has_content("***"), "stars");
    expect(!segment_has_content("42)"), "digits");
    expect(!segment_has_content("•"), "bullet");
    expect(segment_has_content("Backup generator failed"), "real phrase");
    expect(split_sentences("1.").empty(), "number dropped");
    expect(split_sentences("- ").empty(), "dash dropped");
    expect(split_sentences("***").empty(), "stars dropped");
    auto kept = split_sentences("Backup generator failed");
    expect(kept.size() == 1, "phrase kept");
    auto listed = split_sentences("1.\n2.\n- \n***\nBackup generator failed.");
    bool saw_phrase = false;
    for (const SentenceSpan &s : listed) {
        expect(segment_has_content(s.text), "listed span has words");
        if (s.text.find("Backup generator failed") != std::string::npos)
            saw_phrase = true;
    }
    expect(saw_phrase, "phrase survives a list");
    auto one = split_sentences("Good coffee.");
    expect(one.size() == 1, "single sentence");
    if (one.size() == 1) {
        expect(one[0].byte_start == 0, "single starts at 0");
        expect(one[0].byte_length == 12, "single covers Good coffee.");
    }
}

void test_index()
{
    auto engine = make_cpu_similarity_engine();
    auto index = make_memory_vector_index(*engine);
    index->reset(2);
    float a[] = {1.f, 0.f};
    float b[] = {0.f, 1.f};
    float c[] = {0.6f, 0.8f};
    expect(index->add(a, 10), "add a");
    expect(index->add(b, 11), "add b");
    expect(index->add(c, 12), "add c");
    SemanticHit hits[3];
    size_t n = index->score_all(a, hits, 3);
    expect(n == 3, "score count");
    if (n == 3) {
        expect(hits[0].sentence_id == 10 && std::fabs(hits[0].score - 1.f) < 1e-5, "score a");
        expect(hits[1].sentence_id == 11 && std::fabs(hits[1].score) < 1e-5, "score b");
        expect(hits[2].sentence_id == 12 && std::fabs(hits[2].score - 0.6f) < 1e-5, "score c");
    }
    SemanticHit top[1];
    expect(index->search(a, 1, top) == 1 && top[0].sentence_id == 10, "top hit");
}

void test_manifest()
{
    ModelZoo zoo;
    std::string error;
    expect(zoo.load_file(SEMANTIC_MANIFEST, error), error.empty() ? "manifest" : error);
    expect(zoo.default_id() == "all-MiniLM-L6-v2-fp32", "default id");
    expect(zoo.models().size() == 3, "three models");
    const ModelSpec *spec = zoo.find("all-MiniLM-L6-v2");
    expect(spec != nullptr, "default spec");
    if (!spec) return;
    expect(spec->dimension == 384, "dim");
    expect(spec->pooling == "mean", "pooling");
    expect(spec->max_tokens == 256, "max tokens");
    expect(spec->vocab.sha256.size() == 64, "vocab sha");
    expect(spec->model.sha256.size() == 64, "model sha");
    expect(!spec->model.filename.empty(), "arch file");
    expect(zoo.find("missing") == nullptr, "unknown id");

    std::string hex;
    std::string vocab = std::string(SEMANTIC_FIXTURE_DIR) + "/vocab.txt";
    expect(ModelZoo::file_sha256(vocab, hex, error), "hash vocab");
    gchar *dir = g_dir_make_tmp("npp-sem-XXXXXX", nullptr);
    expect(dir != nullptr, "tmp dir");
    if (!dir) return;
    std::string dest_vocab = std::string(dir) + "/vocab.txt";
    std::string dest_model = std::string(dir) + "/model.onnx";
    {
        std::ifstream in(vocab, std::ios::binary);
        std::ofstream out1(dest_vocab, std::ios::binary);
        std::ofstream out2(dest_model, std::ios::binary);
        out1 << in.rdbuf();
        in.clear();
        in.seekg(0);
        out2 << in.rdbuf();
    }
    ModelSpec fake = *spec;
    fake.vocab.filename = "vocab.txt";
    fake.vocab.sha256 = hex;
    fake.vocab.url = "http://127.0.0.1:9/missing-vocab";
    fake.model.filename = "model.onnx";
    fake.model.sha256 = hex;
    fake.model.url = "http://127.0.0.1:9/missing-model";
    std::string err2;
    expect(zoo.ensure(fake, dir, err2), err2.empty() ? "ensure cache hit" : err2);
    g_free(dir);
}

void test_providers()
{
    std::string error;
    auto turbo = make_turboembed_provider(error);
    expect(!turbo && error.find("TurboEmbed") != std::string::npos, error);
    bool ort_loaded = npp_ort_runtime_available();
    auto cat = execution_provider_catalog();
    bool saw_cpu = false;
    for (const ExecProviderInfo &ep : cat) {
        if (ep.id == "cpu") {
            saw_cpu = true;
            expect(ep.runtime_present == ort_loaded,
                   ort_loaded ? "cpu present" : "cpu absent without ONNX Runtime");
        }
        if (ep.id == "auto")
            expect(ep.runtime_present == ort_loaded, "auto follows the loaded runtime");
    }
    expect(saw_cpu, "catalog lists cpu");
    if (!ort_loaded) {
        OnnxProviderConfig cfg;
        cfg.dimension = 384;
        cfg.max_tokens = 256;
        cfg.pooling = "mean";
        auto missing = make_onnx_provider(cfg, error);
        expect(!missing && error.find(npp_ort_missing_message()) != std::string::npos, error);
    }
}

#if NPP_HAVE_ONNXRUNTIME

std::string model_dir()
{
    const char *env = std::getenv("NPP_SEMANTIC_TEST_MODEL_DIR");
    if (env && *env) return env;
    return SEMANTIC_MODEL_DIR;
}

bool file_ok(const std::string &path)
{
    return g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR);
}

void compare_model(const std::string &model_path, const std::string &golden_path,
                   int max_tokens, double floor_all, double floor_short, const char *label)
{
    std::string dir = model_dir();
    std::string vocab = dir + "/vocab.txt";
    if (dir.empty() || !file_ok(model_path) || !file_ok(vocab) || !file_ok(golden_path)) {
        std::cout << "skip " << label << " (model or golden missing)\n";
        return;
    }
    OnnxProviderConfig cfg;
    cfg.model_path = model_path;
    cfg.vocab_path = vocab;
    cfg.dimension = 384;
    cfg.max_tokens = max_tokens;
    cfg.pooling = "mean";
    cfg.execution_provider = "cpu";
    std::string error;
    auto provider = make_onnx_provider(cfg, error);
    expect(provider != nullptr, error.empty() ? std::string("load ") + label : error);
    if (!provider) return;
    std::vector<Record> gold;
    expect(read_records(golden_path, gold, true, error), error);
    if (!error.empty() && gold.empty()) return;
    /* One text per call. The int8 graph moves when a batch pads shorter
     * sequences up to a longer neighbour, so the fixture is unpadded. */
    for (size_t i = 0; i < gold.size(); ++i) {
        std::vector<float> got;
        expect(provider->embed_batch({gold[i].text}, got, error, {}),
               error.empty() ? "embed" : error);
        if (got.size() != 384) {
            expect(false, "embedding width");
            return;
        }
        double c = cosine(got, gold[i].vec);
        double floor = gold[i].text.size() < 200 ? floor_short : floor_all;
        if (c < floor) {
            std::ostringstream msg;
            msg << label << " cosine " << c << " on record " << i
                << " (" << gold[i].text.size() << " bytes), floor " << floor;
            expect(false, msg.str());
        }
    }
}

void test_embeddings()
{
    std::string dir = model_dir();
    compare_model(dir + "/model_fp32.onnx",
                  std::string(SEMANTIC_FIXTURE_DIR) + "/golden_st.txt",
                  256, 0.999, 0.999, "fp32 vs sentence-transformers");
#if defined(__aarch64__)
    // The AVX2 quantization reference is architecture-specific. Test the
    // manifest's ARM export against the shared semantic reference instead.
    compare_model(dir + "/model_qint8_arm64.onnx",
                  std::string(SEMANTIC_FIXTURE_DIR) + "/golden_st.txt",
                  256, 0.93, 0.97, "ARM int8 vs sentence-transformers");
#else
    compare_model(dir + "/model_quint8_avx2.onnx",
                  std::string(SEMANTIC_FIXTURE_DIR) + "/golden_int8.txt",
                  256, 0.999, 0.999, "int8 vs ort reference");
    compare_model(dir + "/model_quint8_avx2.onnx",
                  std::string(SEMANTIC_FIXTURE_DIR) + "/golden_st.txt",
                  256, 0.93, 0.97, "int8 vs sentence-transformers");
#endif
}

void test_automatic_provider()
{
    const bool require_gpu = std::getenv("NPP_SEMANTIC_REQUIRE_GPU") != nullptr;
    OnnxProviderConfig cfg;
    cfg.model_path = model_dir() + "/model_fp32.onnx";
    cfg.vocab_path = model_dir() + "/vocab.txt";
    cfg.dimension = 384;
    cfg.max_tokens = 256;
    cfg.pooling = "mean";
    if (!file_ok(cfg.model_path) || !file_ok(cfg.vocab_path)) {
        expect(!require_gpu, "GPU test requires fp32 model and vocab");
        std::cout << "skip automatic provider (model missing)\n";
        return;
    }
    std::string error;
    auto automatic = make_onnx_provider(cfg, error);
    expect(automatic != nullptr, error);
    if (!automatic) return;
    std::cout << "automatic backend: " << automatic->backend_name() << "\n";
    if (require_gpu)
        expect(automatic->backend_name().find("GPU") != std::string::npos &&
               automatic->backend_name().find("CPU") == std::string::npos,
               "automatic selection must execute on GPU");
    cfg.execution_provider = "cpu";
    auto cpu = make_onnx_provider(cfg, error);
    expect(cpu != nullptr, error);
    if (!cpu) return;
    const std::vector<std::string> texts = {
        "The hospital runs on its backup generator.", "A power cut.",
        "Children drew pictures at school.", "The emergency supply kept the ward lit."
    };
    std::vector<float> actual, reference;
    expect(automatic->embed_batch(texts, actual, error, {}), error);
    expect(cpu->embed_batch(texts, reference, error, {}), error);
    if (require_gpu) expect(automatic->backend_name().find("CPU") == std::string::npos,
                            "GPU must remain active after inference");
    expect(actual.size() == texts.size() * 384 && actual.size() == reference.size(), "GPU batch size");
    if (actual.size() != texts.size() * 384 || actual.size() != reference.size()) return;
    for (size_t i = 0; i < texts.size(); ++i) {
        std::vector<float> a(actual.begin() + i * 384, actual.begin() + (i + 1) * 384);
        std::vector<float> b(reference.begin() + i * 384, reference.begin() + (i + 1) * 384);
        expect(cosine(a, b) > 0.999, "automatic/CPU embedding parity");
        for (float v : a) expect(std::isfinite(v), "finite automatic embedding");
        std::vector<float> aq(actual.begin(), actual.begin() + 384);
        std::vector<float> bq(reference.begin(), reference.begin() + 384);
        double as = cosine(aq, a), bs = cosine(bq, b);
        expect(std::abs(as - bs) < 0.002, "CPU/GPU similarity scores agree");
        expect(SemanticHeatmap::bandForScore(as) == SemanticHeatmap::bandForScore(bs),
               "CPU/GPU heatmap bands agree");
        for (int sensitivity : {-1, 0, 1}) {
            auto ac = SemanticHeatmap::colorBGR(as, sensitivity);
            auto bc = SemanticHeatmap::colorBGR(bs, sensitivity);
            for (int shift : {0, 8, 16})
                expect(std::abs(int((ac >> shift) & 255) - int((bc >> shift) & 255)) <= 3,
                       "CPU/GPU heatmap colours agree");
        }
    }
    bool cuda_present = false;
    for (const auto &ep : execution_provider_catalog()) if (ep.id == "cuda") cuda_present = true;
    if (!cuda_present) {
        cfg.execution_provider = "cuda";
        auto fallback = make_onnx_provider(cfg, error);
        expect(fallback && fallback->backend_name().find("CPU") != std::string::npos,
               "missing GPU falls back to CPU");
        if (fallback) expect(fallback->embed_batch(texts, actual, error, {}), error);
    }
}

int bench()
{
    std::string dir = model_dir();
    std::string model = dir + "/model_quint8_avx2.onnx";
    std::string vocab = dir + "/vocab.txt";
    if (!file_ok(model) || !file_ok(vocab)) {
        std::cerr << "bench needs int8 model and vocab in " << dir << "\n";
        return 1;
    }
    OnnxProviderConfig cfg;
    cfg.model_path = model;
    cfg.vocab_path = vocab;
    cfg.dimension = 384;
    cfg.max_tokens = 256;
    cfg.pooling = "mean";
    cfg.execution_provider = "cpu";
    std::string error;
    auto provider = make_onnx_provider(cfg, error);
    if (!provider) {
        std::cerr << error << "\n";
        return 1;
    }
    const char *samples[] = {
        "The cat sat on the mat.",
        "Machine learning models embed text.",
        "A completely unrelated remark about the weather in Oslo.",
        "Semantic search finds sentences with similar meaning.",
        "Short note about the river bank.",
    };
    std::vector<std::string> texts;
    for (int i = 0; i < 100; ++i)
        texts.push_back(std::string(samples[i % 5]) + " item " + std::to_string(i));
    std::vector<float> warm;
    if (!provider->embed_batch({texts[0]}, warm, error, {})) {
        std::cerr << error << "\n";
        return 1;
    }
    std::vector<float> out;
    auto t0 = std::chrono::steady_clock::now();
    if (!provider->embed_batch(texts, out, error, {})) {
        std::cerr << error << "\n";
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "int8 throughput: " << (texts.size() / sec)
              << " sentences/sec (" << texts.size() << " sentences, " << sec
              << " s, batch 8, intra-op threads 2, after one warmup sentence)\n";
    return 0;
}

#endif /* NPP_HAVE_ONNXRUNTIME */

std::string hex64(char fill)
{
    return std::string(64, fill);
}

std::string manifest_model(const std::string &id, const std::string &arch_key)
{
    return "    {\n"
           "      \"id\": \"" + id + "\",\n"
           "      \"name\": \"" + id + "\",\n"
           "      \"dimension\": 4,\n"
           "      \"pooling\": \"mean\",\n"
           "      \"max_tokens\": 8,\n"
           "      \"vocab\": {\"filename\": \"vocab.txt\", \"url\": \"file:///missing-vocab\",\n"
           "        \"sha256\": \"" + hex64('a') + "\"},\n"
           "      \"model\": {\"" + arch_key + "\": {\"filename\": \"m.onnx\", \"url\": \"file:///missing-model\",\n"
           "        \"sha256\": \"" + hex64('b') + "\"}}\n"
           "    }";
}

bool write_manifest(const std::string &path, const std::string &json)
{
    std::ofstream out(path);
    if (!out) return false;
    out << json;
    return out.good();
}

void test_manifest_arch_filter()
{
    std::string arch = ModelZoo::host_arch();
    std::string other = arch == "aarch64" ? "x86_64" : "aarch64";
    gchar *dir = g_dir_make_tmp("npp-sem-manifest-XXXXXX", nullptr);
    expect(dir != nullptr, "manifest tmp");
    if (!dir) return;
    std::string path = std::string(dir) + "/models.json";

    std::string keep = "{\n  \"default\": \"keep\",\n  \"models\": [\n" +
                       manifest_model("skip-me", other) + ",\n" +
                       manifest_model("keep", "all") + "\n  ]\n}\n";
    expect(write_manifest(path, keep), "write keep manifest");
    ModelZoo zoo;
    std::string error;
    expect(zoo.load_file(path, error), error.empty() ? "skip other arch" : error);
    expect(zoo.models().size() == 1, "one model remains");
    expect(zoo.find("skip-me") == nullptr, "other arch was skipped");
    expect(zoo.find("keep") != nullptr, "all-arch model kept");
    expect(zoo.default_id() == "keep", "default kept");

    std::string skipped_default = "{\n  \"default\": \"needs-file\",\n  \"models\": [\n" +
                                  manifest_model("needs-file", other) + ",\n" +
                                  manifest_model("keep", "all") + "\n  ]\n}\n";
    expect(write_manifest(path, skipped_default), "write default-skip manifest");
    ModelZoo missing_default;
    expect(!missing_default.load_file(path, error), "default without a file must fail");
    expect(error.find("needs-file") != std::string::npos, error);
    expect(error.find("no file") != std::string::npos, error);

    std::string none = "{\n  \"default\": \"absent\",\n  \"models\": [\n" +
                       manifest_model("one", other) + ",\n" +
                       manifest_model("two", other) + "\n  ]\n}\n";
    expect(write_manifest(path, none), "write empty-arch manifest");
    ModelZoo empty;
    expect(!empty.load_file(path, error), "no runnable model must fail");
    expect(error == "manifest lists no models", error);
    g_free(dir);
}

void test_model_files_reused()
{
    gchar *dir = g_dir_make_tmp("npp-sem-reuse-XXXXXX", nullptr);
    expect(dir != nullptr, "model reuse temporary directory");
    if (!dir) return;
    std::string root = dir, source = root + "/source";
    std::string error, hash;
    expect(g_file_set_contents(source.c_str(), "cached model fixture", -1, nullptr), "write model fixture");
    expect(ModelZoo::file_sha256(source, hash, error), error);
    ModelSpec spec;
    spec.id = "reuse-test";
    spec.vocab = {"vocab.txt", "file://" + source, hash};
    spec.model = {"model.onnx", "file://" + source, hash};
    CancellableSpawn spawn;
    int children = 0;
    int download_notifications = 0;
    spawn.set_pre_lock_hook([&] { ++children; });
    ModelZoo zoo;
    expect(zoo.ensure(spec, root + "/cache", error, &spawn, std::nullopt,
                      [&] { ++download_notifications; }), error);
    expect(children == 2, "first installation downloads vocabulary and model once");
    expect(download_notifications == 2, "both real transfers report downloading");
    std::remove(source.c_str()); // the source is no longer available
    expect(zoo.ensure(spec, root + "/cache", error, &spawn, std::nullopt,
                      [&] { ++download_notifications; }), error);
    expect(children == 2, "verified model files are reused without any network request");
    expect(download_notifications == 2, "cached files do not report a download");
    std::remove((root + "/cache/vocab.txt").c_str());
    std::remove((root + "/cache/model.onnx").c_str());
    g_rmdir((root + "/cache").c_str());
    g_rmdir(root.c_str());
    g_free(dir);
}

void test_download_failure_message()
{
    std::string network = ModelZoo::download_failure_message("offline-demo", false, 6);
    std::string curl = ModelZoo::download_failure_message("offline-demo", true);
    expect(network.find("offline-demo") != std::string::npos, network);
    expect(network.find("not installed") != std::string::npos, network);
    expect(network.find("network") != std::string::npos, network);
    expect(network.find("curl") == std::string::npos, network);
    expect(curl.find("curl") != std::string::npos, curl);
    expect(ModelZoo::download_failure_message("m", false, 22).find("HTTP") != std::string::npos, "HTTP failure");
    expect(ModelZoo::download_failure_message("m", false, 23).find("disk") != std::string::npos, "disk failure");
    expect(ModelZoo::download_failure_message("m", false, 60).find("TLS") != std::string::npos, "TLS failure");

    ModelSpec spec;
    spec.id = "offline-demo";
    spec.vocab.filename = "vocab.txt";
    spec.vocab.url = "file:///no/such/npp-semantic-vocab";
    spec.vocab.sha256 = hex64('a');
    spec.model.filename = "m.onnx";
    spec.model.url = "file:///no/such/npp-semantic-model";
    spec.model.sha256 = hex64('b');
    gchar *dir = g_dir_make_tmp("npp-sem-dl-XXXXXX", nullptr);
    expect(dir != nullptr, "download tmp");
    if (!dir) return;
    ModelZoo zoo;
    std::string error;
    std::string diagnostic;
    guint log_handler = g_log_set_handler(nullptr, G_LOG_LEVEL_MESSAGE,
        [](const gchar *, GLogLevelFlags, const gchar *message, gpointer data) {
            *static_cast<std::string *>(data) = message;
        }, &diagnostic);
    bool installed = zoo.ensure(spec, dir, error);
    g_log_remove_handler(nullptr, log_handler);
    expect(!installed, "missing file should fail");
    expect(diagnostic.find("curl exit 37") != std::string::npos &&
           diagnostic.find("curl:") != std::string::npos,
           "curl failure code and stderr must be logged");
    expect(error.find("offline-demo") != std::string::npos, error);
    expect(error.find("not installed") != std::string::npos, error);
    expect(error.find("source file") != std::string::npos, error);
    expect(error.find("curl:") == std::string::npos, error);
    expect(error != "cancelled", error);

    CancellableSpawn spawn;
    auto ticket = spawn.ticket();
    spawn.cancel();
    std::string stale;
    expect(!zoo.ensure(spec, dir, stale, &spawn, ticket) && stale == "cancelled",
           "queued ensure cannot acquire a new ticket after cancel");
    spawn.retire();
    std::string cancelled;
    expect(!zoo.ensure(spec, dir, cancelled, &spawn), "retired download should fail");
    expect(cancelled == "cancelled", cancelled);
    g_free(dir);
}

void test_backend_label()
{
    expect(onnx_backend_label(false, "auto", nullptr) == "ONNX Runtime CPU", "automatic cpu");
    expect(onnx_backend_label(false, "cpu", nullptr) == "ONNX Runtime CPU", "explicit cpu");
    expect(onnx_backend_label(false, "", nullptr) == "ONNX Runtime CPU", "empty request");
    expect(onnx_backend_label(false, "cuda", nullptr) == "ONNX Runtime CPU (GPU unavailable)",
           "explicit cuda");
    expect(onnx_backend_label(false, "OpenVINO", nullptr) == "ONNX Runtime CPU (GPU unavailable)",
           "explicit openvino");
    expect(onnx_backend_label(true, "auto", "cuda") == "ONNX Runtime cuda GPU", "cuda ran");
    expect(std::string(onnx_cpu_fallback_label()) == "ONNX Runtime CPU (GPU inference failed)",
           "fallback label");
}

void test_embed_fallback_restarts()
{
    int calls = 0;
    int resets = 0;
    int fallbacks = 0;
    bool gpu = true;
    std::vector<size_t> seen;
    std::string error;
    bool ok = run_embed_batches(
        3, 2, gpu,
        [&](size_t base, size_t n, std::vector<size_t> &indexes, std::string &err) {
            ++calls;
            if (gpu && base > 0) {
                err = "gpu batch failed";
                return false;
            }
            indexes.clear();
            for (size_t k = 0; k < n; ++k) indexes.push_back(base + k);
            return true;
        },
        [&](std::string &err) {
            ++fallbacks;
            err.clear();
            return true;
        },
        [&](const std::vector<size_t> &indexes) {
            if (indexes.empty()) {
                ++resets;
                seen.clear();
                return true;
            }
            seen.insert(seen.end(), indexes.begin(), indexes.end());
            return true;
        },
        error);
    expect(ok, error.empty() ? "restart ok" : error);
    expect(!gpu, "still marked GPU");
    expect(fallbacks == 1, "fallback once");
    expect(resets == 1, "one discard");
    expect(calls == 4, "every batch ran again");
    expect(seen.size() == 3 && seen[0] == 0 && seen[1] == 1 && seen[2] == 2, "cpu rows only");

    int fallback_calls = 0;
    bool stay = false;
    std::string hard;
    bool failed = run_embed_batches(
        1, 1, stay,
        [&](size_t, size_t, std::vector<size_t> &, std::string &err) {
            err = "cpu failed";
            return false;
        },
        [&](std::string &) {
            ++fallback_calls;
            return true;
        },
        {},
        hard);
    expect(!failed && hard == "cpu failed", hard);
    expect(fallback_calls == 0, "cpu failure must not fall back");

    bool again = true;
    int second = 0;
    std::string give_up;
    bool gave = run_embed_batches(
        1, 1, again,
        [&](size_t, size_t, std::vector<size_t> &, std::string &err) {
            err = "still bad";
            return false;
        },
        [&](std::string &err) {
            ++second;
            err.clear();
            return second == 1;
        },
        {},
        give_up);
    expect(!gave, "second failure must stop");
    expect(second == 1, "fallback only on the first failure");
    expect(!again, "gpu flag cleared");
}

void test_document_track_and_handles()
{
    DocumentTrack track;
    const std::uintptr_t doc = 100;
    const std::uintptr_t view_a = 1;
    const std::uintptr_t view_b = 2;
    expect(track.classify_attach(view_a, doc) == DocAttach::SwitchDocument, "first attach");
    track.switch_to(view_a, doc);
    expect(track.classify_attach(view_a, doc) == DocAttach::SameView, "same view");
    expect(track.classify_attach(view_b, doc) == DocAttach::SameDocument, "clone");
    track.retarget(view_b);
    expect(track.view() == view_b && track.doc() == doc, "retarget keeps the document");
    track.retarget(view_a);
    expect(track.accepts_modification(doc), "clone edit");
    expect(!track.accepts_modification(999), "other document");

    std::vector<DocView> both = {{view_a, doc}, {view_b, doc}};
    DocCloseResult keep = track.classify_close(view_b, doc, both);
    expect(keep.action == DocClose::Keep, "closing the other clone");
    track.apply_close(keep);
    expect(track.view() == view_a && track.doc() == doc, "tracked view stays");

    DocCloseResult retarget = track.classify_close(view_a, doc, both);
    expect(retarget.action == DocClose::Retarget && retarget.view == view_b, "retarget on close");
    track.handles().push_back(7);
    track.apply_close(retarget);
    expect(track.view() == view_b && track.handles().size() == 1, "handles survive retarget");

    std::vector<DocView> only_b = {{view_b, doc}};
    DocCloseResult last = track.classify_close(view_b, doc, only_b);
    expect(last.action == DocClose::ClearLast, "last view");
    track.apply_close(last);
    expect(!track.active() && track.handles().empty(), "cleared once");

    std::map<int, int> line_of;
    std::map<int, unsigned> mask;
    int next_handle = 1;
    MarkerFns fns;
    fns.line_from_handle = [&](int handle) {
        auto it = line_of.find(handle);
        return it == line_of.end() ? -1 : it->second;
    };
    fns.delete_handle = [&](int handle) { line_of.erase(handle); };
    fns.mask_on_line = [&](int line) {
        auto it = mask.find(line);
        return it == mask.end() ? 0u : it->second;
    };
    fns.add_on_line = [&](int line) {
        int handle = next_handle++;
        line_of[handle] = line;
        mask[line] |= 1u;
        return handle;
    };
    std::vector<int> handles;
    const unsigned bit = 1u;
    sync_marker_handles(handles, {5}, bit, fns);
    expect(handles.size() == 1 && line_of[handles[0]] == 5, "added by handle");
    int kept = handles[0];
    line_of[kept] = 6;
    sync_marker_handles(handles, {6}, bit, fns);
    expect(handles.size() == 1 && handles[0] == kept, "moved line kept its handle");
    expect(line_of.count(kept) == 1, "handle was not deleted");
    mask[3] = bit;
    sync_marker_handles(handles, {3, 6}, bit, fns);
    expect(handles.size() == 1 && handles[0] == kept, "pre-existing bookmark left alone");
    // Scintilla moves a deleted line's marker to its neighbour.
    line_of[kept] = 4;
    sync_marker_handles(handles, {3}, bit, fns);
    expect(line_of.count(kept) == 0, "moved marker removed by handle");
    expect(mask[3] == bit, "user bookmark remains");

    MarkerSyncState state;
    mask.clear(); line_of.clear(); handles.clear(); next_handle = 0;
    sync_marker_handles(handles, {7}, bit, fns, &state);
    expect(handles.size() == 1 && handles[0] == 0, "zero is a valid marker handle");
    line_of.clear(); mask.clear(); // user removes the feature bookmark
    sync_marker_handles(handles, {7, 8}, bit, fns, &state);
    expect(handles.size() == 1 && line_of[handles[0]] == 8, "next batch respects removed bookmark");
    sync_marker_handles(handles, {7, 8}, bit, fns, &state);
    expect(handles.size() == 1, "later batches still respect removal");
    sync_marker_handles(handles, {}, bit, fns, &state);
    mask.clear();
    sync_marker_handles(handles, {7}, bit, fns, &state);
    expect(handles.size() == 1, "new selection may add a bookmark again");
}

void test_indicator_alloc()
{
    int next = 9, out = -1;
    expect(npp_indicator_alloc(&next, 9, &out) && out == 10 && next == 19, "contiguous plugin range");
    expect(npp_indicator_alloc(&next, 1, &out) && out == 20 && next == 21, "last plugin slot");
    expect(!npp_indicator_alloc(&next, 1, &out) && out == 0 && next == 21, "never allocate editor slots");
    next = 18; out = 99;
    expect(!npp_indicator_alloc(&next, 2, &out) && out == 0 && next == 18, "do not span reserved 19");
    expect(!npp_indicator_alloc(nullptr, 1, &out) && out == 0, "null cursor clears output");
    expect(!npp_indicator_alloc(&next, 0, &out) && out == 0, "zero request clears output");
}

void test_query_backend_change()
{
    class QueryFallback : public EmbeddingProvider {
    public:
        uint64_t generation = 0;
        bool fail_cpu = false;
        int dimension() const override { return 2; }
        int max_tokens() const override { return 8; }
        bool available() const override { return true; }
        std::string backend_name() const override { return "test"; }
        uint64_t backend_generation() const override { return generation; }
        size_t token_count(const std::string &) const override { return 1; }
        bool embed_batch(const std::vector<std::string> &, std::vector<float> &out,
                         std::string &, const SemanticBatchProgress &) override {
            ++generation;
            out = {0, 1};
            return !fail_cpu;
        }
    } provider;
    auto engine = make_cpu_similarity_engine();
    auto index = make_memory_vector_index(*engine);
    for (bool fail : {false, true}) {
        provider.fail_cpu = fail;
        index->reset(2);
        std::unordered_map<std::string, std::vector<float>> cache{{"sentence", {1, 0}}};
        index->add(cache["sentence"].data(), 1);
        auto before = provider.backend_generation();
        expect(!invalidate_changed_backend(provider, before, *index, cache), "stable backend keeps vectors");
        std::vector<float> query; std::string error;
        provider.embed_batch({"query"}, query, error, {});
        expect(invalidate_changed_backend(provider, before, *index, cache), "query fallback requires rebuild even if CPU fails");
        expect(cache.empty() && index->count() == 0, "GPU cache and index both discarded before CPU query scoring");
    }
}

} /* namespace */

int main(int argc, char **argv)
{
    bool do_bench = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--bench") == 0) do_bench = true;

    test_tokens();
    test_colors();
    test_model_color_profiles();
    test_sentences();
    test_index();
    test_manifest();
    test_manifest_arch_filter();
    test_download_failure_message();
    test_model_files_reused();
    test_backend_label();
    test_embed_fallback_restarts();
    test_document_track_and_handles();
    test_indicator_alloc();
    test_query_backend_change();
    test_providers();
#if NPP_HAVE_ONNXRUNTIME
    if (do_bench) return bench();
    test_embeddings();
    test_automatic_provider();
#else
    (void)do_bench;
    expect(std::getenv("NPP_SEMANTIC_REQUIRE_GPU") == nullptr, "GPU required but runtime not linked");
    std::cout << "skip embeddings (ONNX Runtime not linked)\n";
#endif
    if (g_fails) {
        std::cerr << g_fails << " failure(s)\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}

#include "ModelZoo.h"
#include "CancellableSpawn.h"
#include "JsonMini.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <cctype>
#include <fstream>
#include <sstream>

namespace {

std::string read_file(const std::string &path, std::string &error)
{
    std::ifstream in(path);
    if (!in) {
        error = "Could not read " + path;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool parse_file(const Json &node, ModelFile &file, std::string &error)
{
    const Json *fn = node.field("filename");
    const Json *url = node.field("url");
    const Json *sha = node.field("sha256");
    if (!fn || !url || !sha || fn->type != Json::Type::String ||
        url->type != Json::Type::String || sha->type != Json::Type::String) {
        error = "model file entry needs filename, url, and sha256";
        return false;
    }
    file.filename = fn->str;
    file.url = url->str;
    file.sha256 = sha->str;
    if (file.sha256.size() != 64) {
        error = "sha256 must be 64 hex characters";
        return false;
    }
    for (char c : file.sha256) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            error = "sha256 must be hex";
            return false;
        }
    }
    return true;
}

bool download_url(const std::string &url, const std::string &dest,
                  const std::string &model_id, CancellableSpawn &spawn, CancellableSpawn::Ticket ticket, std::string &error,
                  const std::function<void()> &on_download)
{
    std::string partial = dest + ".partial";
    if (spawn.cancelled(ticket)) {
        error = "cancelled";
        return false;
    }
    if (on_download) on_download();
    CancellableSpawn::Result result = spawn.run({
        "curl", "-fsSL",
        "--retry", "3",
        "--retry-delay", "1",
        "--connect-timeout", "30",
        "--speed-limit", "1",
        "--speed-time", "60",
        "-o", partial,
        url
    }, ticket);
    if (result.cancelled || spawn.cancelled(ticket)) {
        g_unlink(partial.c_str());
        error = "cancelled";
        return false;
    }
    if (!result.started || !result.spawn_error.empty()) {
        g_message("Semantic model %s: curl could not start: %s", model_id.c_str(),
                  result.spawn_error.c_str());
        error = ModelZoo::download_failure_message(model_id, !result.started);
        g_unlink(partial.c_str());
        return false;
    }
    if (!result.exited || result.exit_code != 0) {
        g_message("Semantic model %s: curl exit %d: %s", model_id.c_str(),
                  result.exit_code, result.stderr_text.c_str());
        error = ModelZoo::download_failure_message(model_id, false, result.exit_code);
        g_unlink(partial.c_str());
        return false;
    }
    if (g_rename(partial.c_str(), dest.c_str()) != 0) {
        error = "could not move downloaded file into place";
        g_unlink(partial.c_str());
        return false;
    }
    return true;
}

bool ensure_one(const ModelFile &file, const std::string &dest_dir,
                const std::string &model_id, CancellableSpawn &spawn, CancellableSpawn::Ticket ticket, std::string &error,
                  const std::function<void()> &on_download)
{
    if (spawn.cancelled(ticket)) { error = "cancelled"; return false; }
    std::string dest = dest_dir + "/" + file.filename;
    std::string hex;
    if (g_file_test(dest.c_str(), G_FILE_TEST_IS_REGULAR)) {
        if (!ModelZoo::file_sha256(dest, hex, error)) return false;
        if (g_ascii_strcasecmp(hex.c_str(), file.sha256.c_str()) == 0) return true;
        g_unlink(dest.c_str());
    }
    if (!download_url(file.url, dest, model_id, spawn, ticket, error, on_download)) return false;
    if (!ModelZoo::file_sha256(dest, hex, error)) return false;
    if (g_ascii_strcasecmp(hex.c_str(), file.sha256.c_str()) != 0) {
        g_unlink(dest.c_str());
        error = "checksum mismatch for " + file.filename;
        return false;
    }
    return true;
}

} /* namespace */

std::string ModelZoo::host_arch()
{
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

bool ModelZoo::file_sha256(const std::string &path, std::string &hex, std::string &error)
{
    GError *gerr = nullptr;
    gchar *contents = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &len, &gerr)) {
        error = gerr ? gerr->message : "could not read file for checksum";
        if (gerr) g_error_free(gerr);
        return false;
    }
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, reinterpret_cast<const guchar *>(contents), len);
    hex = g_checksum_get_string(sum);
    g_checksum_free(sum);
    g_free(contents);
    return true;
}

bool ModelZoo::load_file(const std::string &path, std::string &error)
{
    std::string text = read_file(path, error);
    if (text.empty() && !error.empty()) return false;
    Json root;
    if (!parse_json(text, root, error)) return false;
    const Json *def = root.field("default");
    const Json *models = root.field("models");
    if (!def || def->type != Json::Type::String || !models || models->type != Json::Type::Array) {
        error = "manifest needs a default id and a models array";
        return false;
    }
    std::vector<ModelSpec> parsed;
    std::string arch = host_arch();
    for (const Json &entry : models->arr) {
        if (entry.type != Json::Type::Object) {
            error = "model entry must be an object";
            return false;
        }
        ModelSpec spec;
        const Json *id = entry.field("id");
        const Json *name = entry.field("name");
        const Json *dim = entry.field("dimension");
        const Json *pool = entry.field("pooling");
        const Json *max_tok = entry.field("max_tokens");
        const Json *vocab = entry.field("vocab");
        const Json *model = entry.field("model");
        if (!id || id->type != Json::Type::String || !name || name->type != Json::Type::String ||
            !pool || pool->type != Json::Type::String || !vocab || !model) {
            error = "model entry is missing a required field";
            return false;
        }
        const Json *profile = entry.field("heatmap_profile");
        if (profile && (profile->type != Json::Type::String ||
                        !SemanticHeatmap::profileFromID(profile->str, spec.heatmap_profile))) {
            error = "unknown heatmap profile";
            return false;
        }
        spec.id = id->str;
        spec.name = name->str;
        spec.dimension = dim ? dim->as_int(0) : 0;
        spec.pooling = pool->str;
        spec.max_tokens = max_tok ? max_tok->as_int(256) : 256;
        if (spec.dimension <= 0 || spec.max_tokens < 2) {
            error = "model " + spec.id + " has a bad dimension or max_tokens";
            return false;
        }
        if (spec.pooling != "mean") {
            error = "model " + spec.id + " uses unsupported pooling " + spec.pooling;
            return false;
        }
        if (!parse_file(*vocab, spec.vocab, error)) return false;
        if (model->type != Json::Type::Object) {
            error = "model file map must be an object";
            return false;
        }
        const Json *arch_node = model->field(arch);
        if (!arch_node) arch_node = model->field("all");
        if (!arch_node) {
            /* Skip a model this machine cannot run. The default is
             * required, and an empty list is still an error. */
            if (spec.id == def->str) {
                error = "default model " + spec.id + " has no file for " + arch;
                return false;
            }
            continue;
        }
        if (!parse_file(*arch_node, spec.model, error)) return false;
        parsed.push_back(std::move(spec));
    }
    if (parsed.empty()) {
        error = "manifest lists no models";
        return false;
    }
    bool found_default = false;
    for (const ModelSpec &s : parsed)
        if (s.id == def->str) found_default = true;
    if (!found_default) {
        error = "default model is not in the list";
        return false;
    }
    default_id_ = def->str;
    models_ = std::move(parsed);
    return true;
}

const ModelSpec *ModelZoo::find(const std::string &id) const
{
    for (const ModelSpec &s : models_)
        if (s.id == id) return &s;
    return nullptr;
}

bool ModelZoo::ensure(const ModelSpec &spec, const std::string &dest_dir, std::string &error,
                      CancellableSpawn *spawn, std::optional<CancellableSpawn::Ticket> requested_ticket,
                      const std::function<void()> &on_download) const
{
    CancellableSpawn local;
    if (!spawn) spawn = &local;
    const auto ticket = requested_ticket.value_or(spawn->ticket());
    if (spawn->cancelled(ticket)) { error = "cancelled"; return false; }
    if (g_mkdir_with_parents(dest_dir.c_str(), 0700) != 0) {
        error = "could not create " + dest_dir;
        return false;
    }
    if (!ensure_one(spec.vocab, dest_dir, spec.id, *spawn, ticket, error, on_download)) return false;
    if (!ensure_one(spec.model, dest_dir, spec.id, *spawn, ticket, error, on_download)) return false;
    return true;
}

std::string ModelZoo::download_failure_message(const std::string &model_id, bool curl_missing,
                                                int exit_code)
{
    std::string reason;
    if (curl_missing) reason = "Could not start curl; check that it is installed.";
    else switch (exit_code) {
    case 5: case 6: case 7: reason = "Could not connect; check the network and server address."; break;
    case 22: reason = "The server returned an HTTP error (for example 404 or 403)."; break;
    case 23: reason = "Could not write the download; check disk space and permissions."; break;
    case 28: reason = "The download timed out."; break;
    case 35: case 51: case 60: case 77: reason = "TLS connection or certificate verification failed."; break;
    case 37: reason = "The source file could not be read."; break;
    default: reason = "Download failed (curl exit " + std::to_string(exit_code) + ")."; break;
    }
    return "Model " + model_id + " is not installed. " + reason;
}

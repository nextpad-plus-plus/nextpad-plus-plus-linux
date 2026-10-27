#include "OrtRuntime.h"

#include <string>

const char *npp_ort_missing_message()
{
    return "embeddings disabled: ONNX Runtime not found";
}

#if !NPP_HAVE_ONNXRUNTIME

bool npp_ort_runtime_available()
{
    return false;
}

#else

#include "onnxruntime_c_api.h"

#include <cstdlib>
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#include <vector>

namespace {

#ifndef NPP_ONNXRUNTIME_ROOT
#define NPP_ONNXRUNTIME_ROOT ""
#endif

struct LoadedOrt {
    void *handle = nullptr;
    const OrtApi *api = nullptr;
    std::string error;
};

std::string executable_dir()
{
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string path(buf);
    std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash);
}

void add_names(std::vector<std::string> &out, const std::string &dir)
{
    if (dir.empty()) return;
    out.push_back(dir + "/libonnxruntime.so.1");
    out.push_back(dir + "/libonnxruntime.so");
}

std::vector<std::string> candidate_paths()
{
    std::vector<std::string> out;
    std::string exe = executable_dir();
    if (!exe.empty()) {
        /* Private directory used by the install layout. A bundled
         * runtime is found here without a link-time dependency. */
        add_names(out, exe + "/../lib/nextpad-plus-plus");
    }
    const char *baked = NPP_ONNXRUNTIME_ROOT;
    if (baked && baked[0]) {
        add_names(out, std::string(baked) + "/lib");
        add_names(out, std::string(baked) + "/lib64");
    }
    const char *env = std::getenv("ONNXRUNTIME_ROOT");
    if (env && env[0]) {
        add_names(out, std::string(env) + "/lib");
        add_names(out, std::string(env) + "/lib64");
    }
    /* Last, the loader's default search (ldconfig and LD_LIBRARY_PATH). */
    out.emplace_back("libonnxruntime.so.1");
    out.emplace_back("libonnxruntime.so");
    return out;
}

LoadedOrt load_ort()
{
    LoadedOrt loaded;
    /* RTLD_GLOBAL so a CUDA or OpenVINO provider plugin can resolve
     * symbols in the core library. The handle is kept for the process
     * lifetime; closing it would invalidate every session. */
    const int flags = RTLD_NOW | RTLD_GLOBAL;
    for (const std::string &path : candidate_paths()) {
        void *handle = ::dlopen(path.c_str(), flags);
        if (!handle) continue;
        ::dlerror();
        auto *get_base = reinterpret_cast<const OrtApiBase *(*)()>(
            ::dlsym(handle, "OrtGetApiBase"));
        const char *sym_error = ::dlerror();
        if (sym_error || !get_base) {
            ::dlclose(handle);
            loaded.error = "ONNX Runtime is missing OrtGetApiBase";
            return loaded;
        }
        const OrtApiBase *base = get_base();
        if (!base || !base->GetApi) {
            ::dlclose(handle);
            loaded.error = "ONNX Runtime C API version mismatch";
            return loaded;
        }
        const OrtApi *api = base->GetApi(ORT_API_VERSION);
        if (!api) {
            ::dlclose(handle);
            loaded.error = "ONNX Runtime C API version mismatch";
            return loaded;
        }
        loaded.handle = handle;
        loaded.api = api;
        return loaded;
    }
    loaded.error = npp_ort_missing_message();
    return loaded;
}

LoadedOrt &ort_state()
{
    static LoadedOrt state = load_ort();
    return state;
}

} /* namespace */

bool npp_ort_runtime_available()
{
    return ort_state().api != nullptr;
}

const OrtApi *npp_ort_api(std::string &error)
{
    LoadedOrt &state = ort_state();
    error = state.error;
    return state.api;
}

#endif

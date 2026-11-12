/*
 * Starts the semantic engine and reports whether ONNX Runtime loaded.
 * The process must exit 0 in either case. "disabled" means the shared
 * library is absent and embeddings report that in the same words the
 * search bar uses.
 */
#include "ExecutionProviders.h"
#include "OrtRuntime.h"

#include <iostream>
#include <string>

int main()
{
    std::string error;
    if (!npp_ort_runtime_available()) {
        OnnxProviderConfig cfg;
        cfg.pooling = "mean";
        cfg.dimension = 384;
        cfg.max_tokens = 256;
        auto provider = make_onnx_provider(cfg, error);
        if (provider || error.find(npp_ort_missing_message()) == std::string::npos) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "disabled\n";
        return 0;
    }
    std::cout << "loaded\n";
    return 0;
}

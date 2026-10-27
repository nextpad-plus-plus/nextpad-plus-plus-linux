/*
 * OrtRuntime.h
 *
 * ONNX Runtime is optional on the machine that runs the editor. The
 * semantic engine loads libonnxruntime with dlopen the first time an
 * embedding session is needed, and looks up OrtGetApiBase from that
 * handle. The editor has no link dependency on the library, so it
 * still starts where the library is not installed.
 */
#ifndef NPP_ORT_RUNTIME_H
#define NPP_ORT_RUNTIME_H

#include <string>

/* False when this build has no loader, or the shared library could
 * not be opened, or its C API does not match the headers we compile. */
bool npp_ort_runtime_available();

/* Shown in the search bar when embeddings cannot be used because the
 * shared library is not installed. */
const char *npp_ort_missing_message();

#if NPP_HAVE_ONNXRUNTIME
struct OrtApi;
/* Loads the runtime once. On failure, error is a short status string
 * and the result is null. */
const OrtApi *npp_ort_api(std::string &error);
#endif

#endif

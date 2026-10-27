#!/usr/bin/env bash
# Usage: setup-semantic-runtime.sh [prefix] [auto|cpu|cuda|openvino]
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
prefix=${1:-"$root/.cache/onnxruntime"}
backend=$(python3 "$root/tools/detect-semantic-runtime.py" "${2:-auto}")
# Respect an explicitly supplied SDK. Jetson needs kernels built for its SM;
# generic ARM64 PyPI wheels may target SBSA GPUs and fail on Orin (sm_87).
existing=${ONNXRUNTIME_ROOT:-}
if [[ -z "$existing" && "$backend" == jetson ]]; then
    for candidate in /opt/onnxruntime /usr/local/onnxruntime "$HOME/.local/onnxruntime" "$HOME"/ort-*; do
        if [[ -f "$candidate/lib/libonnxruntime.so" || -f "$candidate/lib/libonnxruntime.so.1" ]] &&
           [[ -f "$candidate/lib/libonnxruntime_providers_cuda.so" ]]; then
            existing=$candidate
            break
        fi
    done
    if [[ -z "$existing" ]]; then
        echo 'Jetson requires a matching ONNX Runtime SDK built for its GPU (Orin: CMAKE_CUDA_ARCHITECTURES=87). Set ONNXRUNTIME_ROOT to that SDK.' >&2
        exit 1
    fi
fi
if [[ -n "$existing" ]]; then
    [[ -f "$existing/lib/libonnxruntime.so" || -f "$existing/lib/libonnxruntime.so.1" ]] || {
        echo 'ONNXRUNTIME_ROOT must contain lib/libonnxruntime.so or lib/libonnxruntime.so.1' >&2
        exit 1
    }
    existing=$(cd -- "$existing" && pwd)
    mkdir -p "$prefix"
    prefix=$(cd -- "$prefix" && pwd)
    [[ "$existing" != "$prefix/current" ]] || { echo "Using $existing"; exit 0; }
    ln -sfn "$existing" "$prefix/current"
    printf 'Using installed %s SDK: %s\n' "$backend" "$existing"
    printf 'ONNX Runtime is loaded at run time from %s (no link dependency). Export ONNXRUNTIME_ROOT="%s/current" or pass -DONNXRUNTIME_ROOT="%s/current" to CMake.\n' "$existing" "$prefix" "$prefix"
    exit 0
fi
case "$backend" in
    cuda12) package=onnxruntime-gpu; version=1.24.1 ;;
    cuda13) package=onnxruntime-gpu; version=1.31.0 ;;
    openvino) package=onnxruntime-openvino; version=1.24.1 ;;
    cpu) package=onnxruntime; version=1.24.1 ;;
esac
# A backend-specific directory prevents overlapping ONNX Python packages and
# stale providers when this prefix is reused with another backend.
mkdir -p "$prefix"
prefix=$(cd -- "$prefix" && pwd)
package_dir="$prefix/$backend-$version"
if command -v uv >/dev/null; then
    uv venv --python 3.12 --allow-existing "$package_dir/venv"
    uv pip install --python "$package_dir/venv/bin/python" "$package==$version"
    runtime=$("$package_dir/venv/bin/python" -c 'import pathlib, onnxruntime; print(pathlib.Path(onnxruntime.__file__).parent / "capi")')
else
    # --target does not alter system Python, including on Jetson without venv.
    python3 -m pip install --upgrade --target "$package_dir/python" "$package==$version"
    runtime="$package_dir/python/onnxruntime/capi"
fi
mkdir -p "$package_dir/include" "$package_dir/lib"
for header in onnxruntime_c_api.h onnxruntime_ep_c_api.h; do
    curl -fLsS "https://raw.githubusercontent.com/microsoft/onnxruntime/v$version/include/onnxruntime/core/session/$header" -o "$package_dir/include/$header"
done
# Keep EP plugins and their dependencies beside the runtime that dlopen loads.
for library in "$runtime"/*.so*; do
    ln -sfn "$library" "$package_dir/lib/$(basename "$library")"
done
ln -sfn "$runtime/libonnxruntime.so.$version" "$package_dir/lib/libonnxruntime.so"
ln -sfn "$runtime/libonnxruntime.so.$version" "$package_dir/lib/libonnxruntime.so.1"
# Switch only our managed link; preserve any older include/lib directories.
ln -sfn "$package_dir" "$prefix/current"
printf 'Selected %s (%s %s).\n' "$backend" "$package" "$version"
printf 'ONNX Runtime is loaded at run time from %s/current (no link dependency). Export ONNXRUNTIME_ROOT="%s/current" or pass -DONNXRUNTIME_ROOT="%s/current" to CMake.\n' "$prefix" "$prefix" "$prefix"

#!/usr/bin/env python3
"""Select a runtime build using usable CUDA libraries or Intel render devices.

This is installation-time selection. The C++ provider still creates a session
and runs inference before considering a GPU usable. nvcc is not required.
"""
import ctypes
import glob
import pathlib
import platform
import sys


def cuda_major():
    for major in (13, 12):
        try:
            runtime = ctypes.CDLL(f"libcudart.so.{major}")
            count = ctypes.c_int()
            if runtime.cudaGetDeviceCount(ctypes.byref(count)) != 0 or count.value < 1:
                continue
            ctypes.CDLL(f"libcublas.so.{major}")
            ctypes.CDLL("libcudnn.so.9")
            return major
        except (OSError, AttributeError):
            continue
    return None


def intel_gpu():
    for vendor in glob.glob("/sys/class/drm/renderD*/device/vendor"):
        try:
            if pathlib.Path(vendor).read_text().strip() == "0x8086":
                return True
        except OSError:
            pass
    return False


def select(requested="auto"):
    if requested == "cpu":
        return "cpu"
    if requested not in ("auto", "cuda", "openvino"):
        raise ValueError("backend must be auto, cpu, cuda, or openvino")
    if requested != "openvino":
        major = cuda_major()
        if major and platform.machine() == "aarch64" and pathlib.Path("/etc/nv_tegra_release").exists():
            return "jetson"
        if major and (platform.machine() == "x86_64" or major == 13):
            return f"cuda{major}"
        if requested == "cuda":
            raise ValueError("CUDA needs a working device, CUDA 12/13 and cuDNN 9; "
                             "ARM64 CUDA 12 requires a JetPack-compatible ONNXRUNTIME_ROOT")
    if platform.machine() == "x86_64" and intel_gpu():
        return "openvino"
    if requested == "openvino":
        raise ValueError("OpenVINO setup requires an Intel GPU on x86_64")
    return "cpu"


if __name__ == "__main__":
    try:
        print(select(sys.argv[1] if len(sys.argv) > 1 else "auto"))
    except ValueError as error:
        sys.exit(str(error))

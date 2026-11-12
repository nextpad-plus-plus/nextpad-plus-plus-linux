"""Selection tests: driver/toolkit presence alone must not select an unusable EP."""
import importlib.util
from pathlib import Path
import sys
sys.dont_write_bytecode = True
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "detect_runtime", Path(__file__).parents[2] / "tools/detect-semantic-runtime.py")
detect = importlib.util.module_from_spec(spec)
spec.loader.exec_module(detect)


class RuntimeSelection(unittest.TestCase):
    def select(self, requested="auto", arch="x86_64", cuda=None, intel=False, jetson=False):
        with patch.object(detect.platform, "machine", return_value=arch), \
             patch.object(detect, "cuda_major", return_value=cuda), \
             patch.object(detect, "intel_gpu", return_value=intel), \
             patch.object(detect.pathlib.Path, "exists", return_value=jetson):
            return detect.select(requested)

    def test_nvidia_precedes_intel(self):
        self.assertEqual(self.select(cuda=12, intel=True), "cuda12")

    def test_cpu_does_not_require_cuda(self):
        self.assertEqual(self.select("cpu", cuda=13, intel=True), "cpu")

    def test_intel_and_cpu_fallback(self):
        self.assertEqual(self.select(intel=True), "openvino")
        self.assertEqual(self.select(), "cpu")

    def test_explicit_missing_gpu_fails_setup(self):
        for backend in ("cuda", "openvino"):
            with self.assertRaises(ValueError):
                self.select(backend)

    def test_arm_cuda_build_selection(self):
        self.assertEqual(self.select(arch="aarch64", cuda=13), "cuda13")
        self.assertEqual(self.select(arch="aarch64", cuda=12), "cpu")
        with self.assertRaises(ValueError):
            self.select("cuda", arch="aarch64", cuda=12)

    def test_jetson_uses_installed_sdk_not_sbsa_wheel(self):
        self.assertEqual(self.select(arch="aarch64", cuda=13, jetson=True), "jetson")
        self.assertEqual(self.select(arch="aarch64", cuda=12, jetson=True), "jetson")

    def test_no_other_gpu_families(self):
        for backend in ("migraphx", "webgpu", "metal"):
            with self.assertRaises(ValueError):
                self.select(backend)


if __name__ == "__main__":
    unittest.main()

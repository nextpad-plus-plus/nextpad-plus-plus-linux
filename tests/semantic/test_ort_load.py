#!/usr/bin/env python3
"""The semantic binaries must not list libonnxruntime as NEEDED, and the
probe must start with that library absent."""

import os
import subprocess
import sys


def assert_no_ort(path):
    readelf = subprocess.run(
        ["readelf", "-d", path], check=True, capture_output=True, text=True)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = ""
    env.pop("LD_PRELOAD", None)
    ldd = subprocess.run(
        ["ldd", path], check=True, capture_output=True, text=True, env=env)
    blob = readelf.stdout + "\n" + ldd.stdout
    if "libonnxruntime" in blob:
        sys.stderr.write(blob)
        raise SystemExit("%s depends on libonnxruntime" % path)


def system_has_ort():
    try:
        listed = subprocess.run(
            ["ldconfig", "-p"], check=False, capture_output=True, text=True)
    except OSError:
        return False
    return "libonnxruntime" in (listed.stdout or "")


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: test_ort_load.py PROBE [BINARY ...]")
    probe = sys.argv[1]
    for path in sys.argv[1:]:
        assert_no_ort(path)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = ""
    env.pop("LD_PRELOAD", None)
    env.pop("ONNXRUNTIME_ROOT", None)
    proc = subprocess.run(
        [probe], check=False, capture_output=True, text=True, env=env, timeout=20)
    line = proc.stdout.strip()
    if proc.returncode != 0 or line not in ("disabled", "loaded"):
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit("probe exit %s output %r" % (proc.returncode, line))
    if not system_has_ort() and line != "disabled":
        raise SystemExit("library is not installed but probe said %r" % line)
    print("ort probe: %s" % line)


if __name__ == "__main__":
    main()

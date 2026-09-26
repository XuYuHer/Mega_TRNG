#!/usr/bin/env python3
"""Run host protocol tests and real TRNG.cpp against deterministic ADC stubs."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "test", "-v"],
                   cwd=ROOT, check=True)
    compiler = shutil.which(os.environ.get("CXX", "g++"))
    if not compiler:
        raise RuntimeError("native TRNG checks require g++ (or set CXX)")
    folder = ROOT / ".pio" / "native-tests"
    folder.mkdir(parents=True, exist_ok=True)
    for wide in (0, 1):
        exe = folder / f"trng-{wide}{'.exe' if os.name == 'nt' else ''}"
        subprocess.run([
            compiler, "-std=c++11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-D__AVR_ATmega2560__", f"-DMEGATRNG_ENABLE_WIDE_PLANES={wide}",
            "-Itest/native/fake_avr", "-Ilib/MegaTRNG/src",
            "test/native/fake_registers.cpp", "test/native/test_trng.cpp",
            "lib/MegaTRNG/src/TRNG.cpp", "-o", str(exe),
        ], cwd=ROOT, check=True)
        subprocess.run([str(exe)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()

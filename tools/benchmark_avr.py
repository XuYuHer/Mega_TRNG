#!/usr/bin/env python3
"""Compare AVR instruction costs with deterministic ADC stubs, NOT board speed."""
import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    result = subprocess.run([str(a) for a in args], cwd=ROOT, text=True,
                            capture_output=True, timeout=60, errors="replace")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", default="11cb990")
    parser.add_argument("--toolchain", type=Path,
                        default=Path.home() / ".platformio/packages/toolchain-atmelavr/bin")
    parser.add_argument("--json", type=Path, default=Path("artifacts/avr_benchmark.json"))
    args = parser.parse_args()
    folder = ROOT / ".pio/avr-benchmark"
    baseline = folder / "baseline"
    baseline.mkdir(parents=True, exist_ok=True)
    for filename in ("TRNG.cpp", "TRNG.h"):
        content = run(["git", "show", f"{args.baseline_ref}:lib/MegaTRNG/src/{filename}"])
        (baseline / filename).write_text(content, encoding="utf-8")
    compiler = shutil.which("avr-g++", path=str(args.toolchain))
    debugger = shutil.which("avr-gdb", path=str(args.toolchain))
    if not compiler or not debugger:
        raise RuntimeError("avr-g++ and avr-gdb required; set --toolchain to their folder")
    result = {"baseline_ref": args.baseline_ref, "compiler": run([compiler, "--version"]).splitlines()[0],
              "scope": "AVR simulator, 256 bytes, deterministic ADC stub; no conversion wait/UART/interrupts",
              "flags": "-Os -flto; Timer1/WDT compiled out; wide planes compiled in", "modes": {}}
    for mode in ("lsb", "four_planes"):
        metrics = {}
        for label, src in (("baseline", baseline), ("working", ROOT / "lib/MegaTRNG/src")):
            elf = folder / f"{mode}-{label}.elf"
            command = [compiler, "-std=c++11", "-mmcu=atmega2560", "-Os", "-flto", "-g",
                       "-DMEGATRNG_ENABLE_WATCHDOG=0", "-DMEGATRNG_ENABLE_TIMER1_PHASE=0",
                       "-Itest/native/fake_avr", f"-I{src}",
                       "test/native/fake_registers.cpp", "test/native/avr_benchmark.cpp",
                       src / "TRNG.cpp", "-o", elf]
            if mode == "four_planes":
                command.insert(6, "-DBENCHMARK_WIDE")
            run(command)
            command = [debugger, "--batch", elf]
            for expression in ("target sim", "load", "sim verbose", "break benchmarkDone", "run",
                               "print benchmarkBytes", "print benchmarkRaw", "print benchmarkAccepted",
                               "print benchmarkChecksum", "info target", "quit"):
                command.extend(("-ex", expression))
            log = run(command)
            (folder / f"{mode}-{label}.log").write_text(log, encoding="utf-8")
            values = re.findall(r"\$\d+ = (\d+)", log)
            cycles = re.search(r"# cycles\s+(\d+)", log)
            if len(values) != 4 or cycles is None:
                raise RuntimeError(f"unrecognised simulator result; inspect {folder}")
            count, raw, accepted, checksum = map(int, values)
            if count != 256:
                raise RuntimeError(f"incomplete simulation: only {count} bytes")
            metrics[label] = dict(bytes=count, raw_samples=raw, accepted_bits=accepted,
                                  checksum=checksum, cycles=int(cycles.group(1)))
        for field in ("bytes", "raw_samples", "accepted_bits", "checksum"):
            if metrics["baseline"][field] != metrics["working"][field]:
                raise RuntimeError(f"{mode}: comparison not equivalent ({field})")
        reduction = 100 * (1 - metrics["working"]["cycles"] / metrics["baseline"]["cycles"])
        metrics["cycle_reduction_percent"] = reduction
        result["modes"][mode] = metrics
        print(f"{mode}: {metrics['baseline']['cycles']:,} -> {metrics['working']['cycles']:,} "
              f"simulated cycles, {reduction:.2f}% lower; counters/output checksum match")
    output = args.json if args.json.is_absolute() else ROOT / args.json
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("These are software cost comparisons, not Mega ADC throughput or entropy measurements.")


if __name__ == "__main__":
    main()

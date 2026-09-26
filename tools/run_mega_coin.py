#!/usr/bin/env python3
"""One-command, real Mega2560 coin experiment.

The command detects a USB Mega/CH340 port, optionally uploads the collector
firmware, then delegates to coin_lln.py for framed collection and analysis.
It never falls back to a computer random source.

Typical offline use from the repository root:
    python tools/run_mega_coin.py --flips 1000000

If the board is already flashed with megaatmega2560_coin:
    python tools/run_mega_coin.py --no-upload --port COM9 --flips 1000000
"""
from __future__ import annotations

import argparse
import datetime as _datetime
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]
USB_VID_PID = {
    (0x1A86, 0x7523),  # CH340/CH341 clone commonly fitted to Mega boards
    (0x2341, 0x0010),  # Arduino Mega 2560
    (0x2341, 0x0042),
    (0x2341, 0x0043),
    (0x2341, 0x0044),
    (0x2A03, 0x0010),
    (0x2A03, 0x0042),
}


def _ports():
    try:
        import serial.tools.list_ports  # type: ignore
    except ImportError as exc:
        raise RuntimeError("install pyserial first: python -m pip install pyserial") from exc
    return list(serial.tools.list_ports.comports())


def find_board_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    candidates = []
    for port in _ports():
        text = " ".join(
            str(value or "")
            for value in (getattr(port, "description", None),
                          getattr(port, "manufacturer", None),
                          getattr(port, "hwid", None),
                          getattr(port, "product", None))
        ).lower()
        if "bluetooth" in text or "bthenum" in text:
            continue
        if (port.vid, port.pid) in USB_VID_PID or any(
            keyword in text for keyword in ("ch340", "ch341", "arduino mega", "mega 2560")
        ):
            candidates.append(port)
    if len(candidates) == 1:
        return candidates[0].device
    if not candidates:
        visible = ", ".join(getattr(port, "device", "?") for port in _ports()) or "none"
        raise RuntimeError(
            "no USB Mega/CH340 port found; connect the board and check the cable/driver "
            f"(visible ports: {visible})"
        )
    details = ", ".join(f"{port.device} ({port.description})" for port in candidates)
    raise RuntimeError(f"more than one possible Mega port; pass --port explicitly: {details}")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="Mega port; auto-detected when omitted")
    parser.add_argument("--flips", type=int, default=1_000_000,
                        help="number of real board output bits to analyse")
    parser.add_argument("--mode", choices=("conservative", "fast", "turbo"), default="fast")
    parser.add_argument("--baud", type=int, default=1_000_000,
                        help="must match the collector firmware")
    parser.add_argument("--timeout", type=float, default=10,
                        help="seconds without a complete frame")
    parser.add_argument("--boot-delay", type=float, default=2,
                        help="seconds to wait after opening the port")
    parser.add_argument("--no-upload", action="store_true",
                        help="do not flash; use an already uploaded collector firmware")
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts"))
    parser.add_argument("--name", help="output name prefix; default includes the timestamp")
    parser.add_argument("--no-plot", action="store_true",
                        help="skip PNG generation (CSV, JSON and raw frame capture remain)")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if args.flips < 1 or args.baud < 1 or args.timeout <= 0 or args.boot_delay < 0:
        _parser().error("flips/baud/timeout must be positive and boot-delay non-negative")
    try:
        port = find_board_port(args.port)
        print(f"Using Mega port: {port}", flush=True)
        if not args.no_upload:
            pio = shutil.which("pio")
            if pio is None:
                raise RuntimeError("PlatformIO command `pio` was not found; install PlatformIO first")
            print("Uploading real-board collector firmware...", flush=True)
            result = subprocess.run(
                [pio, "run", "-e", "megaatmega2560_coin", "-t", "upload", "--upload-port", port],
                cwd=ROOT,
            )
            if result.returncode != 0:
                return result.returncode

        stamp = _datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        prefix = args.name or f"mega_{args.mode}_{stamp}"
        output_dir = args.output_dir if args.output_dir.is_absolute() else ROOT / args.output_dir
        base = output_dir / prefix
        command = [
            sys.executable,
            str(ROOT / "tools" / "coin_lln.py"),
            "--serial", port,
            "--mode", args.mode,
            "--flips", str(args.flips),
            "--baud", str(args.baud),
            "--timeout", str(args.timeout),
            "--boot-delay", str(args.boot_delay),
            "--capture", str(base.with_suffix(".mlln")),
            "--csv", str(base.with_suffix(".csv")),
            "--json", str(base.with_suffix(".json")),
        ]
        if not args.no_plot and importlib.util.find_spec("matplotlib") is not None:
            command.extend(("--plot", str(base.with_suffix(".png"))))
        elif not args.no_plot:
            print("matplotlib is not installed; continuing without PNG (CSV/JSON/capture stay enabled).")
        # This command contains --serial unconditionally. There is no
        # --simulate branch in the automatic real-board workflow.
        return subprocess.run(command, cwd=ROOT).returncode
    except (OSError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

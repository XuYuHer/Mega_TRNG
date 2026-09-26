#!/usr/bin/env python3
"""Coin toss experiment: computer simulation, Mega serial capture, or replay.

Python 3.10+. No dependencies for simulation/CSV/JSON.
Serial: pyserial. PNG plot: matplotlib. See docs/COIN_LLN.md.
"""
from __future__ import annotations

import argparse
import binascii
import csv
import json
import math
import os
import random
import struct
import sys
import time
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator, Sequence

MAGIC = b"MLLN"
HEADER = struct.Struct("<4sBBHI")
METADATA = struct.Struct("<IIIBB")
PROTOCOL_VERSION = 1
PAYLOAD_BYTES = 240
FLAG_ERROR = 1
MODE_COMMANDS = {"conservative": b"C", "fast": b"F", "turbo": b"T"}
MODE_NAMES = tuple(MODE_COMMANDS)
ERRORS = {1: "startup health/configuration failure", 2: "ADC health failure",
          3: "no full byte within 4096 ADC samples"}


class ProtocolError(RuntimeError):
    pass


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff, no reflection/xorout."""
    return binascii.crc_hqx(data, 0xFFFF)


@dataclass(frozen=True)
class Frame:
    flags: int
    sequence: int
    payload: bytes


def frame_bytes(payload: bytes, sequence: int = 0, flags: int = 0) -> bytes:
    header = HEADER.pack(MAGIC, PROTOCOL_VERSION, flags, len(payload), sequence)
    return header + payload + struct.pack("<H", crc16_ccitt(header[4:] + payload))


class FrameReader:
    """Handle arbitrarily split serial reads. Never silently discard bad data."""

    def __init__(self, stream: BinaryIO, *, live: bool = False, timeout: float = 10):
        self.stream = stream
        self.live = live
        self.timeout = timeout
        self.buffer = bytearray()
        self.deadline = time.monotonic() + timeout
        self.resync_bytes = 0
        self.last_sequence: int | None = None

    def _ensure(self, size: int) -> bool:
        while len(self.buffer) < size:
            if self.live and time.monotonic() >= self.deadline:
                raise ProtocolError("serial timeout: no complete frame; check firmware, baud and A0")
            count = min(4096, max(1, getattr(self.stream, "in_waiting", 256))) if self.live else 4096
            chunk = self.stream.read(count)
            if chunk:
                self.buffer.extend(chunk)
            elif not self.live:
                return False
        return True

    def frames(self) -> Iterator[Frame]:
        while True:
            if not self._ensure(4):
                if self.buffer:
                    raise ProtocolError("truncated magic/header at end of capture")
                return
            if self.buffer[:4] != MAGIC:
                if self.last_sequence is not None:
                    raise ProtocolError("lost frame boundary after valid data")
                # Allow startup noise or joining a stream mid-frame, only
                # before the first valid frame. Bound this even for a file.
                marker = self.buffer.find(MAGIC, 1)
                discarded = marker if marker >= 0 else len(self.buffer) - 3
                del self.buffer[:discarded]
                self.resync_bytes += discarded
                if self.resync_bytes > 1_048_576:
                    raise ProtocolError("no MLLN header within 1 MiB")
                continue
            if not self._ensure(HEADER.size):
                raise ProtocolError("truncated frame header")
            _, version, flags, length, sequence = HEADER.unpack_from(self.buffer)
            if version != PROTOCOL_VERSION or flags not in (0, FLAG_ERROR):
                raise ProtocolError(f"unsupported protocol version/flags: {version}/{flags}")
            expected_length = 1 if flags == FLAG_ERROR else METADATA.size + PAYLOAD_BYTES
            if length != expected_length:
                raise ProtocolError(f"invalid payload length {length}; expected {expected_length}")
            size = HEADER.size + length + 2
            if not self._ensure(size):
                raise ProtocolError("truncated frame payload/CRC")
            crc = struct.unpack_from("<H", self.buffer, size - 2)[0]
            if crc != crc16_ccitt(bytes(self.buffer[4:size - 2])):
                raise ProtocolError(f"CRC mismatch in frame {sequence}")
            if self.last_sequence is not None and sequence != ((self.last_sequence + 1) & 0xFFFFFFFF):
                raise ProtocolError(f"lost/duplicate frame: sequence {self.last_sequence} -> {sequence}")
            payload = bytes(self.buffer[HEADER.size:size - 2])
            del self.buffer[:size]
            self.last_sequence = sequence
            self.deadline = time.monotonic() + self.timeout
            yield Frame(flags, sequence, payload)


class CollectorSource:
    def __init__(self, reader: FrameReader, capture: BinaryIO | None = None,
                 expected_mode: str | None = None):
        self.reader = reader
        self.capture = capture
        self.expected_mode = expected_mode
        self.frames = self.payload_bytes = self.generation_us = 0
        self.raw_samples = self.accepted_bits = 0
        self.mode: str | None = None
        self.channel: int | None = None

    def chunks(self) -> Iterator[bytes]:
        for frame in self.reader.frames():
            # Save whole validated frames, including a board error frame.
            if self.capture is not None:
                self.capture.write(frame_bytes(frame.payload, frame.sequence, frame.flags))
            if frame.flags == FLAG_ERROR:
                raise ProtocolError(f"Mega: {ERRORS.get(frame.payload[0], 'unknown error')}")
            elapsed, raw, accepted, mode, channel = METADATA.unpack_from(frame.payload)
            if mode >= len(MODE_NAMES) or channel > 15 or elapsed == 0:
                raise ProtocolError("invalid collector timing/configuration metadata")
            name = MODE_NAMES[mode]
            if self.expected_mode is not None and name != self.expected_mode:
                raise ProtocolError(f"requested {self.expected_mode}, collector sent {name}")
            if self.mode is not None and (name != self.mode or channel != self.channel):
                raise ProtocolError("collector configuration changed during the run")
            self.mode, self.channel = name, channel
            self.frames += 1
            self.payload_bytes += PAYLOAD_BYTES
            self.generation_us += elapsed
            self.raw_samples += raw
            self.accepted_bits += accepted
            yield frame.payload[METADATA.size:]

    def statistics(self, wall_seconds: float, *, replay: bool = False) -> dict:
        return {
            "mode": self.mode, "adc_channel": self.channel, "frames": self.frames,
            "received_random_bytes": self.payload_bytes, "board_generation_us": self.generation_us,
            "board_us_per_byte": self.generation_us / self.payload_bytes if self.payload_bytes else None,
            "board_bytes_per_second": self.payload_bytes * 1e6 / self.generation_us if self.generation_us else None,
            "raw_samples": self.raw_samples, "accepted_bits": self.accepted_bits,
            # File playback is not a measurement of the USB/serial transport.
            "host_bytes_per_second": None if replay else self.payload_bytes / max(wall_seconds, 1e-9),
            "startup_bytes_skipped": self.reader.resync_bytes,
        }


def iter_simulated_bytes(total_bytes: int, seed: int | None, chunk_bytes: int = 65536) -> Iterator[bytes]:
    generator = random.Random(seed) if seed is not None else None
    while total_bytes:
        amount = min(total_bytes, chunk_bytes)
        # Generate the full seeded chunk even at the end: a different requested
        # flip count must not change earlier bits in the same seeded sequence.
        yield generator.randbytes(chunk_bytes)[:amount] if generator else os.urandom(amount)
        total_bytes -= amount


def checkpoint_sizes(total_bits: int) -> list[int]:
    sizes, n = [], 1
    while n < total_bits:
        sizes.append(n)
        n *= 10
    return sizes + [total_bits]


def curve_targets(total_bits: int, stride: int | None = None) -> list[int]:
    targets = set(checkpoint_sizes(total_bits))
    if stride is not None:
        if stride < 1:
            raise ValueError("--stride must be positive")
        if total_bits // stride > 1_000_000:
            raise ValueError("--stride would create over one million rows; use a larger stride")
        targets.update(range(stride, total_bits + 1, stride))
    else:
        # Show early fluctuations on a log axis, plus detail in the long tail.
        for i in range(1001):
            targets.add(max(1, min(total_bits, round(total_bits ** (i / 1000)))))
        targets.update(range(max(1, total_bits // 1000), total_bits + 1, max(1, total_bits // 1000)))
    return sorted(targets)


def count_bits(data: memoryview, offset: int, length: int) -> int:
    """Count any LSB-first bit slice using C-level popcount for full bytes."""
    count = 0
    if offset % 8 and length:
        take = min(8 - offset % 8, length)
        count += ((data[offset // 8] >> (offset % 8)) & ((1 << take) - 1)).bit_count()
        offset += take
        length -= take
    whole_bytes, tail = divmod(length, 8)
    start = offset // 8
    count += int.from_bytes(data[start:start + whole_bytes], "little").bit_count()
    if tail:
        count += (data[start + whole_bytes] & ((1 << tail) - 1)).bit_count()
    return count


def analyze_chunks(chunks: Iterable[bytes], total_bits: int,
                   point_stride: int | None = None) -> tuple[list[dict], dict]:
    if total_bits < 1:
        raise ValueError("--flips must be at least 1")
    targets = iter(curve_targets(total_bits, point_stride))
    target = next(targets)
    rows: list[dict] = []
    seen = heads = 0
    for chunk in chunks:
        data = memoryview(chunk)
        offset = 0
        while offset < len(data) * 8 and seen < total_bits:
            take = min(target - seen, len(data) * 8 - offset)
            heads += count_bits(data, offset, take)
            seen += take
            offset += take
            if seen == target:
                rows.append({"flips": seen, "heads": heads, "proportion": heads / seen,
                             "error": heads / seen - 0.5})
                target = next(targets, total_bits)
        if seen == total_bits:
            break
    if seen != total_bits:
        raise ValueError(f"input ended after {seen} flips; need {total_bits}")
    error = heads / total_bits - 0.5
    summary = {
        "flips": total_bits, "heads": heads, "tails": total_bits - heads,
        "proportion": heads / total_bits, "error": error, "absolute_error": abs(error),
        "fair_coin_95_band_half_width": 1.96 * math.sqrt(0.25 / total_bits),
        "z_score": error / math.sqrt(0.25 / total_bits), "curve_points": len(rows),
    }
    return rows, summary


def print_report(rows: Sequence[dict], summary: dict) -> None:
    print(f"source      : {summary['source']}")
    print(f"flips       : {summary['flips']:,}")
    print(f"heads/tails : {summary['heads']:,} / {summary['tails']:,}")
    print(f"heads ratio : {summary['proportion']:.8f}")
    print(f"error       : {summary['error']:+.8f}")
    print(f"fair 95% band: +/-{summary['fair_coin_95_band_half_width']:.8f} (pointwise normal approximation)")
    print("checkpoints :")
    checkpoints = set(checkpoint_sizes(summary["flips"]))
    for row in rows:
        if row["flips"] in checkpoints:
            print(f"  n={row['flips']:<12d} p={row['proportion']:.8f} error={row['error']:+.8f}")
    metrics = summary.get("collector")
    if metrics:
        print(f"board source: {metrics['mode']}, A{metrics['adc_channel']}")
        print(f"generation  : {metrics['board_bytes_per_second']:.2f} bytes/s, "
              f"{metrics['board_us_per_byte']:.2f} us/byte (excludes framing/UART)")
        if metrics["host_bytes_per_second"] is not None:
            print(f"received    : {metrics['host_bytes_per_second']:.2f} random bytes/s (includes transfer/counting)")
        print(f"ADC samples : {metrics['raw_samples']:,}; accepted VN bits: {metrics['accepted_bits']:,}")
    print("Finite-sample illustration, assuming independent fair flips; error need not decrease at every step.")


def write_csv(path: Path, rows: Sequence[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("flips", "heads", "proportion", "error"))
        writer.writeheader()
        writer.writerows(rows)


def write_plot(path: Path, rows: Sequence[dict], summary: dict) -> None:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise RuntimeError("--plot requires: python -m pip install matplotlib") from exc
    x = [row["flips"] for row in rows]
    y = [row["proportion"] for row in rows]
    band = [1.96 * math.sqrt(0.25 / n) for n in x]
    figure, (axis, errors) = plt.subplots(2, 1, figsize=(10, 7.5), sharex=True)
    axis.fill_between(x, [max(0, 0.5 - b) for b in band], [min(1, 0.5 + b) for b in band],
                      color="#c5ddf1", alpha=0.6, label="95% pointwise reference (approx., independent fair flips)")
    axis.plot(x, y, color="#2166ac", linewidth=1.4, label="cumulative heads proportion")
    axis.axhline(0.5, color="#555555", linestyle="--", linewidth=1, label="expected 0.5")
    axis.set_ylim(-0.02, 1.02)
    axis.set_ylabel("Heads / flips")
    axis.set_title(f"Coin toss and the law of large numbers\n"
                   f"{summary['source']} | n={summary['flips']:,} | final p={summary['proportion']:.6f}")
    axis.legend(fontsize=8, loc="upper right")
    # Zero deviations are omitted from the log plot, rather than invented.
    errors.plot(x, [abs(row["error"]) or float("nan") for row in rows],
                color="#b35806", linewidth=1, label="absolute error (zero errors omitted)")
    errors.plot(x, band, color="#555555", linestyle="--", label="1.96 x sqrt(0.25 / n)")
    errors.set_yscale("log")
    errors.set_xscale("log")
    errors.set_ylabel("|heads / flips - 0.5|")
    errors.set_xlabel("Number of flips (log scale)")
    errors.legend(fontsize=8)
    for subplot in (axis, errors):
        subplot.grid(True, alpha=0.2)
    figure.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=160)
    plt.close(figure)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--simulate", action="store_true", help="computer only (default)")
    source.add_argument("--serial", metavar="PORT", help="Mega USB serial port, e.g. COM7")
    source.add_argument("--input", type=Path, help="replay a validated .mlln capture")
    source.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    parser.add_argument("--flips", type=int, default=1_000_000)
    parser.add_argument("--seed", type=int, help="reproducible PC simulation only")
    parser.add_argument("--mode", choices=MODE_NAMES, default="fast", help="Mega sampling mode")
    parser.add_argument("--baud", type=int, default=1_000_000, help="must match collector firmware")
    parser.add_argument("--timeout", type=float, default=10, help="seconds without a valid frame")
    parser.add_argument("--boot-delay", type=float, default=2, help="wait for Mega auto-reset after open")
    parser.add_argument("--capture", type=Path, help="save complete frames from --serial for replay")
    parser.add_argument("--csv", type=Path)
    parser.add_argument("--json", dest="json_path", type=Path)
    parser.add_argument("--plot", type=Path, help="save PNG (matplotlib)")
    parser.add_argument("--stride", type=int, help="record every N flips plus powers of ten")
    return parser


def stop_collection(device) -> None:
    try:
        device.write(b"S")
        device.flush()
    except OSError:
        pass


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if (args.flips < 1 or args.baud < 1 or args.timeout <= 0 or args.boot_delay < 0
            or not math.isfinite(args.timeout) or not math.isfinite(args.boot_delay)):
        parser.error("flips/baud/timeout must be positive; boot-delay must be non-negative")
    if args.stride is not None and args.stride < 1:
        parser.error("--stride must be positive")
    if args.seed is not None and (args.serial or args.input):
        parser.error("--seed is only available for computer simulation")
    if args.capture and not args.serial:
        parser.error("--capture requires --serial")
    paths = [p.resolve() for p in (args.input, args.capture, args.csv, args.json_path, args.plot) if p]
    if len(paths) != len(set(paths)):
        parser.error("input and output paths must all be different")
    device = None
    collector = None
    try:
        # Check optional dependencies before acquiring a long board sample.
        if args.plot:
            import importlib.util
            if importlib.util.find_spec("matplotlib") is None:
                raise RuntimeError("--plot requires: python -m pip install matplotlib")
        if args.serial or args.list_ports:
            try:
                import serial
                import serial.tools.list_ports
            except ImportError as exc:
                raise RuntimeError("serial mode requires: python -m pip install pyserial") from exc
        if args.list_ports:
            for port in serial.tools.list_ports.comports():
                print(f"{port.device}: {port.description} [{port.hwid}]")
            return 0
        with ExitStack() as stack:
            if args.serial:
                device = stack.enter_context(serial.Serial(args.serial, args.baud, timeout=0.1, write_timeout=2))
                # Also stop on failures before analysis begins, e.g. an
                # unwritable capture path. Sending S twice is harmless.
                stack.callback(stop_collection, device)
                time.sleep(args.boot_delay)
                device.reset_input_buffer()
                device.write(MODE_COMMANDS[args.mode])
                device.flush()
                capture = None
                if args.capture:
                    args.capture.parent.mkdir(parents=True, exist_ok=True)
                    capture = stack.enter_context(args.capture.open("wb"))
                collector = CollectorSource(FrameReader(device, live=True, timeout=args.timeout),
                                            capture, args.mode)
                chunks = collector.chunks()
                source_name = f"Mega ADC ({args.mode})"
            elif args.input:
                handle = stack.enter_context(args.input.open("rb"))
                collector = CollectorSource(FrameReader(handle))
                chunks = collector.chunks()
                source_name = "Mega capture replay"
            else:
                chunks = iter_simulated_bytes((args.flips + 7) // 8, args.seed)
                source_name = f"PC PRNG (seed={args.seed})" if args.seed is not None else "PC OS random"
            print(f"Collecting {args.flips:,} flips from {source_name}...", flush=True)
            start = time.perf_counter()
            try:
                rows, summary = analyze_chunks(chunks, args.flips, args.stride)
                elapsed = time.perf_counter() - start
            finally:
                if device is not None:
                    stop_collection(device)
            summary.update(source=source_name, seed=args.seed, analysis_seconds=elapsed,
                           bit_order="LSB first; 1=heads, 0=tails",
                           assumption="independent fair flips; finite data cannot prove LLN or entropy")
            if collector:
                summary["collector"] = collector.statistics(elapsed, replay=bool(args.input))
            print_report(rows, summary)
            if args.csv:
                write_csv(args.csv, rows)
            if args.json_path:
                args.json_path.parent.mkdir(parents=True, exist_ok=True)
                args.json_path.write_text(json.dumps({"summary": summary, "rows": rows}, indent=2),
                                          encoding="utf-8")
            if args.plot:
                write_plot(args.plot, rows, summary)
        return 0
    except (OSError, ProtocolError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Cancelled; any captured complete frames are kept.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Capture and summarize raw A0 samples from the diagnostic Mega firmware."""
from __future__ import annotations

import argparse
import struct
import time
from pathlib import Path


def read_exact(stream, count: int) -> bytes:
    data = bytearray()
    while len(data) < count:
        chunk = stream.read(count - len(data))
        if not chunk:
            raise RuntimeError(f"serial ended after {len(data)}/{count} bytes")
        data.extend(chunk)
    return bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=1_000_000)
    parser.add_argument("--output", type=Path, default=Path("artifacts/a0_raw.bin"))
    args = parser.parse_args()
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise SystemExit("install pyserial first: python -m pip install pyserial") from exc

    with serial.Serial(args.port, args.baud, timeout=2) as device:
        time.sleep(2)
        device.reset_input_buffer()
        device.write(b"R")
        device.flush()
        header = read_exact(device, 7)
        if header[:4] != b"ADCR" or header[4] != 1:
            raise RuntimeError(f"unexpected diagnostic header: {header!r}")
        count = struct.unpack_from("<H", header, 5)[0]
        raw = read_exact(device, count * 2)
    values = list(struct.unpack("<" + "H" * count, raw))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(raw)
    print(f"samples={count} min={min(values)} max={max(values)} mean={sum(values)/count:.2f}")
    for bit in range(4):
        ones = sum((value >> bit) & 1 for value in values)
        changes = sum(((values[i - 1] ^ values[i]) >> bit) & 1 for i in range(1, count))
        print(f"bit{bit}: ones={ones}/{count} ({ones/count:.3f}) adjacent_changes={changes}/{count-1}")
    print(f"saved raw little-endian uint16 samples to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

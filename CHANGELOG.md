# Changelog

## Unreleased

- Reduced sampling overhead with fixed plane operations, shared VN pair
  boundaries, and byte-at-a-time reservoir extraction; health checks retained.
- Preserved partial bytes across bounded reads and kept output unchanged on failure.
- Fixed ADC8..15 selection and stopped conversions before restoring ADC registers.
- Honoured optional Timer1/WDT ownership; turbo now disables seasoning by default.
- Replaced wrapping Timer1 byte timing with 512-byte micros() batch benchmarks.
- Added a host-controlled 1 Mbaud collector with CRC, sequence and timing metadata.
- Added Python coin/LLN simulation, Mega collection, replay, CSV/JSON and PNG plots.
- Added protocol/statistics tests, native library tests and an AVR cost comparison.
- Corrected the watchdog interrupt-only WDIE behaviour described below and the
  ADC clock accuracy description: /16 at 16 MHz is already above 200 kHz.

## 0.2.0 - 2026-09-22

- Added a free-running ADC backend through `Config::fast()`.
- Added opt-in `Config::turbo()` with four independently debiased candidate
  planes and a surplus-bit reservoir.
- Added per-plane repetition/proportion health checks and a Turbo example.
- Added `docs/COMPARISON.md` with source-linked, non-fabricated comparisons.
- Reduced output mixing to one ARX absorption per byte in the hot path.
- Replaced the generic variable-rotate helper with fixed-count AVR rotations.
- Documented the `/2` ADC-clock and cross-plane validation limits explicitly.

## 0.1.0 - 2026-09-22

- Initial ATmega2560 implementation.
- ADC LSB physical-noise path with non-overlapping Von Neumann extraction.
- Optional Timer1 phase sampling and watchdog oscillator seasoning.
- Register save/restore, health stops, bounded reads and complete PlatformIO examples.
- Compile-time minimal profile for removing watchdog and Timer1 seasoning.
- Re-arming of the ATmega2560 watchdog interrupt after each hardware-cleared WDIE event.
- Bilingual documentation with explicit limits and board-side benchmark instructions.

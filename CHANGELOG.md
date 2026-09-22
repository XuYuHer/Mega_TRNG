# Changelog

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

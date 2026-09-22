# Changelog

## 0.1.0 - 2026-09-22

- Initial ATmega2560 implementation.
- ADC LSB physical-noise path with non-overlapping Von Neumann extraction.
- Optional Timer1 phase sampling and watchdog oscillator seasoning.
- Register save/restore, health stops, bounded reads and complete PlatformIO examples.
- Compile-time minimal profile for removing watchdog and Timer1 seasoning.
- Re-arming of the ATmega2560 watchdog interrupt after each hardware-cleared WDIE event.
- Bilingual documentation with explicit limits and board-side benchmark instructions.

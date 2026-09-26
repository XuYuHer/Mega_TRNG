# MegaTRNG

**中文使用说明：[抛硬币、大数定律与测速](docs/COIN_LLN.md)。**
新增电脑模拟 / Mega 采集两种运行方式、CSV/JSON/收敛图、二进制串口协议。
优化与实际检查结果见 [验证记录](docs/VALIDATION.md)。

> **Real analogue noise in, auditable random bytes out — tuned for the ATmega2560.**
>
> **把 ATmega2560 的真实模拟噪声变成可审计、可测量的随机字节。**

![MegaTRNG demo](docs/mega-trng-demo.gif)

<!-- GIF slot: replace docs/mega-trng-demo.gif with a board capture when you have one. -->

MegaTRNG is a small, dependency-free PlatformIO/Arduino library for the
Arduino Mega2560. The default path samples the LSB of a noisy or floating ADC
input, removes first-order bias with non-overlapping Von Neumann pairs, and
diffuses the accepted bits through a 128-bit ARX state. Timer1 phase and the
independent watchdog oscillator add physical timing disturbance as seasoning;
they are never presented as a deterministic counter becoming entropy.

MegaTRNG 是面向 Arduino Mega2560 的小型、无第三方依赖 PlatformIO/Arduino 库。
默认路径采集噪声 ADC 输入的最低位，用不重叠 Von Neumann 对去除一阶偏置，再用
128 位 ARX 状态扩散。Timer1 相位和独立看门狗振荡器只作为物理时序扰动加入，
不会把确定性计数器冒充成熵源。

## Why star this project

| Mode | Physical path | Intended use | What is measured here |
|---|---|---|---|
| `Config()` | One ADC LSB, `/16`, optional Timer1/WDT seasoning | Conservative Mega2560 deployment | Compiles; board throughput must be measured by the included sketch |
| `Config::fast()` | Same LSB, ADC free-running | Lower per-sample software overhead | Same physical sampling floor; no fabricated cycle claim |
| `Config::turbo()` | Four low ADC planes, free-running `/2`, per-plane VN and reservoir | Explicit throughput experiment | Compiles; `/2` timing and cross-plane independence remain hardware-validation work |

The conservative `/16` path has a **theoretical** ADC conversion floor near
416 µs/byte at 16 MHz (32 raw conversions × 13 µs under an ideal unbiased model).
Turbo's ideal source-side floor is near 13 µs/byte (8 conversions × 1.625 µs),
but that number is not a board measurement and does not prove four independent
bits. See the [comparison report](docs/COMPARISON.md) for the assumptions and
fair measurement procedure.

The repository never claims to have “beaten every project” without a same-board
log. Stuck or extremely biased sampled bits stop output; there is no PRNG
fallback seeded from `millis()`, a fixed constant, or `random()`.

## Quick start

Keep A0 high impedance or connect a documented analogue noise source. A stable
0 V or 5 V input is intentionally treated as a health failure.

```cpp
#include <TRNG.h>

TRNG rng;

void setup() {
  Serial.begin(115200);
  TRNG::Config cfg;                 // single-plane, speed-oriented ADC timing
  cfg.adcChannel = 0;               // A0
  cfg.adcPrescaler = TRNG::ADC_DIV_16;
  cfg.enableWatchdog = 1;
  cfg.claimTimer1 = 1;
  if (!rng.begin(cfg)) {
    while (true) {}
  }
}

void loop() {
  uint8_t byte = 0;
  if (rng.next(byte, 0)) {          // 0 means wait until a byte is ready
    Serial.println(byte, HEX);
  }
}
```

For the lower-overhead backend:

```cpp
TRNG::Config cfg = TRNG::Config::fast();
cfg.adcChannel = 0;
rng.begin(cfg);
```

For the explicit experiment (read the limitations first):

```cpp
TRNG::Config cfg = TRNG::Config::turbo();
cfg.adcChannel = 0;
if (!rng.begin(cfg)) {
  // Invalid mask, resource conflict, or an immediate health failure.
}
```

## Coin toss and the law of large numbers

Run a computer-only, reproducible million-flip experiment:

```text
python tools/coin_lln.py --simulate --flips 1000000 --seed 7
```

For Mega collection, upload the dedicated firmware and use the board's USB port:

```text
pio run -e megaatmega2560_coin -t upload
python -m pip install -r tools/requirements.txt
python tools/coin_lln.py --list-ports
python tools/coin_lln.py --serial COM7 --mode fast --flips 1000000 --capture artifacts/mega.mlln --csv artifacts/mega.csv --plot artifacts/mega.png
```

The board only collects/conditions bytes and sends binary frames at 1 Mbaud;
the computer counts heads, computes errors and draws the curve. The host can
select `conservative`, `fast` (default) or experimental `turbo` without reflashing.
Each frame carries its sequence, CRC and generation time. Corruption, gaps,
health failures and stalled extraction stop the run explicitly.
Replay with `--input artifacts/mega.mlln`. For plots from the PC simulation,
add `--plot artifacts/coin.png`.

See [the Chinese guide](docs/COIN_LLN.md) for wiring, commands, statistical
assumptions, transport/generation timing and the complete protocol.

`next(byte, limit)` bounds the number of new ADC conversions. It returns
`false` on timeout or a health fault and never substitutes deterministic data.
`fill(buffer, length)` uses the same path until it reaches the requested length
or a fault. Prefer the reference overload in production; the `next()` wrapper
returns zero on failure for Arduino-style convenience.

## Entropy path

1. **ADC quantisation noise.** The source bit is an ADC result bit from the
   configured analogue pin. Use a genuinely noisy node; a floating pin is an
   experiment, not a quantified security source.
2. **Timing seasoning.** Timer1 runs freely at the CPU clock and its sampled
   phase is folded into a rolling digest. The watchdog oscillator contributes
   occasional interrupt phase. Neither counter is used as a raw output bit.
3. **Non-overlapping extraction.** Each enabled plane keeps its own previous
   bit. `01` and `10` become one accepted bit; `00` and `11` are discarded, and
   a pair is never reused in an overlapping window.
4. **Small state mixer.** A 128-bit ARX state absorbs a digest and the accepted
   byte. It spreads physical input across successive outputs; it is not a
   cryptographic proof or a replacement for an entropy estimate.
5. **Health stop.** A 128-sample equal-bit run or an extreme 256-sample
   per-plane proportion stops the generator. Failure is visible through
   `healthFault()` and `ready()`.

Turbo enables four low ADC planes from each conversion. Each plane is debiased
independently, and surplus accepted bits are retained in a small reservoir so
one conversion is not thrown away at a byte boundary. Bits from the same ADC
conversion can still be correlated; the library therefore calls turbo a
candidate multi-plane path and requires raw-capture validation before a security
claim.

## API

### `TRNG::Config`

| Field | Default | Meaning |
|---|---:|---|
| `adcChannel` | `0` | ADC channel `0..15` (`A0..A15`). Leave it noisy or high impedance. |
| `adcPrescaler` | `ADC_DIV_16` | ADPS bits. `/16` is the conservative speed-oriented default. |
| `warmupSamples` | `64` | Samples absorbed and health-checked before output. |
| `enableWatchdog` | `1` | Own the WDT interrupt and add independent oscillator phase events. |
| `claimTimer1` | `1` | Save/configure/restore Timer1 as a free-running phase counter. |
| `freeRunning` | `0` | Keep ADC conversions continuous when set. `Config::fast()` and `turbo()` set it. |
| `bitPlaneMask` | `0x01` | Enabled ADC planes 0..3. `0x01` is the conservative path; `0x0F` is turbo. |

`Config::fast()` selects free-running ADC at `/16`. `Config::turbo()` selects
free-running `/2` and mask `0x0F`; `/2` is outside the conventional 50–200 kHz
ADC accuracy range and is intentionally opt-in. Turbo now disables optional
Timer1/WDT seasoning by default; applications can explicitly enable it again.
At 16 MHz even `/16` is a 1 MHz ADC clock, above the full-accuracy recommendation;
"conservative" describes the single-plane extractor, not a precision ADC setting.

Compile-time flags remove seasoning code when the application does not need it:

```ini
build_flags =
  -DMEGATRNG_ENABLE_WATCHDOG=0
  -DMEGATRNG_ENABLE_TIMER1_PHASE=0
  -DMEGATRNG_ENABLE_WIDE_PLANES=0
```

The third flag removes the experimental multi-plane loops from a conservative
small build; that build accepts only `bitPlaneMask = 0x01`.

### Methods

- `bool begin()` / `bool begin(const Config&)`: save affected registers,
  configure the ADC, optionally claim Timer1 and WDT, and absorb warm-up samples.
- `void end()`: stop free-running ADC, restore saved ADC/Timer1/DIDR/WDT state.
- `bool next(uint8_t& out, uint16_t maxRawSamples = 0)`: get one byte;
  `0` waits, a non-zero limit bounds new consumed ADC samples. Partial bytes
  survive bounded timeouts, and `out` stays unchanged when the call fails.
- `uint8_t next()`: convenience wrapper that returns zero if the reference call
  fails.
- `size_t fill(void* buffer, size_t length)`: fill until complete or a fault.
- `ready()`, `started()`, `healthFault()`: status; `rawSamples()` and
  `acceptedBits()` expose diagnostic counters.

Timer1, the ADC multiplexer and the WDT vector are shared MCU resources. Call
`end()` before another subsystem needs them and do not install a second
`WDT_vect` handler in the same link.

## Performance, size and measurement

The demo uses `Config::fast()` and measures 512-byte batches with `micros()`.
UART output is drained before timing, and results include µs/byte, bytes/s,
consumed ADC samples and accepted bits. The Benchmark example compares all
three modes with the same optional seasoning disabled. This works in minimal
builds and avoids the old 4.096 ms Timer1 wrap. The demo also runs
frequency/runs/128-bit-block smoke tests; these are not NIST certification.

Known from this checkout before a board run:

| Build | Flash | Static RAM | Status |
|---|---:|---:|---|
| `megaatmega2560` demo | **9,448 B** | **532 B** | PlatformIO build with `-Os -flto`; includes formatted batch statistics |
| `megaatmega2560_min` demo | **8,842 B** | **529 B** | Watchdog, Timer1 seasoning and wide-plane loops compiled out |
| `megaatmega2560_coin` collector | **5,346 B** | **513 B** | Binary collection and timing; analysis runs on the computer |

The `/16` 416 µs/byte and turbo `/2` 13 µs/byte values are conversion floors
under stated ideal assumptions. They exclude software and do not certify entropy.
Publish a board result only with the serial log, wiring, reference voltage,
compiler flags and raw-sample statistics.

Run the demo in VS Code + PlatformIO:

```text
pio run -e megaatmega2560
pio run -t upload -e megaatmega2560
pio device monitor -b 115200

# size/minimal-path build
pio run -e megaatmega2560_min

# compile the library examples
pio ci lib/MegaTRNG/examples/Basic/Basic.ino --board megaatmega2560 --lib=lib/MegaTRNG
pio ci lib/MegaTRNG/examples/Benchmark/Benchmark.ino --board megaatmega2560 --lib=lib/MegaTRNG
pio ci lib/MegaTRNG/examples/Turbo/Turbo.ino --board megaatmega2560 --lib=lib/MegaTRNG
```

## Innovation / 创新点

- **Free-running ADC without an entropy shortcut.** `fast()` removes repeated
  conversion setup while the output bit still comes from ADC noise.
- **AVR-aware ARX hot path.** Output absorption uses fixed-count rotations and
  one state absorption per byte, avoiding a generic variable-rotate helper.
- **Parallel candidate planes with lossless byte boundaries.** `turbo()` runs
  separate VN state per low bit plane and retains surplus accepted bits instead
  of discarding them. Cross-plane independence is exposed as a validation task,
  not hidden behind a benchmark claim.
- **One hot path, two latency policies.** The bounded and blocking `next()` APIs
  share the same extractor and health path; no fallback stream appears when a
  caller asks for a latency bound.
- **Resource ownership is explicit.** ADC, Timer1, digital-input-disable bits
  and WDT settings are saved and restored.
- **Failure is observable.** Stuck inputs stop output rather than silently
  changing to a seeded PRNG.
- **AVR-sized implementation.** No heap, floating point, lookup table or third-
  party dependency is required by the library core.

## Self-review record

1. **Physical-source review:** removed `millis()`, fixed-seed PRNG and counter
   output; ADC noise is the high-rate source.
2. **Bias review:** made VN pairs non-overlapping and discarded equal pairs.
3. **Ownership review:** added register save/restore, one active instance and
   explicit Timer1/WDT controls.
4. **Failure review:** added repetition and adaptive-proportion stops plus a
   bounded read API.
5. **Hot-path review:** replaced full state absorption per raw sample with a
   rolling digest and one absorption per output byte.
6. **Backend review:** added free-running ADC and an opt-in multi-plane reservoir;
   marked `/2` timing and cross-plane independence as unverified.
7. **Footprint review:** added `MEGATRNG_ENABLE_WIDE_PLANES=0` so a conservative
   build can remove experimental loops, in addition to the WDT/Timer1 flags.
8. **Assembly review:** replaced the generic variable-rotate helper with
   fixed-count AVR expressions and kept one ARX absorption per output byte.
9. **Documentation review:** moved all speed numbers into a labelled
   theoretical-floor table and added the source-linked comparison report.

The current hot path shares pair boundaries across planes, uses fixed plane
offsets, and drains the reservoir a byte at a time. The ADC source, health
thresholds, Von Neumann rule and ARX conditioning are retained.
See [validation](docs/VALIDATION.md) for a reproducible AVR instruction cost
comparison; actual board throughput must still be measured.

## Limits and honest claims / 限制与诚实声明

- A floating ADC pin is not a quantified entropy source. Characterise the
  analogue circuit and estimate min-entropy for security work.
- `/2` ADC timing is outside the normal accuracy recommendation. Turbo is an
  experiment until raw captures show stable, independent-enough planes.
- The ARX mixer and included tests are not a NIST certification or a DRBG proof.
  Do not use this library as the sole key-generation primitive without review.
- The library targets ATmega2560 register names and assumes the Arduino AVR
  core. Other AVR parts require a deliberate port.
- Timer1 and the WDT vector are shared. Integrate them deliberately with Servo,
  tone, bootloader watchdog or another WDT user.
- No Mega2560 board was connected while this revision was prepared. The code
  compiles; the serial throughput and noise quality remain user-measured data.

## Roadmap

- Publish labelled Mega2560 raw captures and a reproducible entropy estimator.
- Add raw ADC capture and host per-plane/cross-plane tests (the coin capture
  currently contains conditioned output, not raw ADC samples).
- Add CI across several Arduino AVR core versions.
- Add an optional ADC interrupt backend only after RAM and ISR costs are measured.

## Repository layout

```text
lib/MegaTRNG/
  src/TRNG.h, TRNG.cpp             library implementation
  examples/Basic/                  minimal serial example
  examples/Benchmark/              512-byte batch benchmark
  examples/Turbo/                  explicit multi-plane experiment
  library.json                     PlatformIO metadata
  library.properties               Arduino Library Manager metadata
src/main.cpp                       demo, size report and smoke tests
src/coin_collector.cpp             Mega binary collector, host-controlled modes
tools/coin_lln.py                   PC simulation / serial collection / replay
tools/check.py                     protocol and native library regression checks
tools/benchmark_avr.py             repeatable AVR software-cost comparison
docs/COIN_LLN.md                    Chinese experiment and protocol guide
docs/VALIDATION.md                  build, simulation and validation evidence
docs/COMPARISON.md                 source-linked comparison report
docs/README.md                     documentation landing page
platformio.ini                     Mega2560 build environments
CHANGELOG.md                       release history
.github/ISSUE_TEMPLATE/            issue forms
LICENSE                             MIT license
```

## License

MIT. See [LICENSE](LICENSE).

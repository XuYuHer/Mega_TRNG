# Mega2560 TRNG comparison / 对比报告

This is a source and build comparison, not a leaderboard.  Only a connected
Mega2560 can provide the board timing and noise data needed for a throughput or
entropy claim.  The checkout used for this report builds with PlatformIO's
Arduino AVR core 5.4.0, avr-g++ 7.3.0 and `-Os -flto`; the external projects are
linked to the source reviewed below.  External byte rates are deliberately left
as **unverified** when their repositories do not publish a reproducible
Mega2560 measurement.

这是源码和构建口径的对比，不是未经复现实验的排行榜。吞吐和熵率必须在接好
Mega2560 后测量；外部项目没有可复现的 Mega2560 数据时，本报告保留为“未验证”。

## At-a-glance

| Implementation | Physical source | Debias / conditioning | Blocking behaviour | Mega2560 status | Speed/size evidence |
|---|---|---|---|---|---|
| **MegaTRNG `Config()`** | Floating/noisy ADC LSB; Timer1 phase and WDT oscillator are seasoning | Non-overlapping Von Neumann, 128-bit ARX mixer, repetition + proportion health stop | `next(out, 0)` waits for a byte; bounded overload returns `false` | **Built for ATmega2560** | This checkout: 7,882 B flash / 521 B static RAM for the demo. `/16` sampling floor is about 416 µs/byte before overhead; board result **unmeasured here** |
| **MegaTRNG `Config::fast()`** | Same ADC LSB source | Same | Same API; free-running ADC removes per-sample ADSC setup | **Built path; board result pending** | Same physical floor, lower software overhead; no fabricated cycle number |
| **MegaTRNG `Config::turbo()`** | ADC planes 0..3 from the same conversion | One VN pair per plane, per-plane health checks, surplus-bit reservoir, ARX mixer | Usually fewer conversions per call; still blocks when reservoir is empty | **Compiles; experimental** | `/2` ADC + four candidate planes gives a theoretical source floor near 13 µs/byte. ADC timing is outside the conventional accuracy range and cross-plane independence is **unverified** |
| [Arduino AVR `random()`](https://github.com/arduino/ArduinoCore-avr/blob/master/cores/arduino/WMath.cpp) | None; deterministic PRNG | No extractor | Non-blocking | Builds on Mega2560 | Fast, but it is not a TRNG and is therefore not a like-for-like competitor |
| [TrueRandom](https://github.com/sirleech/TrueRandom) (`master`) | Analog 0 LSB | Two nested Von Neumann stages | Blocking `randomBit()` / `randomByte()` | Its README says Mega is unsupported | Source inspection implies roughly 64 raw ADC conversions per output byte in the common unbiased case; register setup happens for each raw conversion. No same-board cycle/size result published |
| [Entropy / AVR hardware random](https://github.com/sylvandb/avr-hardware-random-number-generation) (`master`) | Watchdog interrupt timing sampled from Timer1 | Jenkins-style hash into a pool | `random()` waits for a populated pool | Broad AVR/Arduino support; Mega build was not run in this comparison | Repository comments state a nominal ~16 ms WDT period and about two 32-bit values/s. Source allocates a 32-byte raw buffer and eight 32-bit pool entries before core/application overhead; exact linked size is unverified |
| [NeumannCorrector](https://github.com/RobTillaart/NeumannCorrector) | Caller-provided stream | Von Neumann corrector only | Depends on caller | Not a TRNG source | Useful extractor reference; it does not provide ADC/WDT entropy or a Mega throughput number |
| External avalanche/noise IC | Dedicated analogue noise circuit or chip | Depends on device and host conditioner | Depends on host interface | Requires extra hardware | Can beat an MCU ADC in quality or rate, but there is no honest apples-to-apples number without naming the part and measuring it |

The first three rows are the only rows for which this repository contains a
Mega2560 build.  “Theoretical floor” means conversion time multiplied by the
expected number of raw samples under an ideal, independent model; it excludes
software, interrupt latency, ADC settling, bias rejection and health failures.

前三行是本仓库实际包含 Mega2560 构建的模式。“理论下界”只把理想独立模型下的
ADC 转换时间乘以期望原始采样数；它不包含软件、ISR、建立时间、偏置丢弃和健康故障。

## Why the numbers are not interchangeable

MegaTRNG's conservative stream uses one ADC LSB and one non-overlapping VN
extractor.  With a 16 MHz CPU and `/16` ADC prescaler, the 13 ADC-clock
conversion is about 208 CPU cycles (13 µs).  An unbiased VN pair accepts one bit
with probability 1/4 per two samples, so eight output bits need about 32 raw
conversions: **416 µs/byte is a floor, not a measurement**.

`Config::fast()` lets the ADC free-run.  It cannot create entropy or beat the
ADC's physical conversion period; it only removes the software start sequence
between conversions.  `Config::turbo()` samples four low bit planes and keeps
extra accepted bits in a small reservoir.  Under an ideal independence model,
eight output bits need about eight conversions at `/2` (26 CPU cycles per
conversion, or roughly 13 µs of conversion time).  The `/2` ADC clock is outside
the usual 50–200 kHz ADC accuracy recommendation, and bits from one conversion
are correlated candidates until raw captures demonstrate otherwise.  The turbo
floor must therefore never be presented as an entropy-rate guarantee.

TrueRandom's source is conceptually close to the conservative mode, but its
source calls save/configure/restore ADC and GPIO registers for every raw bit and
its second VN layer costs another expected factor of two.  Entropy uses a
different, slow physical source: the watchdog oscillator.  Its pool design can
make a later read cheap, but the entropy arrival rate is bounded by the WDT
interrupt period.

## Reproduce a fair Mega2560 comparison

1. Connect the same documented analogue source to A0. A floating pin is an
   experiment, not a quantified security source; record the wiring, reference
   voltage, board revision, supply and temperature.
2. Build this checkout with `pio run -e megaatmega2560`, upload it, and open a
   115200 baud monitor. `src/main.cpp` prints Timer1 cycles and microseconds per
   byte, raw/accepted counters, smoke-test results, and linker sizes.
3. Repeat with `Config::fast()` and `Config::turbo()` in the example. Measure at
   least 128 bytes after warm-up; report median and percentile values because VN
   rejection makes individual calls variable.
4. For turbo, capture the raw four-plane values before the mixer and test each
   plane and cross-plane pairs. A monobit/runs/block smoke test is useful for
   regressions; it is not an entropy certification. For security work, perform
   an SP 800-90B-style min-entropy assessment on a large, labelled capture.
5. Compare flash and static RAM from the same linker command. Do not compare a
   library object with another project's full sketch, and do not infer an
   external project's byte rate from a different MCU or compiler.

Useful commands:

```text
pio run -e megaatmega2560
pio run -e megaatmega2560_min
pio ci lib/MegaTRNG/examples/Basic/Basic.ino --board megaatmega2560 --lib=lib/MegaTRNG
pio ci lib/MegaTRNG/examples/Benchmark/Benchmark.ino --board megaatmega2560 --lib=lib/MegaTRNG
pio device monitor -b 115200
```

## Honest conclusion

On a Mega2560, MegaTRNG's defensible advantage is the combination of a direct
ADC path, one VN pass, free-running sampling, and an API that exposes a bounded
read without a deterministic fallback.  The conservative mode is the only mode
whose timing assumptions are conventional.  Turbo may be much faster on a
particular board, but until raw captures and board logs exist it is a
throughput experiment, not proof that MegaTRNG “beats every project” or that its
four planes contain four independent bits.

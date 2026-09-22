# MegaTRNG

> **让 ATmega2560 的真实模拟噪声，变成可审计、可测量、可直接使用的随机字节。**
>
> **Turn real analogue noise on an ATmega2560 into auditable, measurable random bytes.**

![MegaTRNG demo](docs/mega-trng-demo.gif)

`docs/mega-trng-demo.gif` 是演示位：把串口输出、Timer1 基准测试和测试结果录成 GIF 后放在这里即可。

| 方案 | 真实物理熵 | 在线去偏 | Mega2560 不加外设 | 代码可审计 |
|---|---:|---:|---:|---:|
| **MegaTRNG（本项目）** | ADC 量化噪声 + 独立 WDT 相位 | Von Neumann | 是，需可用的模拟噪声输入 | 是 |
| `randomSeed(analogRead())` | 可能只有一次 ADC 读数 | 否 | 是 | 是 |
| LCG / xorshift / 固定种子 PRNG | 否 | 不适用 | 是 | 是 |
| 外接 avalanche / 噪声芯片 | 是 | 取决于芯片 | 否 | 取决于芯片 |

表格是架构对比，不是对其他项目的统一实验室排名。MegaTRNG 不伪造跨硬件平台的吞吐或熵率数据；请用仓库中的示例在自己的板子上测量。

## English first steps

1. Open `A0` as a high-impedance analogue noise node. A genuinely noisy sensor or a documented noise circuit is better than a quiet, driven DC source.
2. Open this folder in VS Code with the PlatformIO extension.
3. Build and upload `megaatmega2560`, then open a 115200-baud serial monitor.
4. Read the printed Timer1 `us/byte`, flash/RAM report and three small statistical smoke tests.

```cpp
#include <TRNG.h>

TRNG rng;

void setup() {
  Serial.begin(115200);
  TRNG::Config cfg;
  cfg.adcChannel = 0;                 // A0
  cfg.adcPrescaler = TRNG::ADC_DIV_16;
  cfg.enableWatchdog = 1;
  cfg.claimTimer1 = 1;
  if (!rng.begin(cfg)) {
    while (true) {}
  }
}

void loop() {
  uint8_t byte = 0;
  if (rng.next(byte, 0)) {
    Serial.println(byte, HEX);
  }
}
```

`next(byte, 0)` deliberately waits for eight debiased bits. Use a non-zero second argument when a cooperative loop needs a bound:

```cpp
uint8_t byte;
if (!rng.next(byte, 64)) {
  // No byte within 64 ADC samples, or a health fault was detected.
}
```

## 中文快速开始

把 A0 保持高阻浮空，或接入你能说明来源的模拟噪声电路。不要把 A0 接到稳定的 0 V、5 V 或低噪声直流电压上；那样库应当报告健康故障。用 VS Code + PlatformIO 打开本目录，执行：

```text
PlatformIO: Build
PlatformIO: Upload
PlatformIO: Monitor（115200）
```

命令行等价物：

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

`src/main.cpp` 会打印随机字节、Timer1 测得的 `µs/byte` 与 CPU 周期、链接器统计的 Flash/RAM，以及频率、游程、128-bit 块频率三个烟雾测试。统计测试不是 NIST 认证，也不能替代熵源审计。

## Entropy path / 熵源路径

1. **ADC physical sample.** The least-significant bit of a conversion on `adcChannel` is the only bit presented to the debiaser. The default is AVcc reference, right-aligned 10-bit ADC, and `/16` ADC clock (`1 MHz` at 16 MHz CPU). The input must carry physical variation; a deterministic firmware value is never substituted.
2. **Timer1 phase metadata.** Timer1 can be claimed and run at `F_CPU/1`. Its phase is absorbed into the state around each conversion. A counter is not called entropy: it only exposes timing changes to the mixer.
3. **Watchdog oscillator.** When enabled, the independent watchdog oscillator interrupts at its shortest nominal period (about 16 ms on this family). The ISR samples Timer1 phase and injects the event into the mixer. This is a slow secondary source, not the throughput source.
4. **Von Neumann debiasing.** Non-overlapping pairs `01 -> 0`, `10 -> 1`; `00` and `11` are discarded. A pair is never reused in an overlapping window.
5. **ARX state mixer.** A 128-bit in-RAM state diffuses samples and provides forward evolution between output bytes. It is a mixer, not an entropy source and not a cryptographic certification.
6. **Health monitors.** A run of 128 equal raw bits or an extreme 256-bit adaptive-proportion window stops output. This catches common open/shorted-input failures without pretending that a health test proves randomness.

The Von Neumann extractor removes first-order bias when successive raw bits are independent enough. It cannot create entropy from a stuck pin, and the library intentionally has no deterministic fallback.

### 中文原理与接口摘要

高吞吐路径只取 ADC 转换结果的最低位；A0 必须接触真实模拟变化，固件不会用计时器、线性同余或固定种子补位。Timer1 的相位和独立 WDT 振荡器事件只作为第二时钟的物理扰动注入 128 位状态，不能把确定性计数器冒充熵源。ADC 原始位按不重叠二元组执行 `01→0`、`10→1`，`00/11` 丢弃，再由 ARX 状态扩散输出。

`begin()` 保存并配置 ADC、Timer1 和 WDT；`end()` 恢复寄存器。`next(out, 0)` 会阻塞直到得到一个字节，`next(out, 上限)` 在指定 ADC 样本数内没有得到字节就返回 `false`，`fill()` 用同一条路径填充缓冲区。`ready()` 和 `healthFault()` 用来检查健康状态；发生长重复或极端 256 位比例时，库停止输出，不降级到伪随机。

默认 `/16` ADC 分频在 16 MHz 下有约 13 µs 的单次转换下界；Von Neumann 去偏后的八位预计需要约 32 次原始采样，因此 416 µs/byte 只是理论下界。请以 `src/main.cpp` 的 Timer1 结果为准，并同时记录接线、参考电压和编译参数。

## API

### `TRNG::Config`

| Field | Default | Meaning |
|---|---:|---|
| `adcChannel` | `0` | External ADC channel `0..15` (`A0..A15`). Leave it floating or attach a physical noise source. |
| `adcPrescaler` | `ADC_DIV_16` | ADPS bits. `/16` is fast; `/32`, `/64` and `/128` trade throughput for conventional ADC timing. |
| `warmupSamples` | `64` | ADC conversions absorbed before output. |
| `enableWatchdog` | `1` | Add independent watchdog-oscillator phase events. With the default compile flag, the library owns `WDT_vect`; do not define a second WDT ISR in the application. |
| `claimTimer1` | `1` | Save/configure/restore Timer1 as a free-running phase counter. Set `0` if another subsystem owns it. |

For a smaller build, compile-time flags remove the corresponding seasoning path completely:

```ini
build_flags =
  -DMEGATRNG_ENABLE_WATCHDOG=0
  -DMEGATRNG_ENABLE_TIMER1_PHASE=0
```

Runtime configuration cannot re-enable a path removed this way. The ADC source and Von Neumann extractor remain present.

### Methods

- `bool begin()` / `bool begin(const Config&)`: save affected registers, initialise ADC, optionally claim Timer1 and WDT, and absorb warm-up samples.
- `void end()`: restore saved ADC, Timer1, digital-input and watchdog registers.
- `bool next(uint8_t& out, uint16_t maxRawSamples = 0)`: return one byte. `0` waits; a non-zero limit bounds the call. Returns `false` on timeout or health fault.
- `uint8_t next()`: convenience wrapper; returns zero on failure, so production code should prefer the reference overload.
- `size_t fill(void* buffer, size_t length)`: blocking fill until complete or a health fault.
- `ready()`, `started()`, `healthFault()`: status probes. `rawSamples()` and `acceptedBits()` expose counters for diagnostics.

Timer1, the ADC multiplexer and (when compiled in) the watchdog vector are shared MCU resources. Call `end()` before another subsystem needs the registers, and do not provide a competing `WDT_vect` handler in the same link. Set the corresponding configuration flag when you accept weaker timing seasoning.

## Performance and size

At `/16`, one ADC conversion has a 13-cycle ADC conversion floor, or about **13 µs** at 16 MHz. A Von Neumann accepted bit costs an expected four raw bits, so eight output bits have a physical-sampling floor near **416 µs/byte** before register and mixer overhead. The actual rate depends on ADC noise, bias and rejection; a quiet input can take arbitrarily longer and will eventually fail health checks.

The demo measures each byte with Timer1; each individual timing interval must stay below one 4.096 ms counter wrap. It also prints the linker result. A build from this repository currently compiles for `megaatmega2560`; the exact numbers below are build- and demo-dependent:

| Measurement | What is known before a board run |
|---|---|
| Throughput | 416 µs/byte is a sampling floor at `/16`; no hardware throughput claim is made here. |
| Flash | This checkout's default demo build: **7,556 bytes**; `megaatmega2560_min`: **7,226 bytes**. PlatformIO `pio run` is authoritative for your toolchain. |
| RAM | This checkout's default demo build: **505 bytes**; minimal seasoning build: **502 bytes**. The library object itself is **52 bytes**; the demo additionally allocates a 256-byte test buffer and Arduino Serial state. |

Do not quote the floor as a measured result. Copy the serial output from your exact board, ADC reference, wiring and compiler flags when publishing a benchmark.

## Innovation / 创新点

- **Two clocks, one honest boundary.** ADC quantisation noise is the high-rate path; the independent watchdog oscillator supplies a physically different timing disturbance. Timer1 is explicitly treated as phase metadata, never marketed as a random counter.
- **Debias before diffusion.** Non-overlapping Von Neumann pairs sit before the 128-bit state, so the mixer cannot conceal a one-sided raw source.
- **Resource-aware ownership.** All touched registers are saved and restored. `claimTimer1` and `enableWatchdog` make the shared-resource trade-off visible in the API.
- **Bounded and blocking reads share one path.** `next(out, limit)` lets a cooperative application choose a latency bound without adding a second pseudo-random code path.
- **Failure is observable.** Repetition and adaptive-proportion checks stop output instead of returning a seeded PRNG stream when the analogue node is dead.
- **Small AVR footprint.** No heap, no third-party dependency, no lookup table and no floating point in the library core.

## Review loop / 自我审查记录

1. **Round 1 — physical source audit.** Removed any idea of using `millis()` or a fixed-seed PRNG as a source; ADC LSB became the only debiased high-rate bit.
2. **Round 2 — bias audit.** Made Von Neumann pairs non-overlapping and discarded equal pairs; a previous bit is never reused.
3. **Round 3 — shared-resource audit.** Added register save/restore, an explicit Timer1 ownership flag and a watchdog ownership guard.
4. **Round 4 — failure audit.** Added 128-bit repetition and 256-bit adaptive-proportion health stops, plus a bounded `next()` API.
5. **Round 5 — size/speed audit.** Kept the core heap-free, used a 128-bit ARX mixer with no tables, and set `/16` as the speed-oriented default while documenting the ADC timing trade-off.
6. **Round 6 — documentation audit.** Removed unmeasured leaderboard claims, made the 416 µs figure a theoretical floor, and added a board-side measurement procedure.

The design is at a practical documentation/code balance: further speed gains would require relaxing the extractor or ADC sampling assumptions, so this review loop has converged.

### README self-check

The largest weakness was an easy-to-misread speed claim: a theoretical ADC floor can look like a board benchmark. The performance section now labels it as a floor, points to the Timer1 measurement and requires the exact wiring/compiler output for any published number. One addition that makes the project stronger is the `megaatmega2560_min` PlatformIO environment: it proves that the watchdog and Timer1 seasoning paths can be compiled out for size experiments instead of being merely promised in prose.

## Limits and honest claims / 限制与诚实声明

- A floating ADC pin is an entropy opportunity, not a quantified entropy guarantee. For security work, add a characterised analogue noise source and perform a proper entropy estimate.
- The watchdog oscillator is slow and process/voltage/temperature dependent. It improves source diversity but does not rescue a silent ADC input.
- The ARX mixer is not a NIST-approved DRBG, and the included tests are smoke tests. Do not use this library as the sole key-generation primitive without an application-specific security review.
- The library currently targets ATmega2560 and assumes the Arduino AVR register names. Other AVR parts may need a port.
- Timer1 and the watchdog vector are shared resources. Integrate them deliberately with Servo, tone, bootloader watchdog or another WDT user; the default build provides the one `WDT_vect` handler for the library.

## Roadmap

- Add an optional ADC free-running/ADC-noise-reduction backend with the same extractor API.
- Add a host-side capture tool that records raw bits and computes a reproducible entropy estimate.
- Add CI builds for a small matrix of Arduino AVR core versions.
- Publish board-specific measurements only after collecting raw serial logs and wiring details.

## Repository layout

```text
lib/MegaTRNG/
  src/TRNG.h, TRNG.cpp       library implementation
  examples/Basic/             minimal serial example
  examples/Benchmark/         Timer1 benchmark example
  library.json                PlatformIO metadata
  library.properties          Arduino Library Manager metadata
src/main.cpp                  complete demo and smoke tests
platformio.ini                Mega2560 build configuration
CHANGELOG.md                  release history
LICENSE                       MIT license
.github/ISSUE_TEMPLATE/      issue forms
```

## License

MIT. See [LICENSE](LICENSE).

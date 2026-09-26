// Deterministic ADC stub for AVR instruction-simulator cost comparisons only.
// No real conversions, UART, oscillator jitter or entropy claims.
#include <TRNG.h>

static uint32_t inputState;
uint16_t fakeAdcRead() {
    inputState ^= inputState << 13;
    inputState ^= inputState >> 17;
    inputState ^= inputState << 5;
    return (uint16_t)(inputState & 1023U);
}

volatile uint16_t benchmarkBytes = 0;
volatile uint32_t benchmarkRaw = 0;
volatile uint32_t benchmarkAccepted = 0;
volatile uint32_t benchmarkChecksum = 0;

__attribute__((noinline, used)) void benchmarkDone() {
    asm volatile("nop");
}

int main() {
    inputState = 0x715AC39DUL;
    TRNG rng;
    TRNG::Config cfg;
    cfg.warmupSamples = 64;
    cfg.enableWatchdog = 0;
    cfg.claimTimer1 = 0;
    cfg.freeRunning = 1;
#ifdef BENCHMARK_WIDE
    cfg.bitPlaneMask = 15;
    cfg.adcPrescaler = TRNG::ADC_DIV_2;
#endif
    if (rng.begin(cfg)) {
        for (uint16_t i = 0; i < 256; ++i) {
            uint8_t value = 0;
            if (!rng.next(value, 4096)) break;
            benchmarkChecksum = benchmarkChecksum * 33UL + value;
            ++benchmarkBytes;
        }
        benchmarkRaw = rng.rawSamples();
        benchmarkAccepted = rng.acceptedBits();
    }
    benchmarkDone();
    return 0;
}

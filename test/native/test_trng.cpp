#include <TRNG.h>
#include <cassert>
#include <cstdio>
#include <vector>
#include <random>

static std::vector<uint16_t> samples;
static size_t cursor;

uint16_t fakeAdcRead() {
    assert(cursor < samples.size());
    return samples[cursor++];
}

static TRNG::Config config(uint8_t mask = 1) {
    TRNG::Config cfg = TRNG::Config::fast();
    cfg.warmupSamples = 0;
    cfg.claimTimer1 = 0;
    cfg.enableWatchdog = 0;
    cfg.bitPlaneMask = mask;
    return cfg;
}

static std::vector<uint8_t> collect(uint8_t mask, bool bounded) {
    cursor = 0;
    TRNG rng;
    assert(rng.begin(config(mask)));
    std::vector<uint8_t> result;
    for (unsigned call = 0; result.size() < 256 && call < 50000; ++call) {
        uint8_t out = 0xA5;
        uint32_t before = rng.rawSamples();
        if (rng.next(out, bounded ? 1 : 0)) {
            result.push_back(out);
        } else {
            assert(out == 0xA5);
            assert(!rng.healthFault());
        }
        if (bounded) assert(rng.rawSamples() - before <= 1);
    }
    assert(result.size() == 256);
    // Compare accepted counter with an independent VN pair reference.
    uint32_t accepted = 0;
    for (size_t i = 0; i + 1 < cursor; i += 2) {
        for (uint8_t plane = 0; plane < 4; ++plane) {
            if (mask & (1U << plane)) {
                accepted += ((samples[i] ^ samples[i + 1]) >> plane) & 1U;
            }
        }
    }
    assert(rng.acceptedBits() == accepted);
    assert(accepted >= 2048 && accepted <= 2051);
    assert(rng.rawSamples() == cursor);
    return result;
}

static void extraction() {
    std::mt19937 generator(1024);
    samples.clear();
    for (unsigned i = 0; i < 50000; ++i) samples.push_back(generator() & 1023);
    for (uint8_t mask = 1; mask < 16; ++mask) {
#if !MEGATRNG_ENABLE_WIDE_PLANES
        if (mask != 1) break;
#endif
        assert(collect(mask, false) == collect(mask, true));
    }
}

static void health() {
    for (uint16_t constant : {0, 1023}) {
        samples.assign(256, constant);
        cursor = 0;
        TRNG rng;
        assert(rng.begin(config()));
        uint8_t out = 0x7B;
        assert(!rng.next(out, 4096));
        assert(rng.healthFault());
        assert(rng.rawSamples() == 128);
        assert(out == 0x7B);
        assert(!rng.next(out) && cursor == 128);
        assert(rng.fill(&out, 1) == 0);
    }
    // Proportion failure without a long repeated run (one bit per 32 samples).
    samples.assign(256, 0);
    for (size_t i = 0; i < 256; i += 32) samples[i] = 1;
    cursor = 0;
    TRNG rng;
    TRNG::Config cfg = config();
    cfg.warmupSamples = 256;
    assert(!rng.begin(cfg));
    assert(rng.healthFault() && !rng.started());
    assert(rng.rawSamples() == 256);
#if MEGATRNG_ENABLE_WIDE_PLANES
    // A stuck upper plane must fault even when the LSB looks healthy.
    samples.clear();
    for (unsigned i = 0; i < 256; ++i) samples.push_back(i & 1);
    cursor = 0;
    cfg = config(8);
    assert(rng.begin(cfg));
    uint8_t out;
    assert(!rng.next(out, 256) && rng.healthFault());
#endif
}

static void ownership() {
    fakeAdmux = 0x42;
    fakeAdcsrb = 0x20;
    fakeDidr0 = 0x04;
    fakeDidr2 = 0x10;
    fakeTimerReads = 0;
    for (uint8_t channel = 0; channel < 16; ++channel) {
        TRNG first, second;
        TRNG::Config cfg = config();
        cfg.adcChannel = channel;
        assert(first.begin(cfg));
        assert(fakeAdmux == (_BV(REFS0) | (channel & 7)));
        assert((fakeAdcsrb & _BV(MUX5)) == (channel >= 8 ? _BV(MUX5) : 0));
        assert(!second.begin(cfg));
        first.end();
        assert(fakeAdmux == 0x42 && fakeAdcsrb == 0x20);
        assert(fakeDidr0 == 0x04 && fakeDidr2 == 0x10);
        assert(second.begin(cfg));
    }
    assert(fakeTimerReads == 0);
    TRNG rng;
    TRNG::Config cfg = config();
    cfg.adcChannel = 16;
    assert(!rng.begin(cfg));
    cfg = config(0);
    assert(!rng.begin(cfg));
    cfg = config(0x10);
    assert(!rng.begin(cfg));
#if !MEGATRNG_ENABLE_WIDE_PLANES
    assert(!rng.begin(config(2)));
#endif
}

static void boundedTimeout() {
    samples.clear();
    // 00,11,00,11... passes both raw health tests but VN accepts nothing.
    for (unsigned i = 0; i < 4096; ++i) samples.push_back((i / 2) & 1);
    for (unsigned i = 0; i < 16; ++i) samples.push_back(i & 1);
    cursor = 0;
    TRNG rng;
    assert(rng.begin(config()));
    uint8_t out = 0xA5;
    assert(!rng.next(out, 4096));
    assert(out == 0xA5 && rng.ready() && rng.acceptedBits() == 0);
    assert(rng.rawSamples() == 4096);
    assert(rng.next(out, 16) && rng.acceptedBits() == 8);
    assert(rng.fill(nullptr, 10) == 0);
    rng.end();
    assert(rng.begin(config()));
    assert(rng.rawSamples() == 0 && rng.acceptedBits() == 0);
}

static void timerAndWatchdogOwnership() {
    TRNG rng;
    TRNG::Config cfg = config();
    cfg.enableWatchdog = 1;
    fakeTimerReads = 0;
    fakeWdtcsr = 5;
    assert(rng.begin(cfg));
    TRNG::onWatchdogInterrupt();
    assert(fakeTimerReads == 0); // WDT alone must not read another user's timer.
    rng.end();
    assert(fakeWdtcsr == 5);

    fakeTccr1a = 12; fakeTccr1b = 34; fakeTccr1c = 56;
    fakeTimsk1 = 78; fakeTcnt1 = 0x1234;
    cfg.claimTimer1 = 1;
    assert(rng.begin(cfg));
    assert(fakeTccr1a == 0 && fakeTccr1b == 1 && fakeTimsk1 == 0);
    TRNG::onWatchdogInterrupt();
    assert(fakeTimerReads > 0);
    rng.end();
    assert(fakeTccr1a == 12 && fakeTccr1b == 34 && fakeTccr1c == 56);
    assert(fakeTimsk1 == 78 && fakeTcnt1 == 0x1234);
}

int main() {
    extraction();
    health();
    ownership();
    boundedTimeout();
    timerAndWatchdogOwnership();
    std::puts("TRNG native regression checks passed");
}

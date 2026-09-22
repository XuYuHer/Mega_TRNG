#include "TRNG.h"

#include <avr/interrupt.h>
#if MEGATRNG_ENABLE_WATCHDOG
#include <avr/wdt.h>
#endif
#include <util/atomic.h>

namespace {
#define MEGATRNG_ROTL32(value, amount) \
    ((uint32_t)(((value) << (amount)) | ((value) >> (32U - (amount)))))

#if MEGATRNG_ENABLE_WATCHDOG
volatile uint8_t g_wdtMix = 0;
volatile uint8_t g_wdtEvents = 0;
volatile uint8_t g_wdtSequence = 0;
#endif
}

TRNG *TRNG::active_ = 0;

TRNG::Config::Config()
    : adcChannel(0),
      adcPrescaler(TRNG::ADC_DIV_16),
      warmupSamples(64),
      enableWatchdog(1),
      claimTimer1(1),
      freeRunning(0),
      bitPlaneMask(0x01) {}

TRNG::Config TRNG::Config::fast() {
    Config config;
    config.freeRunning = 1;
    return config;
}

TRNG::Config TRNG::Config::turbo() {
    Config config;
    config.freeRunning = 1;
    config.adcPrescaler = TRNG::ADC_DIV_2;
    config.bitPlaneMask = 0x0F;
    return config;
}

TRNG::TRNG()
    : config_(),
      started_(false),
      healthFault_(false),
      previousBits_(0),
      havePreviousBits_(0),
      lastHealthBits_(0),
      healthHaveBits_(0),
      healthWindowBits_(0),
      healthRunLength_{0, 0, 0, 0},
      healthWindowOnes_{0, 0, 0, 0},
      rawSamples_(0),
      acceptedBits_(0),
      sampleDigest_(0),
      pendingValue_(0),
      pendingBits_(0),
      s0_(0),
      s1_(0),
      s2_(0),
      s3_(0),
      savedAdmux_(0),
      savedAdcsra_(0),
      savedAdcsrb_(0),
      savedDidr0_(0),
      savedDidr2_(0),
      savedTccr1a_(0),
      savedTccr1b_(0),
      savedTccr1c_(0),
      savedTcnt1_(0),
      savedTimsk1_(0),
      savedWdtcsr_(0),
      timerClaimed_(0),
      watchdogClaimed_(0) {}

TRNG::~TRNG() {
    end();
}

bool TRNG::begin() {
    Config config;
    return begin(config);
}

bool TRNG::begin(const Config &config) {
    if (started_) {
        return true;
    }
    if (active_ != 0 && active_ != this) {
        return false;
    }
    if (config.adcChannel > 15 || config.adcPrescaler > 7 ||
        (config.bitPlaneMask & 0x0FU) == 0 ||
        (config.bitPlaneMask & 0xF0U) != 0) {
        return false;
    }
#if !MEGATRNG_ENABLE_WIDE_PLANES
    if (config.bitPlaneMask != 0x01U) {
        return false;
    }
#endif

    config_ = config;
    savedAdmux_ = ADMUX;
    savedAdcsra_ = ADCSRA;
    savedAdcsrb_ = ADCSRB;
#if defined(DIDR0)
    savedDidr0_ = DIDR0;
#endif
#if defined(DIDR2)
    savedDidr2_ = DIDR2;
#endif
    savedTccr1a_ = TCCR1A;
    savedTccr1b_ = TCCR1B;
    savedTccr1c_ = TCCR1C;
    savedTcnt1_ = TCNT1;
    savedTimsk1_ = TIMSK1;

    s0_ = 0x243F6A88UL;
    s1_ = 0x85A308D3UL;
    s2_ = 0x13198A2EUL;
    s3_ = 0x03707344UL;
    previousBits_ = 0;
    havePreviousBits_ = 0;
    lastHealthBits_ = 0;
    healthHaveBits_ = 0;
    healthWindowBits_ = 0;
    for (uint8_t plane = 0; plane < 4U; ++plane) {
        healthRunLength_[plane] = 0;
        healthWindowOnes_[plane] = 0;
    }
    rawSamples_ = 0;
    acceptedBits_ = 0;
    sampleDigest_ = 0xA4093822UL;
    pendingValue_ = 0;
    pendingBits_ = 0;
    healthFault_ = false;
    timerClaimed_ = 0;
    watchdogClaimed_ = 0;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
#if MEGATRNG_ENABLE_WATCHDOG
        g_wdtMix = 0;
        g_wdtEvents = 0;
        g_wdtSequence = 0;
#endif
        configureAdc();
        if (config_.claimTimer1 && MEGATRNG_ENABLE_TIMER1_PHASE) {
            configureTimer1();
            timerClaimed_ = 1;
        }
        active_ = this;
        started_ = true;

        if (config_.enableWatchdog && MEGATRNG_ENABLE_WATCHDOG) {
            configureWatchdog();
            watchdogClaimed_ = 1;
        }
    }

    // Absorb startup samples into the state.  The fixed constants above are
    // only domain separation; no output is released until physical samples
    // have been observed.
    uint16_t warmup = config_.warmupSamples;
    while (warmup-- != 0) {
        uint16_t value = sampleAdc();
        uint8_t phase = 0;
#if MEGATRNG_ENABLE_TIMER1_PHASE
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            phase = (uint8_t)(TCNT1L ^ TCNT1H);
        }
#endif
        mixSample(value, phase, 0, 0);
        if (healthFault_) {
            break;
        }
    }
    if (healthFault_) {
        end();
        return false;
    }
    return true;
}

void TRNG::end() {
    if (!started_) {
        return;
    }
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (watchdogClaimed_) {
            restoreWatchdog();
            watchdogClaimed_ = 0;
        }
        if (active_ == this) {
            active_ = 0;
        }
        restoreHardware();
        started_ = false;
        timerClaimed_ = 0;
    }
}

void TRNG::configureAdc() {
    const uint8_t channel = config_.adcChannel;
    ADMUX = (uint8_t)(_BV(REFS0) | (channel & 0x0F));
#if defined(MUX5)
    ADCSRB = (uint8_t)((ADCSRB & (uint8_t)~_BV(MUX5)) |
                       ((channel & 0x08U) ? _BV(MUX5) : 0));
#endif
    // ADTS=0 selects free running mode when ADATE is set.
#if defined(ADTS0)
    ADCSRB = (uint8_t)(ADCSRB & (uint8_t)~(_BV(ADTS0) | _BV(ADTS1) |
                                           _BV(ADTS2)));
#endif
#if defined(DIDR0)
    if (channel < 8) {
        DIDR0 = (uint8_t)(DIDR0 | _BV(channel));
    }
#endif
#if defined(DIDR2)
    if (channel >= 8) {
        DIDR2 = (uint8_t)(DIDR2 | _BV(channel - 8));
    }
#endif

    uint8_t control = (uint8_t)(_BV(ADEN) | (config_.adcPrescaler & 0x07U));
#if defined(ADATE)
    if (config_.freeRunning) {
        control = (uint8_t)(control | _BV(ADATE));
    }
#endif
    ADCSRA = control;
    ADCSRA |= _BV(ADIF); // clear a possible stale completion flag
    if (config_.freeRunning) {
        // In free-running mode ADSC starts the first conversion and the ADC
        // hardware starts every following conversion automatically.
        ADCSRA |= _BV(ADSC);
    }
}

void TRNG::configureTimer1() {
    TCCR1A = 0;
    TCCR1B = _BV(CS10); // 16 MHz free-running phase counter
    TCCR1C = 0;
    TCNT1 = 0;
    TIMSK1 = 0;
}

void TRNG::configureWatchdog() {
#if MEGATRNG_ENABLE_WATCHDOG
    savedWdtcsr_ = WDTCSR;
    MCUSR = (uint8_t)(MCUSR & (uint8_t)~_BV(WDRF));
    // WDCE/WDE must be written in the four-cycle configuration window.  A
    // zero WDP field is the shortest (nominally 16 ms) watchdog period.
    WDTCSR = (uint8_t)(_BV(WDCE) | _BV(WDE));
    WDTCSR = _BV(WDIE);
#endif
}

void TRNG::restoreWatchdog() {
#if MEGATRNG_ENABLE_WATCHDOG
    MCUSR = (uint8_t)(MCUSR & (uint8_t)~_BV(WDRF));
    WDTCSR = (uint8_t)(_BV(WDCE) | _BV(WDE));
    WDTCSR = savedWdtcsr_;
#endif
}

void TRNG::restoreHardware() {
    ADMUX = savedAdmux_;
    ADCSRA = savedAdcsra_;
    ADCSRB = savedAdcsrb_;
#if defined(DIDR0)
    DIDR0 = savedDidr0_;
#endif
#if defined(DIDR2)
    DIDR2 = savedDidr2_;
#endif
    if (timerClaimed_) {
        TCCR1A = savedTccr1a_;
        TCCR1B = savedTccr1b_;
        TCCR1C = savedTccr1c_;
        TCNT1 = savedTcnt1_;
        TIMSK1 = savedTimsk1_;
    }
}

uint16_t TRNG::sampleAdc() {
    if (config_.freeRunning) {
        while ((ADCSRA & _BV(ADIF)) == 0) {
            // The watchdog ISR remains enabled while the ADC is converting.
        }
        const uint16_t value = ADC;
        // Writing one clears ADIF and leaves the free-running trigger alive.
        ADCSRA |= _BV(ADIF);
        return value;
    }

    ADCSRA |= _BV(ADSC);
    while ((ADCSRA & _BV(ADSC)) != 0) {
        // The watchdog ISR remains enabled while the ADC is converting.
    }
    return ADC;
}

void TRNG::processSample(uint16_t value) {
    uint8_t phase = 0;
#if MEGATRNG_ENABLE_WATCHDOG
    uint8_t watchdogMix = 0;
    uint8_t watchdogEvents = 0;
#else
    const uint8_t watchdogMix = 0;
    const uint8_t watchdogEvents = 0;
#endif
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
#if MEGATRNG_ENABLE_TIMER1_PHASE
        phase = (uint8_t)(TCNT1L ^ TCNT1H);
#endif
#if MEGATRNG_ENABLE_WATCHDOG
        watchdogMix = g_wdtMix;
        watchdogEvents = g_wdtEvents;
        g_wdtEvents = 0;
#endif
    }

    // Timer phase and watchdog phase season a rolling digest.  The debiased
    // bits still come only from ADC bit planes; deterministic counters never
    // become an entropy source by being mixed in here.
    mixSample(value, phase, watchdogMix, watchdogEvents);

#if MEGATRNG_ENABLE_WIDE_PLANES
    const uint8_t mask = config_.bitPlaneMask;
    for (uint8_t plane = 0; plane < 4U; ++plane) {
        const uint8_t flag = (uint8_t)(1U << plane);
        if ((mask & flag) != 0) {
            acceptPlaneBit(plane, (uint8_t)((value >> plane) & 1U));
        }
    }
#else
    acceptPlaneBit(0, (uint8_t)(value & 1U));
#endif
}

void TRNG::acceptPlaneBit(uint8_t plane, uint8_t bit) {
    const uint8_t flag = (uint8_t)(1U << plane);
    if ((havePreviousBits_ & flag) == 0) {
        if (bit != 0) {
            previousBits_ = (uint8_t)(previousBits_ | flag);
        } else {
            previousBits_ = (uint8_t)(previousBits_ & (uint8_t)~flag);
        }
        havePreviousBits_ = (uint8_t)(havePreviousBits_ | flag);
        return;
    }

    const uint8_t first = (previousBits_ & flag) != 0 ? 1U : 0U;
    havePreviousBits_ = (uint8_t)(havePreviousBits_ & (uint8_t)~flag);
    if (bit == first) {
        return;
    }

    ++acceptedBits_;
    // At most four accepted bits can be generated by one ADC conversion and
    // next() drains the reservoir before taking another sample.
    pendingValue_ = (uint16_t)((pendingValue_ << 1) | first);
    ++pendingBits_;
}

void TRNG::mixSample(uint16_t value, uint8_t phase, uint8_t watchdogMix,
                     uint8_t watchdogEvents) {
    uint32_t digest = sampleDigest_;
    digest = (uint32_t)((digest << 5) | (digest >> 27));
    digest += (uint32_t)((uint8_t)value ^ (uint8_t)(value >> 8) ^ phase) +
              0x9E3779B9UL;
    digest ^= digest >> 7;
#if MEGATRNG_ENABLE_WATCHDOG
    if (watchdogEvents != 0) {
        digest ^= (uint32_t)watchdogMix +
                  ((uint32_t)watchdogEvents << 24);
        digest = (uint32_t)((digest << 3) | (digest >> 29));
    }
#else
    (void)watchdogMix;
    (void)watchdogEvents;
#endif
    sampleDigest_ = digest;

    // Health history is deliberately updated after the digest so it cannot
    // accidentally become the source of output bits.
#if MEGATRNG_ENABLE_WIDE_PLANES
    const uint8_t mask = config_.bitPlaneMask;
    for (uint8_t plane = 0; plane < 4U; ++plane) {
        const uint8_t flag = (uint8_t)(1U << plane);
        if ((mask & flag) == 0) {
            continue;
        }
        const uint8_t bit = (uint8_t)((value >> plane) & 1U);
        const uint8_t previous = (uint8_t)((lastHealthBits_ >> plane) & 1U);
        if ((healthHaveBits_ & flag) == 0) {
            healthHaveBits_ = (uint8_t)(healthHaveBits_ | flag);
            lastHealthBits_ = (uint8_t)((lastHealthBits_ & (uint8_t)~flag) |
                                        (bit != 0 ? flag : 0));
            healthRunLength_[plane] = 1;
        } else if (bit == previous) {
            if (healthRunLength_[plane] != 0xFFU) {
                ++healthRunLength_[plane];
            }
        } else {
            lastHealthBits_ = (uint8_t)((lastHealthBits_ & (uint8_t)~flag) |
                                        (bit != 0 ? flag : 0));
            healthRunLength_[plane] = 1;
        }
        if (healthRunLength_[plane] >= 128U) {
            healthFault_ = true;
        }
        if (bit != 0) {
            ++healthWindowOnes_[plane];
        }
    }
#else
    const uint8_t bit = (uint8_t)(value & 1U);
    const uint8_t previous = (uint8_t)(lastHealthBits_ & 1U);
    if ((healthHaveBits_ & 1U) == 0) {
        healthHaveBits_ |= 1U;
        lastHealthBits_ = (uint8_t)(bit != 0 ? 1U : 0U);
        healthRunLength_[0] = 1;
    } else if (bit == previous) {
        if (healthRunLength_[0] != 0xFFU) {
            ++healthRunLength_[0];
        }
    } else {
        lastHealthBits_ = (uint8_t)(bit != 0 ? 1U : 0U);
        healthRunLength_[0] = 1;
    }
    if (healthRunLength_[0] >= 128U) {
        healthFault_ = true;
    }
    if (bit != 0) {
        ++healthWindowOnes_[0];
    }
#endif
    ++healthWindowBits_;
    if (healthWindowBits_ == 0) {
#if MEGATRNG_ENABLE_WIDE_PLANES
        const uint8_t mask = config_.bitPlaneMask;
        for (uint8_t plane = 0; plane < 4U; ++plane) {
            const uint8_t flag = (uint8_t)(1U << plane);
            if ((mask & flag) != 0 &&
                (healthWindowOnes_[plane] < 16U ||
                 healthWindowOnes_[plane] > 240U)) {
                healthFault_ = true;
            }
            healthWindowOnes_[plane] = 0;
        }
#else
        if (healthWindowOnes_[0] < 16U || healthWindowOnes_[0] > 240U) {
            healthFault_ = true;
        }
        healthWindowOnes_[0] = 0;
#endif
        healthWindowBits_ = 0;
    }
    ++rawSamples_;
}

void TRNG::absorbByte(uint8_t value) {
    s0_ ^= (uint32_t)value + 0x9E3779B9UL + (s3_ << 6) + (s3_ >> 2);
    s1_ += MEGATRNG_ROTL32(s0_, 5) ^ 0xA5A5A5A5UL;
    s2_ ^= MEGATRNG_ROTL32(s1_ + (uint32_t)value, 11);
    s3_ += MEGATRNG_ROTL32(s2_ ^ s0_, 17) + 0x7F4A7C15UL;
    s0_ = MEGATRNG_ROTL32(s0_ + s3_, 7);
    s1_ ^= MEGATRNG_ROTL32(s2_, 13);
    s2_ += MEGATRNG_ROTL32(s3_, 19);
    s3_ ^= MEGATRNG_ROTL32(s0_, 23);
}

uint8_t TRNG::squeezeByte(uint8_t input) {
    // One ARX absorption per output is sufficient for this small mixer and
    // removes a second full round from the hot next() path.
    absorbByte(input);
    uint32_t value = s0_ ^ MEGATRNG_ROTL32(s1_, 7) ^
                     MEGATRNG_ROTL32(s2_, 13) ^ MEGATRNG_ROTL32(s3_, 21);
    value ^= value >> 16;
    value *= 0x7FEB352DUL;
    value ^= value >> 15;
    return (uint8_t)value;
}

bool TRNG::next(uint8_t &out, uint16_t maxRawSamples) {
    if (!ready()) {
        return false;
    }

    uint8_t outputBits = 0;
    uint16_t samples = 0;
    out = 0;
    while (outputBits < 8U) {
        // Drain accepted bits left over from a multi-plane sample first.  In
        // turbo mode this is the common zero-conversion latency case.
        while (pendingBits_ != 0 && outputBits < 8U) {
            const uint8_t shift = (uint8_t)(pendingBits_ - 1U);
            out = (uint8_t)((out << 1) | ((pendingValue_ >> shift) & 1U));
            --pendingBits_;
            if (shift == 0) {
                pendingValue_ = 0;
            } else {
                pendingValue_ = (uint16_t)(pendingValue_ &
                                            (uint16_t)((1U << shift) - 1U));
            }
            ++outputBits;
        }
        if (outputBits == 8U) {
            break;
        }
        if (maxRawSamples != 0 && samples >= maxRawSamples) {
            return false;
        }
        processSample(sampleAdc());
        ++samples;
        if (healthFault_) {
            return false;
        }
    }

    const uint8_t input = (uint8_t)sampleDigest_ ^
                          (uint8_t)(sampleDigest_ >> 8) ^ out ^
                          (uint8_t)acceptedBits_ ^
                          (uint8_t)(acceptedBits_ >> 8);
    out = (uint8_t)(out ^ squeezeByte(input));
    return true;
}

uint8_t TRNG::next() {
    uint8_t value = 0;
    (void)next(value, 0);
    return value;
}

size_t TRNG::fill(void *buffer, size_t length) {
    if (buffer == 0 || !ready()) {
        return 0;
    }
    uint8_t *bytes = static_cast<uint8_t *>(buffer);
    size_t written = 0;
    while (written < length) {
        if (!next(bytes[written], 0)) {
            break;
        }
        ++written;
    }
    return written;
}

void TRNG::onWatchdogInterrupt() {
#if MEGATRNG_ENABLE_WATCHDOG
    if (active_ == 0 || !active_->watchdogClaimed_) {
        return;
    }
    uint8_t sample = ++g_wdtSequence;
#if MEGATRNG_ENABLE_TIMER1_PHASE
    uint8_t low = TCNT1L;
    uint8_t high = TCNT1H;
    sample = (uint8_t)(sample ^ low ^ high);
#endif
    g_wdtMix = (uint8_t)((g_wdtMix << 3) | (g_wdtMix >> 5));
    g_wdtMix ^= (uint8_t)(sample + 0xA7U);
    ++g_wdtEvents;
    // ATmega2560 clears WDIE automatically when an interrupt is serviced.
    // Re-arm it so the independent oscillator contributes periodically.
    WDTCSR |= _BV(WDIE);
#endif
}

#if MEGATRNG_ENABLE_WATCHDOG
ISR(WDT_vect) {
    TRNG::onWatchdogInterrupt();
}
#endif

#undef MEGATRNG_ROTL32

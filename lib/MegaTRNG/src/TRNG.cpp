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
    // Turbo is intended to move data, not to add a slow timing side channel.
    // Applications that need the extra seasoning can turn these back on.
    config.enableWatchdog = 0;
    config.claimTimer1 = 0;
    return config;
}

TRNG::TRNG()
    : config_(),
      started_(false),
      healthFault_(false),
      previousBits_(0),
      havePreviousSample_(false),
      lastHealthBits_(0),
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
#if MEGATRNG_ENABLE_TIMER1_PHASE
    if (config_.claimTimer1) {
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            savedTccr1a_ = TCCR1A;
            savedTccr1b_ = TCCR1B;
            savedTccr1c_ = TCCR1C;
            savedTcnt1_ = TCNT1;
            savedTimsk1_ = TIMSK1;
        }
    }
#endif

    s0_ = 0x243F6A88UL;
    s1_ = 0x85A308D3UL;
    s2_ = 0x13198A2EUL;
    s3_ = 0x03707344UL;
    previousBits_ = 0;
    havePreviousSample_ = false;
    lastHealthBits_ = 0;
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
        if (timerClaimed_ != 0) {
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                phase = (uint8_t)(TCNT1L ^ TCNT1H);
            }
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
    // MUX5 selects the upper bank. MUX3 must stay clear for ADC8..15;
    // setting it would select a differential input instead.
    ADCSRA = 0;
    ADMUX = (uint8_t)(_BV(REFS0) | (channel & 0x07));
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
    wdt_reset();
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
    wdt_reset();
    MCUSR = (uint8_t)(MCUSR & (uint8_t)~_BV(WDRF));
    WDTCSR = (uint8_t)(_BV(WDCE) | _BV(WDE));
    WDTCSR = savedWdtcsr_;
#endif
}

void TRNG::restoreHardware() {
    // Stop free-running conversions before restoring the mux/reference.
    ADCSRA = 0;
    ADMUX = savedAdmux_;
    ADCSRB = savedAdcsrb_;
    ADCSRA = savedAdcsra_;
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
#if MEGATRNG_ENABLE_TIMER1_PHASE || MEGATRNG_ENABLE_WATCHDOG
    // A collector can disable both seasoning sources.  Avoid the CLI/SEI
    // pair in that configuration; this function is on the hottest path.
    if (timerClaimed_ != 0 || watchdogClaimed_ != 0) {
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
#if MEGATRNG_ENABLE_TIMER1_PHASE
            if (timerClaimed_ != 0) {
                phase = (uint8_t)(TCNT1L ^ TCNT1H);
            }
#endif
#if MEGATRNG_ENABLE_WATCHDOG
            if (watchdogClaimed_ != 0) {
                watchdogMix = g_wdtMix;
                watchdogEvents = g_wdtEvents;
                g_wdtEvents = 0;
            }
#endif
        }
    }
#endif

    // Timer phase and watchdog phase season a rolling digest.  The debiased
    // bits still come only from ADC bit planes; deterministic counters never
    // become an entropy source by being mixed in here.
    mixSample(value, phase, watchdogMix, watchdogEvents);

    if (!healthFault_) {
        acceptSample((uint8_t)value);
    }
}

void TRNG::acceptSample(uint8_t value) {
    // All enabled planes observe the same conversions, so their pair
    // boundaries are identical. One XOR finds all unequal VN pairs at once.
    if (!havePreviousSample_) {
        previousBits_ = value;
        havePreviousSample_ = true;
        return;
    }
    havePreviousSample_ = false;
    const uint8_t accepted = (uint8_t)((previousBits_ ^ value) & config_.bitPlaneMask);
    // Fixed shifts compile to AVR bit instructions, avoiding variable shifts
    // and per-plane pair bookkeeping. Preserve ascending plane order.
    if (accepted & 0x01U) appendBit(previousBits_ & 1U);
#if MEGATRNG_ENABLE_WIDE_PLANES
    if (accepted & 0x02U) appendBit((previousBits_ >> 1) & 1U);
    if (accepted & 0x04U) appendBit((previousBits_ >> 2) & 1U);
    if (accepted & 0x08U) appendBit((previousBits_ >> 3) & 1U);
#endif
}

void TRNG::appendBit(uint8_t bit) {
    ++acceptedBits_;
    // Before a conversion there are at most seven pending bits; a conversion
    // adds at most four, so the 16-bit reservoir cannot overflow.
    pendingValue_ = (uint16_t)((pendingValue_ << 1) | bit);
    ++pendingBits_;
}

template <uint8_t Plane>
void TRNG::inspectPlane(uint8_t value) {
    const uint8_t flag = (uint8_t)(1U << Plane);
    if (((value ^ lastHealthBits_) & flag) == 0) {
        ++healthRunLength_[Plane];
    } else {
        healthRunLength_[Plane] = 1;
    }
    if (healthRunLength_[Plane] >= 128U) {
        healthFault_ = true;
    }
    healthWindowOnes_[Plane] += (value & flag) != 0;
    if (healthWindowBits_ == 0) {
        if (healthWindowOnes_[Plane] < 16U || healthWindowOnes_[Plane] > 240U) {
            healthFault_ = true;
        }
        healthWindowOnes_[Plane] = 0;
    }
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

    // Keep every health check, using fixed plane offsets on this 8-bit CPU.
    // A 256-one window cannot overflow unnoticed: the run test stops at 128.
    ++healthWindowBits_;
#if MEGATRNG_ENABLE_WIDE_PLANES
    const uint8_t mask = config_.bitPlaneMask;
    if (mask & 0x01U) inspectPlane<0>((uint8_t)value);
    if (mask & 0x02U) inspectPlane<1>((uint8_t)value);
    if (mask & 0x04U) inspectPlane<2>((uint8_t)value);
    if (mask & 0x08U) inspectPlane<3>((uint8_t)value);
#else
    inspectPlane<0>((uint8_t)value);
#endif
    lastHealthBits_ = (uint8_t)value;
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

    uint16_t samples = 0;
    while (pendingBits_ < 8U) {
        if (maxRawSamples != 0 && samples >= maxRawSamples) {
            return false;
        }
        processSample(sampleAdc());
        ++samples;
        if (healthFault_) {
            return false;
        }
    }

    // Extract a whole byte once. Leave partial output in the reservoir when
    // a bounded call times out so small cooperative reads always make progress.
    pendingBits_ -= 8U;
    const uint8_t extracted = (uint8_t)(pendingValue_ >> pendingBits_);
    pendingValue_ &= (uint16_t)((1U << pendingBits_) - 1U);
    const uint8_t input = (uint8_t)sampleDigest_ ^
                          (uint8_t)(sampleDigest_ >> 8) ^ extracted ^
                          (uint8_t)acceptedBits_ ^
                          (uint8_t)(acceptedBits_ >> 8);
    out = (uint8_t)(extracted ^ squeezeByte(input));
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
    if (active_->timerClaimed_ != 0) {
        uint8_t low = TCNT1L;
        uint8_t high = TCNT1H;
        sample = (uint8_t)(sample ^ low ^ high);
    }
#endif
    g_wdtMix = (uint8_t)((g_wdtMix << 3) | (g_wdtMix >> 5));
    g_wdtMix ^= (uint8_t)(sample + 0xA7U);
    ++g_wdtEvents;
    // Interrupt-only mode (WDE=0) keeps WDIE set after the ISR. Automatic
    // clearing only applies to combined interrupt-and-reset mode.
#endif
}

#if MEGATRNG_ENABLE_WATCHDOG
ISR(WDT_vect) {
    TRNG::onWatchdogInterrupt();
}
#endif

#undef MEGATRNG_ROTL32

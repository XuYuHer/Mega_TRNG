#include "TRNG.h"

#include <avr/interrupt.h>
#if MEGATRNG_ENABLE_WATCHDOG
#include <avr/wdt.h>
#endif
#include <util/atomic.h>

namespace {
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
      claimTimer1(1) {}

TRNG::TRNG()
    : config_(),
      started_(false),
      healthFault_(false),
      previousBit_(0),
      havePrevious_(0),
      lastRawBit_(0),
      rawRunLength_(0),
      windowBits_(0),
      windowOnes_(0),
      rawSamples_(0),
      acceptedBits_(0),
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

uint32_t TRNG::rotl32(uint32_t value, uint8_t amount) {
    return (uint32_t)((value << amount) | (value >> (32U - amount)));
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
    if (config.adcChannel > 15 || config.adcPrescaler > 7) {
        return false;
    }

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
    previousBit_ = 0;
    havePrevious_ = 0;
    lastRawBit_ = 0;
    rawRunLength_ = 0;
    windowBits_ = 0;
    windowOnes_ = 0;
    rawSamples_ = 0;
    acceptedBits_ = 0;
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
        absorbByte((uint8_t)value ^ (uint8_t)(value >> 8) ^ phase);
        inspectRawBit((uint8_t)(value & 1U));
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
    uint8_t channel = config_.adcChannel;
    ADMUX = (uint8_t)(_BV(REFS0) | (channel & 0x0F));
#if defined(MUX5)
    ADCSRB = (uint8_t)((ADCSRB & (uint8_t)~_BV(MUX5)) |
                       ((channel & 0x08U) ? _BV(MUX5) : 0));
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
    ADCSRA = (uint8_t)(_BV(ADEN) | (config_.adcPrescaler & 0x07U));
    ADCSRA |= _BV(ADIF); // clear a possible stale completion flag
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
    ADCSRA |= _BV(ADSC);
    while ((ADCSRA & _BV(ADSC)) != 0) {
        // The watchdog ISR remains enabled while the ADC is converting.
    }
    return ADC;
}

uint8_t TRNG::sampleRawBit() {
    uint16_t value = sampleAdc();
    uint8_t phase = 0;
#if MEGATRNG_ENABLE_WATCHDOG
    uint8_t watchdogMix = 0;
    uint8_t watchdogEvents = 0;
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

    // Timer phase and watchdog phase are seasoning for the state mixer.  The
    // debiased bit itself comes from the ADC's physical LSB; this avoids
    // presenting a deterministic counter as entropy.
    absorbByte((uint8_t)value ^ (uint8_t)(value >> 8) ^ phase);
#if MEGATRNG_ENABLE_WATCHDOG
    if (watchdogEvents != 0) {
        absorbByte((uint8_t)(watchdogMix ^ phase ^ watchdogEvents));
    }
#endif
    uint8_t raw = (uint8_t)(value & 1U);
    inspectRawBit(raw);
    return raw;
}

void TRNG::inspectRawBit(uint8_t bit) {
    bit &= 1U;
    if (rawSamples_ == 0) {
        lastRawBit_ = bit;
        rawRunLength_ = 1;
    } else if (bit == lastRawBit_) {
        if (rawRunLength_ != 0xFFU) {
            ++rawRunLength_;
        }
    } else {
        lastRawBit_ = bit;
        rawRunLength_ = 1;
    }

    // A long identical run catches an open circuit, a shorted input, and a
    // dead ADC without rejecting ordinary analogue bias.
    if (rawRunLength_ >= 128U) {
        healthFault_ = true;
    }

    ++windowBits_;
    if (bit != 0) {
        ++windowOnes_;
    }
    if (windowBits_ == 0) {
        // uint8_t wrap means a 256-sample window has completed.
        if (windowOnes_ < 16U || windowOnes_ > 240U) {
            healthFault_ = true;
        }
        windowBits_ = 0;
        windowOnes_ = 0;
    }
    ++rawSamples_;
}

void TRNG::absorbByte(uint8_t value) {
    s0_ ^= (uint32_t)value + 0x9E3779B9UL + (s3_ << 6) + (s3_ >> 2);
    s1_ += rotl32(s0_, 5) ^ 0xA5A5A5A5UL;
    s2_ ^= rotl32(s1_ + (uint32_t)value, 11);
    s3_ += rotl32(s2_ ^ s0_, 17) + 0x7F4A7C15UL;
    s0_ = rotl32(s0_ + s3_, 7);
    s1_ ^= rotl32(s2_, 13);
    s2_ += rotl32(s3_, 19);
    s3_ ^= rotl32(s0_, 23);
}

void TRNG::absorbBit(uint8_t bit) {
    ++acceptedBits_;
    s0_ ^= (uint32_t)(bit & 1U) + 0xD1B54A35UL + acceptedBits_;
    s1_ += rotl32(s0_, 5);
    s2_ ^= rotl32(s1_, 11);
    s3_ += rotl32(s2_, 17);
    s0_ = rotl32(s0_ + s3_, 7);
    s1_ ^= rotl32(s2_, 13);
}

uint8_t TRNG::squeezeByte() {
    // Two short ARX rounds provide diffusion and make consecutive output
    // bytes depend on the entire accumulated state.  This is a mixer, not a
    // claim of cryptographic proof or a replacement for an entropy source.
    absorbByte((uint8_t)(acceptedBits_ ^ (acceptedBits_ >> 8)));
    uint32_t value = s0_ ^ rotl32(s1_, 7) ^ rotl32(s2_, 13) ^ rotl32(s3_, 21);
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
        if (maxRawSamples != 0 && samples >= maxRawSamples) {
            return false;
        }
        uint8_t raw = sampleRawBit();
        ++samples;
        if (healthFault_) {
            return false;
        }

        if (!havePrevious_) {
            previousBit_ = raw;
            havePrevious_ = 1;
            continue;
        }

        uint8_t first = previousBit_;
        havePrevious_ = 0; // non-overlapping pairs are essential to VN bias removal
        if (raw == first) {
            continue;
        }
        absorbBit(first); // 01 -> 0, 10 -> 1
        ++outputBits;
        out = (uint8_t)((out << 1) | first);
    }
    out = (uint8_t)(out ^ squeezeByte());
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

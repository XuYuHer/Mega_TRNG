#ifndef MEGA_TRNG_H
#define MEGA_TRNG_H

/*
 * MegaTRNG - physical-noise random bytes for ATmega2560.
 *
 * The default source is the least-significant bit of an ADC conversion on a
 * deliberately floating analogue input.  A free-running Timer1 is sampled
 * at each conversion, and the independent watchdog oscillator periodically
 * contributes its interrupt phase.  The ADC bit stream is Von Neumann
 * debiased before it reaches the small ARX state mixer.
 *
 * This library is intentionally limited to the AVR Mega2560 target.  It does
 * not claim entropy when the configured analogue input is tied to a quiet,
 * driven voltage.  See the project README for wiring and validation notes.
 */

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#if !defined(__AVR_ATmega2560__)
#error "MegaTRNG currently targets ATmega2560 (Arduino Mega 2560)"
#endif

// Set either macro to 0 in build_flags when the corresponding seasoning path
// is not needed.  The defaults keep the complete dual-source implementation.
#ifndef MEGATRNG_ENABLE_WATCHDOG
#define MEGATRNG_ENABLE_WATCHDOG 1
#endif

#ifndef MEGATRNG_ENABLE_TIMER1_PHASE
#define MEGATRNG_ENABLE_TIMER1_PHASE 1
#endif

// Set to 0 to compile out the experimental multi-plane backend.  The public
// Config field remains source-compatible, but only mask 0x01 is accepted.
#ifndef MEGATRNG_ENABLE_WIDE_PLANES
#define MEGATRNG_ENABLE_WIDE_PLANES 1
#endif

class TRNG {
public:
    enum : uint8_t {
        ADC_DIV_2   = 0,
        ADC_DIV_4   = 2,
        ADC_DIV_8   = 3,
        ADC_DIV_16  = 4,
        ADC_DIV_32  = 5,
        ADC_DIV_64  = 6,
        ADC_DIV_128 = 7
    };

    struct Config {
        uint8_t adcChannel;       // 0..15; use an unconnected A0..A15 pin
        uint8_t adcPrescaler;     // ADPS bits; ADC_DIV_16 is the fast default
        uint16_t warmupSamples;   // conversions absorbed before first output
        uint8_t enableWatchdog;   // independent watchdog phase seasoning
        uint8_t claimTimer1;      // configure Timer1 as a free-running counter
        uint8_t freeRunning;      // ADC free-running backend (lower per-sample overhead)
        uint8_t bitPlaneMask;     // ADC bit planes 0..3; 0x01 is conservative

        Config();

        // Fast keeps the conventional /16 ADC clock but starts conversions
        // continuously, removing one ADSC setup per sample.
        static Config fast();

        // Turbo is an opt-in experiment: /2 ADC clock and four low bit planes.
        // The ADC clock is outside the usual 50..200 kHz accuracy range and
        // cross-plane independence must be validated on the user's hardware.
        static Config turbo();
    };

    TRNG();
    ~TRNG();

    // begin() owns the ADC and (by default) Timer1 for this instance.
    bool begin();
    bool begin(const Config &config);
    void end();

    bool started() const { return started_; }
    bool healthFault() const { return healthFault_; }
    uint32_t rawSamples() const { return rawSamples_; }
    uint32_t acceptedBits() const { return acceptedBits_; }
    uint8_t bitPlaneMask() const { return config_.bitPlaneMask; }

    /*
     * Read one byte.  maxRawSamples == 0 waits until eight debiased bits are
     * available.  A non-zero limit makes the call bounded and is useful in a
     * cooperative loop.  The function returns false on a health fault or when
     * the bound expires.  No pseudo-random fallback is used.
     */
    bool next(uint8_t &out, uint16_t maxRawSamples = 0);

    // Convenience wrapper: returns zero only when next() reports failure.
    uint8_t next();

    // Fill as many bytes as possible; stops on a health fault.
    size_t fill(void *buffer, size_t length);

    // A cheap status probe for applications that use a bounded next() call.
    bool ready() const { return started_ && !healthFault_; }

    // Called by the global WDT_vect ISR.  Do not call from application code.
    static void onWatchdogInterrupt();

private:
    TRNG(const TRNG &);
    TRNG &operator=(const TRNG &);

    Config config_;
    bool started_;
    volatile bool healthFault_;
    uint8_t previousBits_;
    uint8_t havePreviousBits_;
    uint8_t lastHealthBits_;
    uint8_t healthHaveBits_;
    uint8_t healthWindowBits_;
    uint8_t healthRunLength_[4];
    uint8_t healthWindowOnes_[4];
    uint32_t rawSamples_;
    uint32_t acceptedBits_;
    uint32_t sampleDigest_;

    // A wide sample can yield more than one accepted bit. Keep surplus bits
    // so no physical sample is silently discarded at a byte boundary.
    uint16_t pendingValue_;
    uint8_t pendingBits_;

    uint32_t s0_;
    uint32_t s1_;
    uint32_t s2_;
    uint32_t s3_;

    uint8_t savedAdmux_;
    uint8_t savedAdcsra_;
    uint8_t savedAdcsrb_;
    uint8_t savedDidr0_;
    uint8_t savedDidr2_;
    uint8_t savedTccr1a_;
    uint8_t savedTccr1b_;
    uint8_t savedTccr1c_;
    uint16_t savedTcnt1_;
    uint8_t savedTimsk1_;
    uint8_t savedWdtcsr_;
    uint8_t timerClaimed_;
    uint8_t watchdogClaimed_;

    static TRNG *active_;

    void configureAdc();
    void restoreHardware();
    void configureTimer1();
    void configureWatchdog();
    void restoreWatchdog();
    uint16_t sampleAdc();
    void processSample(uint16_t value);
    void acceptPlaneBit(uint8_t plane, uint8_t bit);
    void mixSample(uint16_t value, uint8_t phase, uint8_t watchdogMix,
                   uint8_t watchdogEvents);
    void absorbByte(uint8_t value);
    uint8_t squeezeByte(uint8_t input);
};

#endif

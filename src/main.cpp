#include <Arduino.h>
#include <TRNG.h>
#include <util/atomic.h>

TRNG trng;
static uint8_t testBytes[256];

extern "C" {
extern uint8_t _etext;
extern uint8_t __data_start;
extern uint8_t __data_end;
extern uint8_t __bss_start;
extern uint8_t __bss_end;
extern uint8_t __heap_start;
extern uint8_t *__brkval;
}

static void printHex(uint8_t value) {
    if (value < 0x10) {
        Serial.print('0');
    }
    Serial.print(value, HEX);
}

static int freeRam() {
    uint8_t marker;
    const uintptr_t stackAddress = reinterpret_cast<uintptr_t>(&marker);
    const uintptr_t heapAddress = __brkval == 0
                                      ? reinterpret_cast<uintptr_t>(&__heap_start)
                                      : reinterpret_cast<uintptr_t>(__brkval);
    return static_cast<int>(stackAddress - heapAddress);
}

static void printMemoryReport() {
    const uintptr_t dataStart = reinterpret_cast<uintptr_t>(&__data_start);
    const uintptr_t dataEnd = reinterpret_cast<uintptr_t>(&__data_end);
    const uintptr_t bssStart = reinterpret_cast<uintptr_t>(&__bss_start);
    const uintptr_t bssEnd = reinterpret_cast<uintptr_t>(&__bss_end);
    const uint16_t dataBytes = static_cast<uint16_t>(dataEnd - dataStart);
    const uint16_t bssBytes = static_cast<uint16_t>(bssEnd - bssStart);
    const uint32_t flashBytes = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&_etext)) +
                                dataBytes;
    Serial.print(F("Flash used (linker symbols): "));
    Serial.print(flashBytes);
    Serial.println(F(" bytes; PlatformIO `pio run` is authoritative."));
    Serial.print(F("Static RAM (.data + .bss): "));
    Serial.print((uint16_t)(dataBytes + bssBytes));
    Serial.print(F(" bytes; free RAM now: "));
    Serial.print(freeRam());
    Serial.println(F(" bytes"));
    Serial.print(F("TRNG object: "));
    Serial.print((unsigned)sizeof(TRNG));
    Serial.println(F(" bytes (plus caller buffers)"));
}

static uint16_t timer1Value() {
    uint8_t low = 0;
    uint8_t high = 0;
    // cppcheck-suppress cstyleCast
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        // The AVR SFR macros expand to volatile pointer casts by definition.
        // cppcheck-suppress cstyleCast
        low = TCNT1L;
        // cppcheck-suppress cstyleCast
        high = TCNT1H;
    }
    return static_cast<uint16_t>(low) | (static_cast<uint16_t>(high) << 8);
}

static void printRandomLine(uint8_t count) {
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t value = 0;
        if (!trng.next(value, 0)) {
            Serial.println(F("[TRNG health fault]"));
            return;
        }
        printHex(value);
        Serial.print(i + 1 == count ? '\n' : ' ');
    }
}

static void benchmarkTimer1() {
    const uint8_t count = 16;
    uint32_t cycles = 0;
    for (uint8_t i = 0; i < count; ++i) {
        const uint16_t before = timer1Value();
        uint8_t value = 0;
        if (!trng.next(value, 0)) {
            Serial.println(F("Benchmark stopped by health fault."));
            return;
        }
        const uint16_t after = timer1Value();
        cycles += (uint16_t)(after - before);
    }
    const uint32_t averageCycles = cycles / count;
    const uint32_t cyclesPerUs = F_CPU / 1000000UL;
    const uint32_t wholeUs = averageCycles / cyclesPerUs;
    const uint32_t tenthsUs = (uint32_t)(((averageCycles % cyclesPerUs) * 10UL) /
                                         cyclesPerUs);
    Serial.print(F("Timer1: "));
    Serial.print(wholeUs);
    Serial.print('.');
    Serial.print(tenthsUs);
    Serial.print(F(" us/byte ("));
    Serial.print(averageCycles);
    Serial.println(F(" CPU cycles/byte)"));
    Serial.println(F("The result includes ADC conversion, Von Neumann rejection, and the mixer."));
    Serial.println(F("Timer1 per-byte timing is valid below its 4.096 ms wrap."));
}

static void runSimpleNist() {
    const uint16_t bytes = sizeof(testBytes);
    const uint32_t bits = (uint32_t)bytes * 8UL;
    uint32_t ones = 0;
    uint32_t runs = 0;
    uint8_t previous = 0;
    bool first = true;
    uint16_t blockOnes = 0;
    uint16_t minBlock = 129;
    uint16_t maxBlock = 0;

    if (trng.fill(testBytes, bytes) != bytes) {
        Serial.println(F("NIST sample aborted by health fault."));
        return;
    }
    for (uint16_t i = 0; i < bytes; ++i) {
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint8_t current = (uint8_t)((testBytes[i] >> (7 - bit)) & 1U);
            ones += current;
            blockOnes += current;
            if (first || current != previous) {
                ++runs;
            }
            previous = current;
            first = false;
            if (((i * 8U + bit + 1U) % 128U) == 0) {
                if (blockOnes < minBlock) {
                    minBlock = blockOnes;
                }
                if (blockOnes > maxBlock) {
                    maxBlock = blockOnes;
                }
                blockOnes = 0;
            }
        }
    }

    const bool frequencyPass = (ones * 100UL >= bits * 45UL) &&
                               (ones * 100UL <= bits * 55UL);
    const uint32_t lowRuns = bits * 40UL / 100UL;
    const uint32_t highRuns = bits * 60UL / 100UL;
    const bool runsPass = runs >= lowRuns && runs <= highRuns;
    const bool blockPass = minBlock >= 44U && maxBlock <= 84U;

    Serial.print(F("Monobit frequency: ones="));
    Serial.print(ones);
    Serial.print('/');
    Serial.print(bits);
    Serial.println(frequencyPass ? F(" PASS (45..55%)") : F(" CHECK (45..55%)"));
    Serial.print(F("Runs: "));
    Serial.print(runs);
    Serial.println(runsPass ? F(" PASS (40..60% of bits)") : F(" CHECK (40..60% of bits)"));
    Serial.print(F("128-bit block frequency: min="));
    Serial.print(minBlock);
    Serial.print(F(", max="));
    Serial.println(maxBlock);
    Serial.println(blockPass ? F("Block frequency: PASS (44..84 ones/block)")
                             : F("Block frequency: CHECK (44..84 ones/block)"));
    Serial.println(F("These are smoke tests, not a certification or a proof of entropy."));
}

void setup() {
    Serial.begin(115200);
    const unsigned long serialStart = millis();
    while (!Serial && millis() - serialStart < 2000UL) {
    }

    Serial.println(F("\nMegaTRNG / ATmega2560 hardware demo"));
    Serial.println(F("Leave A0 floating, or connect a documented analogue noise source."));
    Serial.println(F("A driven DC voltage is a health-test failure, by design."));

    TRNG::Config config;
    config.adcChannel = 0;
    config.adcPrescaler = TRNG::ADC_DIV_16;
    config.warmupSamples = 64;
    config.enableWatchdog = 1;
    config.claimTimer1 = 1;

    if (!trng.begin(config)) {
        Serial.println(F("TRNG begin() failed (invalid configuration or another instance)."));
        return;
    }
    Serial.println(F("TRNG started: ADC0 + Timer1 phase + watchdog oscillator seasoning"));
    printMemoryReport();
    Serial.println(F("16 random bytes:"));
    printRandomLine(16);
    benchmarkTimer1();
    runSimpleNist();
    Serial.println(F("Streaming 8-byte lines every 2 s:"));
}

void loop() {
    if (!trng.ready()) {
        Serial.println(F("TRNG is not ready; inspect wiring and healthFault()."));
        delay(1000);
        return;
    }
    printRandomLine(8);
    delay(2000);
}

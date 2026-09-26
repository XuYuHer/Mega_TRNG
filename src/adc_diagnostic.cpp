/* Raw ADC diagnostic for a connected Mega2560. It deliberately bypasses
 * MegaTRNG health checks and never presents these raw values as random bytes.
 * Host command 'R' requests a fixed capture; the host must analyse it. */
#if defined(MEGATRNG_ADC_DIAGNOSTIC)

#include <Arduino.h>

namespace {
constexpr uint32_t kBaud = 1000000UL;
constexpr uint16_t kSamples = 2048U;
constexpr uint8_t kAdcPrescaler = 4U;  // /16, same speed class as fast mode

uint16_t readAdc() {
    ADCSRA |= _BV(ADSC);
    while ((ADCSRA & _BV(ADSC)) != 0) {
    }
    return ADC;
}

void capture() {
    Serial.write((const uint8_t *)"ADCR", 4);
    Serial.write((uint8_t)1U);
    Serial.write((uint8_t)kSamples);
    Serial.write((uint8_t)(kSamples >> 8));
    for (uint16_t i = 0; i < kSamples; ++i) {
        const uint16_t value = readAdc();
        Serial.write((uint8_t)value);
        Serial.write((uint8_t)(value >> 8));
    }
    Serial.flush();
}
}  // namespace

void setup() {
    Serial.begin(kBaud);
    ADMUX = _BV(REFS0);  // AVcc reference, ADC0 (A0)
    ADCSRB = 0;
    DIDR0 |= _BV(ADC0D);
    ADCSRA = (uint8_t)(_BV(ADEN) | kAdcPrescaler);
}

void loop() {
    if (Serial.available() != 0 && Serial.read() == 'R') {
        capture();
    }
}

#endif

#include <TRNG.h>
#include <util/atomic.h>

TRNG trng;

static uint16_t timer1Value() {
  uint8_t low = 0;
  uint8_t high = 0;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    low = TCNT1L;
    high = TCNT1H;
  }
  return static_cast<uint16_t>(low) | (static_cast<uint16_t>(high) << 8);
}

void setup() {
  Serial.begin(115200);
  TRNG::Config config;
  config.adcChannel = 0;
  config.adcPrescaler = TRNG::ADC_DIV_16;
  config.warmupSamples = 64;
  config.enableWatchdog = 1;
  config.claimTimer1 = 1;
  if (!trng.begin(config)) {
    Serial.println(F("begin() failed"));
    return;
  }

  uint32_t cycles = 0;
  const uint8_t count = 16;
  for (uint8_t i = 0; i < count; ++i) {
    const uint16_t before = timer1Value();
    uint8_t value = 0;
    if (!trng.next(value, 0)) {
      Serial.println(F("health fault"));
      return;
    }
    cycles += static_cast<uint16_t>(timer1Value() - before);
  }
  const uint32_t avg = cycles / count;
  Serial.print(F("Timer1 cycles/byte="));
  Serial.println(avg);
  Serial.print(F("us/byte="));
  Serial.println(avg / (F_CPU / 1000000UL));
}

void loop() {}

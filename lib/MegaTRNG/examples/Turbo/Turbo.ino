#include <TRNG.h>
#include <util/atomic.h>

// Turbo is deliberately explicit: it trades the conventional ADC timing
// range for a throughput experiment.  Validate the raw streams on your own
// board before using this mode for any security-sensitive decision.
TRNG trng;

static uint16_t timer1Value() {
  uint8_t low = 0;
  uint8_t high = 0;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    low = TCNT1L;
    high = TCNT1H;
  }
  return static_cast<uint16_t>(low) |
         (static_cast<uint16_t>(high) << 8);
}

void setup() {
  Serial.begin(115200);
  TRNG::Config config = TRNG::Config::turbo();
  config.adcChannel = 0;  // A0: floating or a documented analogue noise source
  if (!trng.begin(config)) {
    Serial.println(F("turbo begin() failed; check A0 and resource ownership"));
    return;
  }

  uint32_t cycles = 0;
  const uint8_t count = 32;
  for (uint8_t i = 0; i < count; ++i) {
    const uint16_t before = timer1Value();
    uint8_t value = 0;
    if (!trng.next(value, 0)) {
      Serial.println(F("health fault; stop"));
      return;
    }
    cycles += static_cast<uint16_t>(timer1Value() - before);
  }

  Serial.print(F("planes=0x"));
  Serial.println(trng.bitPlaneMask(), HEX);
  Serial.print(F("cycles/byte="));
  Serial.println(cycles / count);
  Serial.print(F("us/byte="));
  Serial.println((cycles / count) / (F_CPU / 1000000UL));
  Serial.println(F("Turbo is experimental: /2 ADC clock and four VN candidate planes."));
  Serial.println(F("Do not treat the theoretical floor as a hardware entropy claim."));
}

void loop() {}

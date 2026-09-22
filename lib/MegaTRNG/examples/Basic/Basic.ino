#include <TRNG.h>

TRNG trng;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  TRNG::Config config;
  config.adcChannel = 0;                 // A0: leave floating for the demo
  config.adcPrescaler = TRNG::ADC_DIV_16;
  config.warmupSamples = 64;
  config.enableWatchdog = 1;
  config.claimTimer1 = 1;

  if (!trng.begin(config)) {
    Serial.println(F("MegaTRNG configuration failed"));
    return;
  }
  Serial.println(F("MegaTRNG ready; random bytes:"));
}

void loop() {
  uint8_t value = 0;
  if (trng.next(value, 0)) {
    if (value < 16) Serial.print('0');
    Serial.println(value, HEX);
  } else {
    Serial.println(F("health fault: check that A0 is not driven"));
  }
  delay(100);
}

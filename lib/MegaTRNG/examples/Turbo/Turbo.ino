#include <TRNG.h>
#include <TRNGBenchmark.h>

// Turbo is deliberately explicit: it trades the conventional ADC timing
// range for a throughput experiment.  Validate the raw streams on your own
// board before using this mode for any security-sensitive decision.
TRNG trng;

void setup() {
  Serial.begin(115200);
  TRNG::Config config = TRNG::Config::turbo();
  config.adcChannel = 0;  // A0: floating or a documented analogue noise source
  if (!trng.begin(config)) {
    Serial.println(F("turbo begin() failed; check A0 and resource ownership"));
    return;
  }

  benchmarkTRNG(trng, 512);
  Serial.println(F("Turbo is experimental: /2 ADC clock and four VN candidate planes."));
  Serial.println(F("Do not treat the theoretical floor as a hardware entropy claim."));
}

void loop() {}

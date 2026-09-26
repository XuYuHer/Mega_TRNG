#include <TRNG.h>
#include <TRNGBenchmark.h>

TRNG trng;

void setup() {
  Serial.begin(115200);
  // Same optional sources in all modes; micros() also works in minimal builds.
  for (uint8_t mode = 0; mode < 3; ++mode) {
#if !MEGATRNG_ENABLE_WIDE_PLANES
    if (mode == 2) break;
#endif
    TRNG::Config config = mode == 2 ? TRNG::Config::turbo() : TRNG::Config::fast();
    if (mode == 0) config.freeRunning = 0;
    config.claimTimer1 = 0;
    config.enableWatchdog = 0;
    config.warmupSamples = 256;
    Serial.println(mode == 0 ? F("conservative /16 single-shot") :
                   mode == 1 ? F("fast /16 free-running") :
                               F("turbo /2 four planes (experimental)"));
    if (trng.begin(config)) {
      benchmarkTRNG(trng, 512);
      trng.end();
    } else {
      Serial.println(F("begin failed; check analogue source"));
    }
  }
}

void loop() {}

#ifndef MEGA_TRNG_BENCHMARK_H
#define MEGA_TRNG_BENCHMARK_H

#include <TRNG.h>

// Batch timing works with Timer1 seasoning disabled and includes VN rejection,
// health checks and conditioning. No serial printing occurs inside the batch.
inline bool benchmarkTRNG(TRNG &rng, uint16_t bytes = 512U) {
    if (bytes == 0) return false;
    Serial.flush();
    const uint32_t rawBefore = rng.rawSamples();
    const uint32_t acceptedBefore = rng.acceptedBits();
    const uint32_t start = micros();
    uint8_t value = 0;
    for (uint16_t i = 0; i < bytes; ++i) {
        if (!rng.next(value, 4096U)) {
            Serial.println(rng.healthFault() ? F("Benchmark stopped: health fault")
                                           : F("Benchmark stopped: extraction timeout"));
            return false;
        }
    }
    const uint32_t elapsed = (uint32_t)(micros() - start);
    if (elapsed == 0) return false;
    Serial.print(F("bytes="));
    Serial.print(bytes);
    Serial.print(F(" elapsed_us="));
    Serial.print(elapsed);
    Serial.print(F(" us/byte="));
    Serial.print((float)elapsed / bytes, 2);
    Serial.print(F(" bytes/s="));
    Serial.print((float)bytes * 1000000.0f / elapsed, 2);
    Serial.print(F(" sampled="));
    Serial.print(rng.rawSamples() - rawBefore);
    Serial.print(F(" accepted_bits="));
    Serial.println(rng.acceptedBits() - acceptedBefore);
    return true;
}

#endif

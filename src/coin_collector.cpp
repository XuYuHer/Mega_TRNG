/*
 * Binary collector for the law-of-large-numbers demo.
 *
 * This translation unit is selected by the megaatmega2560_coin environment.
 * It intentionally does no statistical analysis on the board: it packs the
 * bytes produced by MegaTRNG into CRC-protected frames and lets the computer
 * do all counting and plotting.
 */

#if defined(MEGATRNG_COIN_COLLECTOR)

#include <Arduino.h>
#include <TRNG.h>
#include <util/crc16.h>

#ifndef MEGATRNG_SERIAL_BAUD
#define MEGATRNG_SERIAL_BAUD 1000000UL
#endif
#ifndef MEGATRNG_ADC_CHANNEL
#define MEGATRNG_ADC_CHANNEL 0
#endif

namespace {

constexpr uint16_t kPayloadBytes = 240U;
constexpr uint8_t kMetadataBytes = 14U;
constexpr uint8_t kProtocolVersion = 1U;
constexpr uint8_t kFlagError = 0x01U;
constexpr uint8_t kErrorBegin = 1U;
constexpr uint8_t kErrorHealth = 2U;
constexpr uint8_t kErrorTimeout = 3U;
constexpr uint8_t kHeaderBytes = 12U;

TRNG g_trng;
uint8_t g_payload[kMetadataBytes + kPayloadBytes];
uint32_t g_sequence = 0;
uint8_t g_mode = 1U;

void store32(uint8_t *target, uint32_t value) {
    for (uint8_t i = 0; i < 4U; ++i) {
        target[i] = (uint8_t)value;
        value >>= 8;
    }
}

void writeFrame(const uint8_t *payload, uint16_t length, uint8_t flags) {
    uint8_t header[kHeaderBytes];
    header[0] = 'M';
    header[1] = 'L';
    header[2] = 'L';
    header[3] = 'N';
    header[4] = kProtocolVersion;
    header[5] = flags;
    header[6] = (uint8_t)length;
    header[7] = (uint8_t)(length >> 8);
    header[8] = (uint8_t)g_sequence;
    header[9] = (uint8_t)(g_sequence >> 8);
    header[10] = (uint8_t)(g_sequence >> 16);
    header[11] = (uint8_t)(g_sequence >> 24);
    ++g_sequence;

    uint16_t crc = 0xFFFFU;
    // The magic is only a synchronisation marker; version through payload are
    // covered so a lost byte cannot silently turn into a valid sample frame.
    for (uint8_t i = 4U; i < kHeaderBytes; ++i) {
        crc = _crc_xmodem_update(crc, header[i]);
    }
    for (uint16_t i = 0; i < length; ++i) {
        crc = _crc_xmodem_update(crc, payload[i]);
    }

    Serial.write(header, sizeof(header));
    if (length != 0U) {
        Serial.write(payload, length);
    }
    Serial.write((uint8_t)crc);
    Serial.write((uint8_t)(crc >> 8));
}

void writeError(uint8_t code) {
    g_payload[0] = code;
    writeFrame(g_payload, 1U, kFlagError);
    g_trng.end();
}

void startCollection(uint8_t mode) {
    g_trng.end();
    g_sequence = 0;
    g_mode = mode;
    TRNG::Config config = mode == 2U ? TRNG::Config::turbo() : TRNG::Config::fast();
    if (mode == 0U) config.freeRunning = 0;
    config.enableWatchdog = 0;
    config.claimTimer1 = 0;
    config.adcChannel = MEGATRNG_ADC_CHANNEL;
    config.warmupSamples = 256;
    if (!g_trng.begin(config)) {
        writeError(kErrorBegin);
    }
}

}  // namespace

void setup() {
    Serial.begin(MEGATRNG_SERIAL_BAUD);
    // Wait for C/F/T from the host. No random data or text pollutes startup.
}

void loop() {
    while (Serial.available() != 0) {
        switch (Serial.read()) {
            case 'C': startCollection(0); break;
            case 'F': startCollection(1); break;
            case 'T': startCollection(2); break;
            case 'S': g_trng.end(); break;
            default: break;
        }
    }
    if (!g_trng.ready()) return;

    // Keep UART transmission out of the generation timing. Unsigned micros()
    // subtraction is safe across its wrap, unlike a 4.096 ms Timer1 interval.
    Serial.flush();
    const uint32_t rawBefore = g_trng.rawSamples();
    const uint32_t acceptedBefore = g_trng.acceptedBits();
    const uint32_t before = micros();
    for (uint16_t i = 0; i < kPayloadBytes; ++i) {
        if (!g_trng.next(g_payload[kMetadataBytes + i], 4096U)) {
            writeError(g_trng.healthFault() ? kErrorHealth : kErrorTimeout);
            return;
        }
    }
    store32(g_payload, (uint32_t)(micros() - before));
    store32(g_payload + 4, g_trng.rawSamples() - rawBefore);
    store32(g_payload + 8, g_trng.acceptedBits() - acceptedBefore);
    g_payload[12] = g_mode;
    g_payload[13] = MEGATRNG_ADC_CHANNEL;
    writeFrame(g_payload, sizeof(g_payload), 0U);
}

#endif  // MEGATRNG_COIN_COLLECTOR

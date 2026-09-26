#ifndef TEST_FAKE_ARDUINO_H
#define TEST_FAKE_ARDUINO_H
#include <stdint.h>
#include <stddef.h>

// ADC/Timer register shim for deterministic library regression tests.
// It models completed conversions, not analogue timing or physical entropy.
#define _BV(bit) (1U << (bit))
#define REFS0 6
#define MUX5 3
#define ADTS0 0
#define ADTS1 1
#define ADTS2 2
#define ADEN 7
#define ADSC 6
#define ADATE 5
#define ADIF 4
#define CS10 0
#define WDRF 3
#define WDCE 4
#define WDE 3
#define WDIE 6

struct AdcControl {
    uint8_t value;
    operator uint8_t() const { return (value & (uint8_t)~_BV(ADSC)) | _BV(ADIF); }
    AdcControl &operator=(uint8_t v) { value = v; return *this; }
    AdcControl &operator|=(uint8_t v) { value |= v; return *this; }
};
extern AdcControl fakeAdcsra;
extern volatile uint8_t fakeAdmux, fakeAdcsrb, fakeDidr0, fakeDidr2;
extern volatile uint8_t fakeTccr1a, fakeTccr1b, fakeTccr1c, fakeTimsk1;
extern volatile uint8_t fakeWdtcsr, fakeMcusr;
extern volatile uint16_t fakeTcnt1;
extern uint32_t fakeTimerReads;
uint8_t fakeTimerByte(uint8_t shift);
uint16_t fakeAdcRead();
#define ADMUX fakeAdmux
#define ADCSRA fakeAdcsra
#define ADCSRB fakeAdcsrb
#define DIDR0 fakeDidr0
#define DIDR2 fakeDidr2
#define TCCR1A fakeTccr1a
#define TCCR1B fakeTccr1b
#define TCCR1C fakeTccr1c
#define TIMSK1 fakeTimsk1
#define TCNT1 fakeTcnt1
#define TCNT1L fakeTimerByte(0)
#define TCNT1H fakeTimerByte(8)
#define WDTCSR fakeWdtcsr
#define MCUSR fakeMcusr
#define ADC fakeAdcRead()
#endif

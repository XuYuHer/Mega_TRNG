#include <Arduino.h>
AdcControl fakeAdcsra = {0};
volatile uint8_t fakeAdmux = 0, fakeAdcsrb = 0, fakeDidr0 = 0, fakeDidr2 = 0;
volatile uint8_t fakeTccr1a = 0, fakeTccr1b = 0, fakeTccr1c = 0, fakeTimsk1 = 0;
volatile uint8_t fakeWdtcsr = 0, fakeMcusr = 0;
volatile uint16_t fakeTcnt1 = 0;
uint32_t fakeTimerReads = 0;
uint8_t fakeTimerByte(uint8_t shift) {
    ++fakeTimerReads;
    return (uint8_t)(fakeTcnt1 >> shift);
}

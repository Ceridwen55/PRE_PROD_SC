// drivers/Led.cpp -- see Led.h for the "why". Short blinks only.

#include "Led.h"
#include "config.h"


// The "off" level is just the opposite of the "on" (active) level.
static const int LED_OFF_LEVEL = (LED_ACTIVE_LEVEL == HIGH) ? LOW : HIGH;

// Blink timings (kept short on purpose -- battery). These delays are
// BLOCKING and add straight to the sensor's awake window, so they sit at
// the minimum a human eye still catches (~25 ms flash).
static const uint16_t PULSE_ON_MS  = 25;   // power "alive" blink length
static const uint16_t BLINK_ON_MS  = 25;   // status blink: on  time
static const uint16_t BLINK_OFF_MS = 55;   // status blink: gap between blinks


void led_init() {
    pinMode(PIN_LED_POWER,  OUTPUT);
    pinMode(PIN_LED_STATUS, OUTPUT);
    digitalWrite(PIN_LED_POWER,  LED_OFF_LEVEL);
    digitalWrite(PIN_LED_STATUS, LED_OFF_LEVEL);
}


void led_power_pulse() {
    digitalWrite(PIN_LED_POWER, LED_ACTIVE_LEVEL);
    delay(PULSE_ON_MS);
    digitalWrite(PIN_LED_POWER, LED_OFF_LEVEL);
}


void led_status_blink(uint8_t times) {
    for (uint8_t i = 0; i < times; i++) {
        digitalWrite(PIN_LED_STATUS, LED_ACTIVE_LEVEL);
        delay(BLINK_ON_MS);
        digitalWrite(PIN_LED_STATUS, LED_OFF_LEVEL);
        if (i + 1 < times) delay(BLINK_OFF_MS);   // no trailing gap
    }
}

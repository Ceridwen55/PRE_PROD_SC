/* drivers/Led.h -- tiny power/status LED helper

WHAT THIS FILE IS FOR
---------------------
Blink the two indicator LEDs in SHORT pulses only:
  - POWER LED  : one brief blink at boot/wake = "this device is alive".
  - STATUS LED : N brief blinks on an event (data sent, publish OK, paired).

WHY ONLY SHORT BLINKS
---------------------
A lit LED draws a few mA. A 30-60 ms blink costs almost nothing, while a
permanently-on LED would slowly drain the sensor's battery. So we NEVER
leave an LED on -- we only pulse it. (See the battery note in config.h.)

WHERE IT IS USED
----------------
Both roles. The pins + polarity come from config.h
(PIN_LED_POWER, PIN_LED_STATUS, LED_ACTIVE_LEVEL).
On the gateway PCB the LEDs are mains-powered, so blink count there is
just a visual heartbeat.
*/

#pragma once

#include <Arduino.h>


// Set both LED pins as outputs and turn them off. Call once at boot.
void led_init();

// One short blink on the POWER LED ("device is alive").
void led_power_pulse();

// N short blinks on the STATUS LED. Examples used in this project:
//   2 = sensor delivered its reading over ESP-NOW
//   3 = gateway published a reading to MQTT
//   5 = commissioning / pairing succeeded
void led_status_blink(uint8_t times);

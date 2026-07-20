// drivers/Battery.cpp  (GATEWAY)

#include "Battery.h"
#include "log.h"
#include "config.h"


static const int   NUM_SAMPLES   = 16;
static const float ADC_VREF      = 3.3f;
static const int   ADC_MAX_COUNT = 4095;   // 12-bit ADC


void battery_init() {
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

    // Start with the divider DISCONNECTED. The bottom of the divider (IO0)
    // is left floating (Hi-Z) so no current flows through R16/R17 -- this is
    // what stops the battery from being drained between measurements. We
    // only pull it LOW briefly inside battery_read().
    pinMode(PIN_BATTERY_DRAIN, INPUT);

    LOG.printf("[Battery] ADC ready on GPIO%d (drain gate GPIO%d).\n",
                  PIN_BATTERY_ADC, PIN_BATTERY_DRAIN);
}


Battery_Data battery_read() {
    Battery_Data result;
    result.voltage    = 0.0f;
    result.millivolts = 0;
    result.valid      = false;

    // Connect the divider to ground for the measurement (IO0 LOW so IO1 sees
    // VBatt/2). The 1M/1M divider is very high-impedance, so give the ADC
    // sample cap a moment to settle before sampling.
    pinMode(PIN_BATTERY_DRAIN, OUTPUT);
    digitalWrite(PIN_BATTERY_DRAIN, LOW);
    delay(10);

    // Take 3 quick bursts (each an average of NUM_SAMPLES). A real battery is a
    // low-impedance source, so the three bursts agree closely. An empty holder /
    // open VBAT floats and the bursts scatter -- we use that spread to REJECT
    // the noisy "no battery" case instead of trusting a single reading.
    float v[3];
    for (int k = 0; k < 3; k++) {
        long sum = 0;
        for (int i = 0; i < NUM_SAMPLES; i++) {
            sum += analogRead(PIN_BATTERY_ADC);
            delayMicroseconds(50);
        }
        float adc_avg = (float)sum / (float)NUM_SAMPLES;
        v[k] = (adc_avg / (float)ADC_MAX_COUNT) * ADC_VREF / BAT_VOLTAGE_DIVIDER_RATIO;
        if (k < 2) delay(3);
    }

    // Disconnect the divider again (Hi-Z) so it stops draining the battery.
    pinMode(PIN_BATTERY_DRAIN, INPUT);

    // Median + spread of the three bursts.
    float lo = v[0], hi = v[0];
    for (int k = 1; k < 3; k++) { if (v[k] < lo) lo = v[k]; if (v[k] > hi) hi = v[k]; }
    float v_bat  = v[0] + v[1] + v[2] - lo - hi;   // the middle (median) value
    float spread = hi - lo;

    const float STABLE_SPREAD_V = 0.15f;   // bursts must agree within this to trust

    if (v_bat < 2.0f || v_bat > 5.5f) {
        LOG.printf("[Battery] out-of-range: %.3f V -- invalid (no battery?)\n", v_bat);
        return result;
    }
    if (spread > STABLE_SPREAD_V) {
        LOG.printf("[Battery] unstable: spread %.3f V -- invalid (no battery?)\n", spread);
        return result;
    }

    result.voltage    = v_bat;
    result.millivolts = (uint16_t)lroundf(v_bat * 1000.0f);
    result.valid      = true;

    LOG.printf("[Battery] %.3f V (spread %.3f V)\n", v_bat, spread);
    return result;
}

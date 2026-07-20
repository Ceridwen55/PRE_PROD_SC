// drivers/SHT41.cpp — SHT41 Temperature & Humidity Sensor Driver
// IDENTICAL to sensor box version. Same hardware, same code.

#include "SHT41.h"
#include "log.h"
#include "config.h"

// Create one sensor object, private to this file
static Adafruit_SHT4x sht4x;


// Initialize the SHT41 sensor
bool sht41_init() {

    // Power the SHT41 on first. On the sensor PCB its VCC comes through a
    // P-channel MOSFET: gate LOW = ON. (On the gateway PCB this pin is
    // unconnected, so this just does nothing.) Give the rail a moment to
    // settle before we talk to the chip over I2C.
    pinMode(PIN_SHT41_POWER, OUTPUT);
    digitalWrite(PIN_SHT41_POWER, LOW);   // MOSFET ON -> SHT41 powered
    delay(3);                             // SHT41 power-up is <=1 ms; 3 ms margin

    // Try to find the sensor on I2C bus
    if (!sht4x.begin()) {
        LOG.println("[SHT41] ERROR: Sensor not found on I2C bus.");
        LOG.println("[SHT41] Check wiring, power gate, and I2C address (0x44).");
        return false;
    }

    // Set measurement precision
    // HIGH_MED = good accuracy without using too much power
    //sht4x.setPrecision(SHT4X_HIGH_MED);

    // Heater off — we handle heating externally for wind measurement
    sht4x.setHeater(SHT4X_NO_HEATER);

    LOG.println("[SHT41] Initialized successfully.");
    return true;
}


// Read temperature and humidity from the sensor
SHT41_Data sht41_read() {

    // Start with empty result, valid = false
    SHT41_Data result;
    result.temperature = 0.0f;
    result.humidity    = 0.0f;
    result.valid       = false;

    // Adafruit library uses its own event type for readings
    sensors_event_t humidity_event, temp_event;

    // Ask the sensor for a reading
    bool success = sht4x.getEvent(&humidity_event, &temp_event);

    if (!success) {
        LOG.println("[SHT41] ERROR: Failed to read sensor data.");
        return result;
    }

    // Check if values are within the sensor's physical range
    // SHT41 range: Temperature -40°C to +125°C, Humidity 0% to 100%
    float t = temp_event.temperature;
    float h = humidity_event.relative_humidity;

    if (t < -40.0f || t > 125.0f) {
        LOG.printf("[SHT41] ERROR: Temperature out of range: %.2f C\n", t);
        return result;
    }

    if (h < 0.0f || h > 100.0f) {
        LOG.printf("[SHT41] ERROR: Humidity out of range: %.2f %%\n", h);
        return result;
    }

    // All checks passed — fill in the result
    result.temperature = t;
    result.humidity    = h;
    result.valid       = true;

    LOG.printf("[SHT41] Temp: %.2f C | Humidity: %.2f %%\n", t, h);
    return result;
}


// Cut power to the SHT41 (P-channel MOSFET gate HIGH = OFF).
void sht41_power_off() {
    pinMode(PIN_SHT41_POWER, OUTPUT);
    digitalWrite(PIN_SHT41_POWER, HIGH);  // MOSFET OFF -> SHT41 unpowered
}

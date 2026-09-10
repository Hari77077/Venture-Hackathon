#pragma once
#include <Wire.h>
#include <Adafruit_BMP085.h>   // BMP180 uses the BMP085 library (same chip family)
#include <MPU6050.h>           // Electronic Cats / jrowberg MPU6050 library

// ---------------------------------------------------------------------------
// SentinelSensors — wraps BMP180 (pressure/temperature) and MPU6050
// (accelerometer/gyro) with full Serial debug output.
//
// BMP180 I2C address: 0x77 (fixed, cannot change)
// MPU6050 I2C address: 0x68 (AD0 low) or 0x69 (AD0 high)
//
// Vibration is computed as the raw acceleration vector magnitude minus the
// gravitational constant (9.80665 m/s²), then converted to g-force units.
// When the sensor is perfectly still, vibration_g ≈ 0.
// ---------------------------------------------------------------------------

#define GRAVITY_MS2  9.80665f

struct SensorReading {
    float pressure_hpa;   // absolute pressure in hectopascals
    float temp_c;         // temperature in °C (from BMP180)
    float vibration_g;    // net vibration in g (gravity-subtracted)
    bool  ok;             // false if either sensor returned an error
};

class SentinelSensors {
public:
    // Returns true if both sensors initialised successfully.
    bool begin() {
        bool allOk = true;

        // --- BMP180 ---
        if (!_bmp.begin()) {
            Serial.println(F("[Sensors] BMP180 init FAILED — check wiring/I2C (addr 0x77)"));
            _bmpOk = false;
            allOk  = false;
        } else {
            Serial.println(F("[Sensors] BMP180 OK"));
            _bmpOk = true;
        }

        // --- MPU6050 ---
        _mpu.initialize();
        if (!_mpu.testConnection()) {
            Serial.println(F("[Sensors] MPU6050 init FAILED — check wiring/I2C (addr 0x68)"));
            _mpuOk = false;
            allOk  = false;
        } else {
            // Set full-scale range to ±8g — good for flood debris / vibration events
            _mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_8);
            Serial.println(F("[Sensors] MPU6050 OK  range=±8g"));
            _mpuOk = true;
        }

        return allOk;
    }

    SensorReading read() {
        SensorReading r = {};
        r.ok = true;

        // --- BMP180 ---
        if (_bmpOk) {
            r.pressure_hpa = _bmp.readPressure() / 100.0f;  // Pa → hPa
            r.temp_c       = _bmp.readTemperature();

            if (isnan(r.pressure_hpa) || r.pressure_hpa == 0.0f) {
                Serial.println(F("[Sensors] BMP180 read error (pressure=0/NaN)"));
                r.pressure_hpa = 0.0f;
                r.ok = false;
            } else {
                Serial.print(F("[Sensors] BMP180  P="));
                Serial.print(r.pressure_hpa, 2);
                Serial.print(F(" hPa  T="));
                Serial.print(r.temp_c, 2);
                Serial.println(F(" °C"));
            }
        } else {
            Serial.println(F("[Sensors] BMP180 skipped (not initialised)"));
        }

        // --- MPU6050 ---
        if (_mpuOk) {
            int16_t ax, ay, az;
            _mpu.getAcceleration(&ax, &ay, &az);

            // Scale factor for ±8g range: 4096 LSB/g
            const float scale = 4096.0f;
            float gx = ax / scale;
            float gy = ay / scale;
            float gz = az / scale;

            float magnitude_g = sqrt(gx*gx + gy*gy + gz*gz);

            // Subtract 1g (gravity) so stationary sensor reads ~0
            r.vibration_g = magnitude_g - 1.0f;
            if (r.vibration_g < 0.0f) r.vibration_g = 0.0f;  // clamp negatives

            Serial.print(F("[Sensors] MPU6050  ax="));
            Serial.print(gx, 3);
            Serial.print(F("g  ay="));
            Serial.print(gy, 3);
            Serial.print(F("g  az="));
            Serial.print(gz, 3);
            Serial.print(F("g  |vib|="));
            Serial.print(r.vibration_g, 3);
            Serial.println(F("g"));
        } else {
            Serial.println(F("[Sensors] MPU6050 skipped (not initialised)"));
        }

        return r;
    }

private:
    Adafruit_BMP085 _bmp;
    MPU6050         _mpu;
    bool            _bmpOk = false;
    bool            _mpuOk = false;
};

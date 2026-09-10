#pragma once
#include <Wire.h>
#include <Adafruit_BMP085.h>   // BMP180 uses the BMP085 library
#include <MPU6050.h>           // Electronic Cats / jrowberg MPU6050
#include <DHT.h>               // Adafruit DHT sensor library

// ---------------------------------------------------------------------------
// SentinelSensors — ESP32-S3 sensor suite
//
// I2C bus (shared):  SDA = GPIO 8,  SCL = GPIO 9
//   BMP180:   0x77 (pressure + temperature)
//   MPU6050:  0x68 (accelerometer — vibration)
//
// Digital:
//   DHT11:    GPIO 4  (temperature + humidity)
//   HC-SR04:  Trig = GPIO 6,  Echo = GPIO 7  (water-level distance)
// ---------------------------------------------------------------------------

// --- Pin definitions (ESP32-S3 wiring) ---
#define SENSOR_SDA      8
#define SENSOR_SCL      9
#define DHT_PIN         4
#define DHT_TYPE        DHT11
#define US_TRIG_PIN     6
#define US_ECHO_PIN     7

struct SensorReading {
    float pressure_hpa;   // BMP180
    float temp_c;         // BMP180
    float humidity_pct;   // DHT11
    float vibration_g;    // MPU6050 (gravity-subtracted)
    float water_level_cm; // HC-SR04 distance
    bool  ok;
};

class SentinelSensors {
public:
    SentinelSensors() : _dht(DHT_PIN, DHT_TYPE) {}

    bool begin() {
        bool allOk = true;

        // --- I2C on custom pins ---
        Wire.begin(SENSOR_SDA, SENSOR_SCL);
        Serial.print(F("[Sensors] I2C started  SDA="));
        Serial.print(SENSOR_SDA);
        Serial.print(F("  SCL="));
        Serial.println(SENSOR_SCL);

        // --- BMP180 ---
        if (!_bmp.begin()) {
            Serial.println(F("[Sensors] BMP180 FAILED (addr 0x77)"));
            _bmpOk = false;
            allOk  = false;
        } else {
            Serial.println(F("[Sensors] BMP180 OK"));
            _bmpOk = true;
        }

        // --- MPU6050 ---
        _mpu.initialize();
        if (!_mpu.testConnection()) {
            Serial.println(F("[Sensors] MPU6050 FAILED (addr 0x68)"));
            _mpuOk = false;
            allOk  = false;
        } else {
            _mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_8);
            Serial.println(F("[Sensors] MPU6050 OK  range=±8g"));
            _mpuOk = true;
        }

        // --- DHT11 ---
        _dht.begin();
        Serial.print(F("[Sensors] DHT11 started on GPIO "));
        Serial.println(DHT_PIN);
        _dhtOk = true;   // DHT has no begin() error — failures detected on read

        // --- HC-SR04 ultrasonic ---
        pinMode(US_TRIG_PIN, OUTPUT);
        pinMode(US_ECHO_PIN, INPUT);
        digitalWrite(US_TRIG_PIN, LOW);
        Serial.print(F("[Sensors] Ultrasonic  Trig=GPIO"));
        Serial.print(US_TRIG_PIN);
        Serial.print(F("  Echo=GPIO"));
        Serial.println(US_ECHO_PIN);

        return allOk;
    }

    SensorReading read() {
        SensorReading r = {};
        r.ok = true;

        // --- BMP180: pressure + temperature ---
        if (_bmpOk) {
            r.pressure_hpa = _bmp.readPressure() / 100.0f;
            r.temp_c       = _bmp.readTemperature();

            if (isnan(r.pressure_hpa) || r.pressure_hpa < 300.0f) {
                Serial.println(F("[Sensors] BMP180 read error"));
                r.ok = false;
            } else {
                Serial.print(F("[Sensors] BMP180  P="));
                Serial.print(r.pressure_hpa, 2);
                Serial.print(F(" hPa  T="));
                Serial.print(r.temp_c, 2);
                Serial.println(F(" C"));
            }
        } else {
            Serial.println(F("[Sensors] BMP180 skipped"));
        }

        // --- MPU6050: vibration ---
        if (_mpuOk) {
            int16_t ax, ay, az;
            _mpu.getAcceleration(&ax, &ay, &az);

            const float scale = 4096.0f;   // ±8g → 4096 LSB/g
            float gx = ax / scale;
            float gy = ay / scale;
            float gz = az / scale;

            float mag = sqrt(gx*gx + gy*gy + gz*gz);
            r.vibration_g = mag - 1.0f;    // subtract gravity
            if (r.vibration_g < 0.0f) r.vibration_g = 0.0f;

            Serial.print(F("[Sensors] MPU6050  ax="));
            Serial.print(gx, 3);
            Serial.print(F("g  ay="));
            Serial.print(gy, 3);
            Serial.print(F("g  az="));
            Serial.print(gz, 3);
            Serial.print(F("g  vib="));
            Serial.print(r.vibration_g, 3);
            Serial.println(F("g"));
        } else {
            Serial.println(F("[Sensors] MPU6050 skipped"));
        }

        // --- DHT11: humidity ---
        if (_dhtOk) {
            float h = _dht.readHumidity();
            float t = _dht.readTemperature();

            if (isnan(h) || isnan(t)) {
                Serial.println(F("[Sensors] DHT11 read error"));
                r.humidity_pct = 0.0f;
            } else {
                r.humidity_pct = h;
                Serial.print(F("[Sensors] DHT11  H="));
                Serial.print(h, 1);
                Serial.print(F("%  T="));
                Serial.print(t, 1);
                Serial.println(F(" C"));
            }
        }

        // --- HC-SR04: water-level distance ---
        r.water_level_cm = _readUltrasonic();
        if (r.water_level_cm > 0.0f) {
            Serial.print(F("[Sensors] Ultrasonic  dist="));
            Serial.print(r.water_level_cm, 1);
            Serial.println(F(" cm"));
        } else {
            Serial.println(F("[Sensors] Ultrasonic  no echo / out of range"));
        }

        return r;
    }

private:
    Adafruit_BMP085 _bmp;
    MPU6050         _mpu;
    DHT             _dht;
    bool            _bmpOk  = false;
    bool            _mpuOk  = false;
    bool            _dhtOk  = false;

    float _readUltrasonic() {
        // Send 10µs trigger pulse
        digitalWrite(US_TRIG_PIN, LOW);
        delayMicroseconds(2);
        digitalWrite(US_TRIG_PIN, HIGH);
        delayMicroseconds(10);
        digitalWrite(US_TRIG_PIN, LOW);

        // Measure echo pulse duration (timeout 30 ms ≈ ~5 m max range)
        long duration = pulseIn(US_ECHO_PIN, HIGH, 30000UL);
        if (duration == 0) return -1.0f;

        // Speed of sound ≈ 0.0343 cm/µs, distance = duration * 0.0343 / 2
        float dist = duration * 0.01715f;
        return dist;
    }
};

// =============================================================================
// sentinel.ino — Flood-mesh Sentinel Node  (ESP32-S3)
//
// Hardware:
//   MCU     : ESP32-S3
//   Radio   : SX1278 LoRa  (SCK=12, MISO=13, MOSI=11, CS=10, RST=14, DIO0=15)
//   BMP180  : I2C 0x77  (SDA=8, SCL=9)
//   MPU6050 : I2C 0x68  (SDA=8, SCL=9, shared bus)
//   DHT11   : GPIO 4
//   Ultrasonic HC-SR04 : Trig=6, Echo=7
//   Buzzer  : GPIO 5
//
// Role:
//   • Reads sensors every SAMPLE_INTERVAL_MS (non-blocking)
//   • Sends MSG_TELEMETRY into the mesh
//   • Listens continuously, forwards relayed packets
//   • MSG_ALERT → triggers buzzer
//   • MSG_TEXT → prints to Serial immediately
//   • Serial-based messaging: type a message in Serial Monitor → broadcasts
//   • Adaptive SF via MeshRadio::adaptSF()
//   • Local fallback alert if mesh silent >30 min and sensors look bad
//
// NOTE: No LCD on this node — LCD is on the Civilian Node.
// =============================================================================

#include <SPI.h>
#include <RadioLib.h>
#include <Wire.h>
#include "../common/mesh_packet.h"
#include "../common/radio.h"
#include "sensors.h"

// ---- Node identity --------------------------------------------------------
#define MY_NODE_ID        1001U

// ---- LoRa SPI pins (ESP32-S3) ---------------------------------------------
#define LORA_SCK          12
#define LORA_MISO         13
#define LORA_MOSI         11
#define LORA_CS           10
#define LORA_RST          14
#define LORA_DIO0         15

// ---- Other pins ------------------------------------------------------------
#define BUZZER_PIN         5

// ---- Timing ----------------------------------------------------------------
#define SAMPLE_INTERVAL_MS    (5UL * 60UL * 1000UL)   // 5 min
#define ADAPT_SF_INTERVAL_MS  (2UL * 60UL * 1000UL)   // 2 min

// ---- Fallback thresholds ---------------------------------------------------
#define FALLBACK_SILENCE_MS       (30UL * 60UL * 1000UL)
#define FALLBACK_PRESSURE_DROP    3.0f
#define FALLBACK_VIBRATION_G      2.0f

// ---- Hardware instances ----------------------------------------------------
// Custom SPI for LoRa (ESP32-S3 non-default pins)
SPIClass loRaSPI(FSPI);
SX1278           radioModule = new Module(LORA_CS, LORA_DIO0, LORA_RST, RADIOLIB_NC, loRaSPI);
MeshRadio        radio(radioModule);
SeenCache        seenCache;
SentinelSensors  sensors;

// ---- State -----------------------------------------------------------------
uint32_t seqCounter          = 0;
float    lastPressure        = NAN;
uint32_t lastMeshActivityMs  = 0;
uint32_t lastSampleMs        = 0;
uint32_t lastAdaptSFMs       = 0;
bool     radioOk             = false;

SensorReading lastReading    = {};

// ---- Forward declarations --------------------------------------------------
void applyLocalAlert(uint8_t severity);
void checkSerialMessage();
void sendTextMessage(const char *msg);
void printSepLine();

// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);   // ESP32-S3 USB-CDC stabilisation

    printSepLine();
    Serial.println(F("=== Sentinel Node Boot (ESP32-S3) ==="));
    Serial.print(F("  Node ID : ")); Serial.println(MY_NODE_ID);
    Serial.print(F("  Compiled: ")); Serial.print(F(__DATE__));
    Serial.print(F(" ")); Serial.println(F(__TIME__));
    printSepLine();

    // --- GPIO ---
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // --- Sensors (inits I2C internally on SDA=8, SCL=9) ---
    Serial.println(F("[Setup] Initialising sensors..."));
    bool sensorsOk = sensors.begin();
    Serial.println(sensorsOk ? F("[Setup] All sensors OK")
                             : F("[Setup] WARNING: sensor(s) failed — check Serial"));

    // --- LoRa SPI on custom pins ---
    Serial.println(F("[Setup] SPI for LoRa..."));
    loRaSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

    Serial.println(F("[Setup] Initialising radio (SX1278)..."));
    radioOk = radio.begin();
    if (!radioOk) {
        Serial.println(F("[Setup] *** RADIO FAILED — halting ***"));
        while (true) {
            digitalWrite(BUZZER_PIN, HIGH); delay(200);
            digitalWrite(BUZZER_PIN, LOW);  delay(200);
        }
    }

    // --- Seed random ---
    randomSeed(analogRead(0));

    lastMeshActivityMs = millis();
    lastSampleMs       = millis() - SAMPLE_INTERVAL_MS;  // force immediate first sample

    Serial.println(F("[Setup] Boot complete. Type a message in Serial Monitor to broadcast."));
    printSepLine();
}

// ============================================================================
void loop() {
    uint32_t now = millis();

    // =========================================================
    // 1. SENSOR SAMPLE & TX (non-blocking, every SAMPLE_INTERVAL_MS)
    // =========================================================
    if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
        lastSampleMs = now;

        Serial.println();
        printSepLine();
        Serial.println(F("[Loop] --- Sensor cycle ---"));

        lastReading = sensors.read();

        if (radioOk) {
            MeshPacket pkt = {};
            pkt.origin_id      = MY_NODE_ID;
            pkt.seq_num        = seqCounter++;
            pkt.ttl            = MAX_TTL;
            pkt.msg_type       = MSG_TELEMETRY;
            pkt.node_type      = NODE_SENTINEL;
            pkt.severity       = SEV_INFO;
            pkt.pressure_hpa   = lastReading.pressure_hpa;
            pkt.temp_c         = lastReading.temp_c;
            pkt.humidity_pct   = lastReading.humidity_pct;
            pkt.vibration_g    = lastReading.vibration_g;
            pkt.water_level_cm = lastReading.water_level_cm;
            pkt.rssi_dbm       = radio.lastRssi();
            pkt.snr_db         = radio.lastSnr();
            pkt.sf             = radio.currentSF();
            memset(pkt.message, 0, MSG_TEXT_LEN);

            seenCache.add(pkt.origin_id, pkt.seq_num);

            Serial.print(F("[Loop] TX telemetry  seq="));
            Serial.print(pkt.seq_num);
            Serial.print(F("  P="));   Serial.print(pkt.pressure_hpa, 2);
            Serial.print(F(" hPa  T=")); Serial.print(pkt.temp_c, 2);
            Serial.print(F(" C  H=")); Serial.print(pkt.humidity_pct, 1);
            Serial.print(F("%  Vib=")); Serial.print(pkt.vibration_g, 3);
            Serial.print(F("g  WL=")); Serial.print(pkt.water_level_cm, 1);
            Serial.println(F(" cm"));

            radio.sendWithCAD(pkt);
        }

        // --- Fallback check ---
        bool meshIsolated   = (now - lastMeshActivityMs) > FALLBACK_SILENCE_MS;
        bool pressureDrop   = !isnan(lastPressure) &&
                              (lastPressure - lastReading.pressure_hpa) > FALLBACK_PRESSURE_DROP;
        bool vibrationSpike = lastReading.vibration_g > FALLBACK_VIBRATION_G;

        Serial.print(F("[Loop] Fallback: isolated="));
        Serial.print(meshIsolated ? "YES" : "no");
        Serial.print(F("  pDrop="));
        Serial.print(pressureDrop ? "YES" : "no");
        Serial.print(F("  vibSpike="));
        Serial.println(vibrationSpike ? "YES" : "no");

        if (meshIsolated && (pressureDrop || vibrationSpike)) {
            Serial.println(F("[Loop] *** FALLBACK ALERT ***"));
            applyLocalAlert(SEV_WARNING);
        }

        lastPressure = lastReading.pressure_hpa;
        printSepLine();
    }

    // =========================================================
    // 2. CONTINUOUS RECEIVE
    // =========================================================
    if (radioOk) {
        MeshPacket incoming;
        if (radio.receive(incoming, 0) == RADIOLIB_ERR_NONE) {
            lastMeshActivityMs = now;

            Serial.print(F("[RX] origin="));
            Serial.print(incoming.origin_id);
            Serial.print(F("  type=")); Serial.print(incoming.msg_type);
            Serial.print(F("  sev=")); Serial.print(incoming.severity);
            Serial.print(F("  RSSI=")); Serial.print(radio.lastRssi());
            Serial.print(F("  SNR=")); Serial.print(radio.lastSnr());
            Serial.print(F("  SF=")); Serial.println(radio.currentSF());

            if (incoming.msg_type == MSG_ALERT) {
                Serial.print(F("[RX] *** ALERT  sev="));
                Serial.println(incoming.severity);
                applyLocalAlert(incoming.severity);
            }

            if (incoming.msg_type == MSG_TEXT) {
                incoming.message[MSG_TEXT_LEN - 1] = '\0';
                Serial.print(F("[RX] *** MESSAGE from "));
                Serial.print(incoming.origin_id);
                Serial.print(F(": \""));
                Serial.print(incoming.message);
                Serial.println(F("\""));
                // Short beep to signal message arrival
                digitalWrite(BUZZER_PIN, HIGH); delay(80);
                digitalWrite(BUZZER_PIN, LOW);
            }

            forwardPacket(incoming, seenCache, radio);
        }
    }

    // =========================================================
    // 3. SERIAL MESSAGE INPUT — type in Serial Monitor to send
    // =========================================================
    checkSerialMessage();

    // =========================================================
    // 4. ADAPTIVE SF
    // =========================================================
    if (radioOk && (now - lastAdaptSFMs >= ADAPT_SF_INTERVAL_MS)) {
        lastAdaptSFMs = now;
        radio.adaptSF();
    }
}

// ============================================================================
void applyLocalAlert(uint8_t severity) {
    Serial.print(F("[Alert] sev=")); Serial.println(severity);

    if (severity >= SEV_DANGER) {
        // Continuous buzzer for DANGER/EMERGENCY
        digitalWrite(BUZZER_PIN, HIGH);
    } else if (severity >= SEV_WARNING) {
        // Short beep for WARNING
        digitalWrite(BUZZER_PIN, HIGH); delay(300);
        digitalWrite(BUZZER_PIN, LOW);
    } else {
        digitalWrite(BUZZER_PIN, LOW);
    }
}

// ============================================================================
// checkSerialMessage — reads a line from Serial Monitor and broadcasts it
// ============================================================================
void checkSerialMessage() {
    static char buf[MSG_TEXT_LEN];
    static uint8_t pos = 0;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                buf[pos] = '\0';
                sendTextMessage(buf);
                pos = 0;
            }
        } else if (pos < MSG_TEXT_LEN - 1) {
            buf[pos++] = c;
        }
    }
}

void sendTextMessage(const char *msg) {
    if (!radioOk) {
        Serial.println(F("[Msg] Radio down, cannot send"));
        return;
    }

    MeshPacket pkt = {};
    pkt.origin_id = MY_NODE_ID;
    pkt.seq_num   = seqCounter++;
    pkt.ttl       = MAX_TTL;
    pkt.msg_type  = MSG_TEXT;
    pkt.node_type = NODE_SENTINEL;
    pkt.severity  = SEV_WARNING;
    pkt.rssi_dbm  = radio.lastRssi();
    pkt.snr_db    = radio.lastSnr();
    pkt.sf        = radio.currentSF();
    strncpy(pkt.message, msg, MSG_TEXT_LEN - 1);
    pkt.message[MSG_TEXT_LEN - 1] = '\0';

    seenCache.add(pkt.origin_id, pkt.seq_num);

    Serial.print(F("[Msg] TX: \"")); Serial.print(pkt.message); Serial.println(F("\""));
    bool ok = radio.sendWithCAD(pkt);
    Serial.println(ok ? F("[Msg] TX OK") : F("[Msg] TX FAILED"));
}

// ============================================================================
void printSepLine() {
    Serial.println(F("--------------------------------------------"));
}

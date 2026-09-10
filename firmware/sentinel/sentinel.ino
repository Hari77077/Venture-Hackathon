// =============================================================================
// sentinel.ino — Flood-mesh Sentinel Node
//
// Hardware:
//   • MCU     : Arduino Mega / Nano / similar
//   • Radio   : SX1278 LoRa  (NSS=10, DIO0=2, RST=9, DIO1=3)
//   • Pressure: BMP180        (I2C 0x77)
//   • Accel   : MPU6050       (I2C 0x68)
//   • LCD     : I2C 16×2 LCD  (I2C 0x26)
//   • Siren   : active buzzer on SIREN_PIN
//   • LED     : status LED on LED_PIN
//   • Btn     : MSG_TEXT send button on BUTTON_PIN (active LOW, internal pull-up)
//
// Role in mesh:
//   • Reads sensors every SAMPLE_INTERVAL_MS (non-blocking — no long delay)
//   • Sends MSG_TELEMETRY packet
//   • Listens continuously and forwards relayed packets
//   • On MSG_ALERT: triggers siren / LED + shows on LCD immediately
//   • On MSG_TEXT:  shows on LCD immediately + Serial
//   • On button press: broadcasts a distress MSG_TEXT into the mesh
//   • Adaptive SF via MeshRadio::adaptSF()
//   • Local fallback alert if mesh silent >30 min and sensors look bad
// =============================================================================

#include <RadioLib.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "../common/mesh_packet.h"
#include "../common/radio.h"
#include "sensors.h"

// ---- Node identity (change per physical unit at flash time) ---------------
#define MY_NODE_ID        1001U

// ---- Pin assignments -------------------------------------------------------
#define NSS_PIN           10
#define DIO0_PIN           2
#define RST_PIN            9
#define DIO1_PIN           3
#define SIREN_PIN          5
#define LED_PIN            6
#define BUTTON_PIN         7   // active-LOW, send MSG_TEXT on press

// ---- Timing ----------------------------------------------------------------
#define SAMPLE_INTERVAL_MS    (5UL * 60UL * 1000UL)   // 5 min between sensor reads
#define LCD_CYCLE_MS          4000UL                   // rotate LCD screen every 4 s
#define ADAPT_SF_INTERVAL_MS  (2UL * 60UL * 1000UL)   // adapt SF every 2 min
#define BUTTON_DEBOUNCE_MS    50UL

// ---- Fallback thresholds ---------------------------------------------------
#define FALLBACK_SILENCE_MS       (30UL * 60UL * 1000UL)
#define FALLBACK_PRESSURE_DROP    3.0f    // hPa drop triggers fallback
#define FALLBACK_VIBRATION_G      2.0f

// ---- Distress message text -------------------------------------------------
#define DISTRESS_MSG  "HELP - SENTINEL NODE"

// ---- Hardware instances ----------------------------------------------------
SX1278           radioModule = new Module(NSS_PIN, DIO0_PIN, RST_PIN, DIO1_PIN);
MeshRadio        radio(radioModule);
LiquidCrystal_I2C lcd(0x26, 16, 2);   // 16 cols × 2 rows
SeenCache        seenCache;
SentinelSensors  sensors;

// ---- State -----------------------------------------------------------------
uint32_t seqCounter       = 0;
float    lastPressure     = NAN;
uint32_t lastMeshActivityMs  = 0;
uint32_t lastSampleMs        = 0;
uint32_t lastLcdCycleMs      = 0;
uint32_t lastAdaptSFMs       = 0;
uint8_t  lcdScreen           = 0;   // 0=telemetry, 1=link quality, 2=last message

SensorReading lastReading    = {};
bool          radioOk        = false;
bool          buttonWasLow   = false;   // debounce state

// Last received text message (for LCD display)
char lastMsg[MSG_TEXT_LEN + 1] = "No messages yet";
uint16_t lastMsgOrigin = 0;

// ---- Forward declarations --------------------------------------------------
void applyLocalAlert(uint8_t severity);
void sendDistressMessage();
void updateLCD();
void lcdShowTelemetry();
void lcdShowLinkQuality();
void lcdShowLastMessage();
void lcdPrint(const char *line0, const char *line1);
void printSepLine();

// ============================================================================
void setup() {
    Serial.begin(115200);
    while (!Serial);   // wait for USB serial on boards that need it

    printSepLine();
    Serial.println(F("=== Sentinel Node Boot ==="));
    Serial.print(F("  Node ID : "));
    Serial.println(MY_NODE_ID);
    Serial.print(F("  Compiled: "));
    Serial.print(F(__DATE__));
    Serial.print(F(" "));
    Serial.println(F(__TIME__));
    printSepLine();

    // --- GPIO ---
    pinMode(SIREN_PIN, OUTPUT);
    pinMode(LED_PIN,   OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    digitalWrite(SIREN_PIN, LOW);
    digitalWrite(LED_PIN,   LOW);

    // --- I2C ---
    Wire.begin();
    Serial.println(F("[Setup] I2C started"));

    // --- LCD ---
    lcd.init();
    lcd.backlight();
    lcdPrint("Sentinel Node", "Booting...");
    Serial.println(F("[Setup] LCD OK (0x26)"));

    // --- Sensors ---
    Serial.println(F("[Setup] Initialising sensors..."));
    bool sensorsOk = sensors.begin();
    if (sensorsOk) {
        Serial.println(F("[Setup] All sensors OK"));
        lcdPrint("Sensors OK", "");
    } else {
        Serial.println(F("[Setup] WARNING: one or more sensors failed"));
        lcdPrint("Sensor WARN", "Check Serial");
    }
    delay(1000);

    // --- Radio ---
    Serial.println(F("[Setup] Initialising radio (SX1278)..."));
    radioOk = radio.begin();
    if (radioOk) {
        lcdPrint("Radio OK", "433 MHz LoRa");
    } else {
        lcdPrint("RADIO FAIL", "Check wiring!");
        Serial.println(F("[Setup] Radio failed — LED blink halt"));
        // Blink LED fast and halt (but keep Serial alive for debug)
        while (true) {
            digitalWrite(LED_PIN, HIGH);
            delay(200);
            digitalWrite(LED_PIN, LOW);
            delay(200);
        }
    }
    delay(1000);

    // --- Seed random ---
    randomSeed(analogRead(A0));

    lastMeshActivityMs = millis();
    lastSampleMs       = millis() - SAMPLE_INTERVAL_MS;  // force immediate sample

    Serial.println(F("[Setup] Boot complete. Entering loop."));
    printSepLine();
    lcdPrint("Sentinel Ready", "Listening...");
    delay(1500);
}

// ============================================================================
void loop() {
    uint32_t now = millis();

    // =========================================================
    // 1. NON-BLOCKING SENSOR SAMPLE & TX (every SAMPLE_INTERVAL_MS)
    // =========================================================
    if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
        lastSampleMs = now;

        Serial.println(F(""));
        printSepLine();
        Serial.println(F("[Loop] --- Sensor cycle ---"));

        lastReading = sensors.read();

        if (!radioOk) {
            Serial.println(F("[Loop] Radio down, skipping TX"));
        } else {
            MeshPacket pkt = {};
            pkt.origin_id    = MY_NODE_ID;
            pkt.seq_num      = seqCounter++;
            pkt.ttl          = MAX_TTL;
            pkt.msg_type     = MSG_TELEMETRY;
            pkt.node_type    = NODE_SENTINEL;
            pkt.severity     = SEV_INFO;
            pkt.pressure_hpa = lastReading.pressure_hpa;
            pkt.temp_c       = lastReading.temp_c;
            pkt.vibration_g  = lastReading.vibration_g;
            pkt.rssi_dbm     = radio.lastRssi();
            pkt.snr_db       = radio.lastSnr();
            pkt.sf           = radio.currentSF();
            memset(pkt.message, 0, MSG_TEXT_LEN);

            // Add to seen cache so we don't re-forward our own packet
            seenCache.add(pkt.origin_id, pkt.seq_num);

            Serial.print(F("[Loop] TX telemetry  seq="));
            Serial.print(pkt.seq_num);
            Serial.print(F("  P="));
            Serial.print(pkt.pressure_hpa, 2);
            Serial.print(F(" hPa  T="));
            Serial.print(pkt.temp_c, 2);
            Serial.print(F(" °C  Vib="));
            Serial.print(pkt.vibration_g, 3);
            Serial.println(F("g"));

            bool txOk = radio.sendWithCAD(pkt);
            if (!txOk) {
                Serial.println(F("[Loop] TX failed (channel busy or radio error)"));
            }
        }

        // --- Local fallback check ---
        bool meshIsolated   = (now - lastMeshActivityMs) > FALLBACK_SILENCE_MS;
        bool pressureDrop   = !isnan(lastPressure) &&
                              (lastPressure - lastReading.pressure_hpa) > FALLBACK_PRESSURE_DROP;
        bool vibrationSpike = lastReading.vibration_g > FALLBACK_VIBRATION_G;

        Serial.print(F("[Loop] Fallback check — isolated="));
        Serial.print(meshIsolated ? "YES" : "no");
        Serial.print(F("  pDrop="));
        Serial.print(pressureDrop ? "YES" : "no");
        Serial.print(F("  vibSpike="));
        Serial.println(vibrationSpike ? "YES" : "no");

        if (meshIsolated && (pressureDrop || vibrationSpike)) {
            Serial.println(F("[Loop] *** FALLBACK ALERT triggered ***"));
            applyLocalAlert(SEV_WARNING);
        }

        lastPressure = lastReading.pressure_hpa;
        printSepLine();
    }

    // =========================================================
    // 2. CONTINUOUS RECEIVE — process all incoming packets
    // =========================================================
    if (radioOk) {
        MeshPacket incoming;
        // Non-blocking poll (0 ms timeout)
        if (radio.receive(incoming, 0) == RADIOLIB_ERR_NONE) {
            lastMeshActivityMs = now;

            Serial.print(F("[RX] Packet  origin="));
            Serial.print(incoming.origin_id);
            Serial.print(F("  type="));
            Serial.print(incoming.msg_type);
            Serial.print(F("  sev="));
            Serial.print(incoming.severity);
            Serial.print(F("  RSSI="));
            Serial.print(radio.lastRssi());
            Serial.print(F("  SNR="));
            Serial.print(radio.lastSnr());
            Serial.print(F("  SF="));
            Serial.println(radio.currentSF());

            // --- Handle MSG_ALERT ---
            if (incoming.msg_type == MSG_ALERT) {
                Serial.print(F("[RX] *** ALERT received  sev="));
                Serial.println(incoming.severity);
                applyLocalAlert(incoming.severity);
            }

            // --- Handle MSG_TEXT — show immediately on LCD + Serial ---
            if (incoming.msg_type == MSG_TEXT) {
                incoming.message[MSG_TEXT_LEN - 1] = '\0';   // safety null-term
                strncpy(lastMsg, incoming.message, MSG_TEXT_LEN);
                lastMsgOrigin = incoming.origin_id;

                Serial.print(F("[RX] *** MESSAGE from node "));
                Serial.print(incoming.origin_id);
                Serial.print(F(": \""));
                Serial.print(lastMsg);
                Serial.println(F("\""));

                // Force LCD to message screen immediately
                lcdScreen = 2;
                lastLcdCycleMs = now;
                updateLCD();
            }

            // Forward all non-alert, non-text packets (and alerts/texts) onward
            forwardPacket(incoming, seenCache, radio);
        }
    }

    // =========================================================
    // 3. BUTTON — send distress MSG_TEXT
    // =========================================================
    bool buttonNowLow = (digitalRead(BUTTON_PIN) == LOW);
    if (buttonNowLow && !buttonWasLow) {
        delay(BUTTON_DEBOUNCE_MS);
        if (digitalRead(BUTTON_PIN) == LOW) {
            Serial.println(F("[BTN] Button pressed — sending distress MSG_TEXT"));
            sendDistressMessage();
        }
    }
    buttonWasLow = buttonNowLow;

    // =========================================================
    // 4. LCD ROTATION (non-blocking, every LCD_CYCLE_MS)
    // =========================================================
    if (now - lastLcdCycleMs >= LCD_CYCLE_MS) {
        lastLcdCycleMs = now;
        lcdScreen = (lcdScreen + 1) % 3;   // 0→1→2→0
        updateLCD();
    }

    // =========================================================
    // 5. ADAPTIVE SF (every ADAPT_SF_INTERVAL_MS)
    // =========================================================
    if (radioOk && (now - lastAdaptSFMs >= ADAPT_SF_INTERVAL_MS)) {
        lastAdaptSFMs = now;
        radio.adaptSF();
    }
}

// ============================================================================
// applyLocalAlert — drives siren + LED based on severity level
// ============================================================================
void applyLocalAlert(uint8_t severity) {
    Serial.print(F("[Alert] Applying local alert  sev="));
    Serial.println(severity);

    digitalWrite(LED_PIN,   severity >= SEV_WARNING   ? HIGH : LOW);
    digitalWrite(SIREN_PIN, severity >= SEV_DANGER    ? HIGH : LOW);

    // Brief beep for WARNING that doesn't reach DANGER threshold
    if (severity == SEV_WARNING) {
        digitalWrite(SIREN_PIN, HIGH);
        delay(200);
        digitalWrite(SIREN_PIN, LOW);
    }
}

// ============================================================================
// sendDistressMessage — compose and broadcast MSG_TEXT into the mesh
// ============================================================================
void sendDistressMessage() {
    if (!radioOk) {
        Serial.println(F("[Msg] Cannot send — radio down"));
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
    strncpy(pkt.message, DISTRESS_MSG, MSG_TEXT_LEN - 1);
    pkt.message[MSG_TEXT_LEN - 1] = '\0';

    seenCache.add(pkt.origin_id, pkt.seq_num);

    Serial.print(F("[Msg] Sending: \""));
    Serial.print(pkt.message);
    Serial.println(F("\""));

    bool ok = radio.sendWithCAD(pkt);
    Serial.println(ok ? F("[Msg] TX OK") : F("[Msg] TX FAILED"));

    // Show on LCD immediately
    lcdScreen = 2;
    strncpy(lastMsg, pkt.message, MSG_TEXT_LEN);
    lastMsgOrigin = MY_NODE_ID;
    updateLCD();
}

// ============================================================================
// LCD helpers
// ============================================================================
void updateLCD() {
    switch (lcdScreen) {
        case 0: lcdShowTelemetry();    break;
        case 1: lcdShowLinkQuality();  break;
        case 2: lcdShowLastMessage();  break;
    }
}

void lcdShowTelemetry() {
    char row0[17], row1[17];
    // Row 0: P=1013.2 hPa
    snprintf(row0, sizeof(row0), "P%7.1f hPa", lastReading.pressure_hpa);
    // Row 1: T=28.5 V=0.012g
    snprintf(row1, sizeof(row1), "T%4.1fC V%5.3fg",
             lastReading.temp_c, lastReading.vibration_g);
    lcdPrint(row0, row1);
}

void lcdShowLinkQuality() {
    char row0[17], row1[17];
    // Row 0: TTL:8 SF:9
    snprintf(row0, sizeof(row0), "TTL:%-2d SF:%-2d",
             MAX_TTL, radio.currentSF());
    // Row 1: RS:-085 SN:+7
    snprintf(row1, sizeof(row1), "RS:%4d SN:%+3d",
             radio.lastRssi(), radio.lastSnr());
    lcdPrint(row0, row1);
}

void lcdShowLastMessage() {
    char row0[17], row1[17];
    snprintf(row0, sizeof(row0), "MSG<%04u:", lastMsgOrigin);
    // Truncate message to fit 16 chars on row 1
    snprintf(row1, sizeof(row1), "%.16s", lastMsg);
    lcdPrint(row0, row1);
}

void lcdPrint(const char *line0, const char *line1) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line0);
    lcd.setCursor(0, 1);
    lcd.print(line1);
}

void printSepLine() {
    Serial.println(F("--------------------------------------------"));
}

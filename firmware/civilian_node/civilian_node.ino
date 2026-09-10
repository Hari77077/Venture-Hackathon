// =============================================================================
// civilian_node.ino — Flood-mesh Civilian Node  (ESP32)
//
// Hardware:
//   MCU    : ESP32
//   Radio  : SX1278 LoRa  (SCK=18, MISO=19, MOSI=23, NSS=5, RST=14, DIO0=2)
//   LCD    : I2C 16x2 at 0x26  (SDA=21, SCL=22)
//   Buzzer : GPIO 25
//
// Role:
//   • Receives MSG_ALERT / MSG_TEXT from the mesh
//   • LCD shows: telemetry, link quality (TTL/RSSI/SNR/SF), and messages
//   • Messages are shown IMMEDIATELY on LCD the moment they arrive
//   • Buzzer beeps on alerts (continuous for DANGER/EMERGENCY)
//   • Relays packets (acts as passive mesh repeater)
//   • Has NO sensors — does not generate telemetry
//   • Serial-based messaging: type in Serial Monitor to send MSG_TEXT
// =============================================================================

#include <RadioLib.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "../common/mesh_packet.h"
#include "../common/radio.h"

// ---- Node identity --------------------------------------------------------
#define MY_NODE_ID   3001U

// ---- LoRa SPI pins (ESP32 default VSPI) -----------------------------------
// SCK=18, MISO=19, MOSI=23 are the ESP32 VSPI defaults,
// so no custom SPIClass is needed — RadioLib uses them automatically.
#define LORA_NSS     5
#define LORA_DIO0    2
#define LORA_RST    14

// ---- LCD I2C ---------------------------------------------------------------
#define LCD_ADDR    0x26
#define LCD_COLS    16
#define LCD_ROWS     2
#define LCD_SDA     21
#define LCD_SCL     22

// ---- Other pins ------------------------------------------------------------
#define BUZZER_PIN  25

// ---- Timing ----------------------------------------------------------------
#define LCD_CYCLE_MS          4000UL    // rotate LCD screen every 4 s
#define ADAPT_SF_INTERVAL_MS  (3UL * 60UL * 1000UL)

// ---- Hardware instances ----------------------------------------------------
SX1278           radioModule = new Module(LORA_NSS, LORA_DIO0, LORA_RST, RADIOLIB_NC);
MeshRadio        radio(radioModule);
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
SeenCache        seenCache;

// ---- State -----------------------------------------------------------------
bool     radioOk         = false;
uint32_t seqCounter      = 0;
uint32_t lastLcdCycleMs  = 0;
uint32_t lastAdaptSFMs   = 0;
uint8_t  lcdScreen       = 0;    // 0=telemetry, 1=link quality, 2=last message

// Alert display (non-blocking blink)
uint8_t  currentAlertSev = SEV_INFO;
uint32_t lastBlinkMs     = 0;
bool     ledBlinkState   = false;

// Cache of last received telemetry (for LCD display)
float    rxPressure      = 0.0f;
float    rxTemp          = 0.0f;
float    rxHumidity      = 0.0f;
float    rxVibration     = 0.0f;
float    rxWaterLevel    = 0.0f;
uint16_t rxTelOrigin     = 0;

// Cache of last received text message
char     lastMsg[MSG_TEXT_LEN + 1] = "Waiting...";
uint16_t lastMsgOrigin   = 0;

// ---- Forward declarations --------------------------------------------------
void handleAlert(uint8_t severity);
void updateAlertBuzzer();
void checkSerialMessage();
void sendTextMessage(const char *msg);
void updateLCD();
void lcdShowTelemetry();
void lcdShowLinkQuality();
void lcdShowLastMessage();
void lcdPrint(const char *line0, const char *line1);
void printSepLine();

// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);   // ESP32 USB stabilisation

    printSepLine();
    Serial.println(F("=== Civilian Node Boot (ESP32) ==="));
    Serial.print(F("  Node ID : ")); Serial.println(MY_NODE_ID);
    Serial.print(F("  Compiled: ")); Serial.print(F(__DATE__));
    Serial.print(F(" ")); Serial.println(F(__TIME__));
    printSepLine();

    // --- GPIO ---
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // --- I2C for LCD (ESP32 default: SDA=21, SCL=22) ---
    Wire.begin(LCD_SDA, LCD_SCL);
    Serial.print(F("[Setup] I2C  SDA=")); Serial.print(LCD_SDA);
    Serial.print(F("  SCL=")); Serial.println(LCD_SCL);

    // --- LCD ---
    lcd.init();
    lcd.backlight();
    lcdPrint("Civilian Node", "Booting...");
    Serial.println(F("[Setup] LCD OK (0x26)"));
    delay(1000);

    // --- Radio (uses ESP32 default VSPI: 18/19/23/5) ---
    Serial.println(F("[Setup] Initialising radio (SX1278)..."));
    radioOk = radio.begin();
    if (radioOk) {
        lcdPrint("Radio OK", "433 MHz LoRa");
        Serial.println(F("[Setup] Radio OK"));
    } else {
        lcdPrint("RADIO FAIL!", "Check wiring");
        Serial.println(F("[Setup] *** RADIO FAILED ***"));
        // Buzzer alarm + halt
        while (true) {
            digitalWrite(BUZZER_PIN, HIGH); delay(200);
            digitalWrite(BUZZER_PIN, LOW);  delay(200);
        }
    }
    delay(1000);

    randomSeed(analogRead(0));

    Serial.println(F("[Setup] Boot complete. Listening..."));
    Serial.println(F("[Setup] Type a message in Serial Monitor to broadcast."));
    printSepLine();

    lcdPrint("Listening...", "");
    delay(500);
}

// ============================================================================
void loop() {
    uint32_t now = millis();

    // =========================================================
    // 1. CONTINUOUS RECEIVE
    // =========================================================
    MeshPacket pkt;
    if (radio.receive(pkt, 0) == RADIOLIB_ERR_NONE) {
        Serial.print(F("[RX] origin=")); Serial.print(pkt.origin_id);
        Serial.print(F("  seq=")); Serial.print(pkt.seq_num);
        Serial.print(F("  ttl=")); Serial.print(pkt.ttl);
        Serial.print(F("  type=")); Serial.print(pkt.msg_type);
        Serial.print(F("  sev=")); Serial.print(pkt.severity);
        Serial.print(F("  node=")); Serial.print(pkt.node_type);
        Serial.print(F("  RSSI=")); Serial.print(radio.lastRssi());
        Serial.print(F("  SNR=")); Serial.print(radio.lastSnr());
        Serial.print(F("  SF=")); Serial.println(radio.currentSF());

        // --- Telemetry: cache values for LCD ---
        if (pkt.msg_type == MSG_TELEMETRY) {
            rxPressure   = pkt.pressure_hpa;
            rxTemp       = pkt.temp_c;
            rxHumidity   = pkt.humidity_pct;
            rxVibration  = pkt.vibration_g;
            rxWaterLevel = pkt.water_level_cm;
            rxTelOrigin  = pkt.origin_id;

            Serial.print(F("[RX] Telemetry  P=")); Serial.print(rxPressure, 2);
            Serial.print(F(" hPa  T=")); Serial.print(rxTemp, 2);
            Serial.print(F(" C  H=")); Serial.print(rxHumidity, 1);
            Serial.print(F("%  Vib=")); Serial.print(rxVibration, 3);
            Serial.print(F("g  WL=")); Serial.print(rxWaterLevel, 1);
            Serial.println(F(" cm"));
        }

        // --- Alert ---
        if (pkt.msg_type == MSG_ALERT) {
            Serial.print(F("[RX] *** ALERT  sev=")); Serial.println(pkt.severity);
            handleAlert(pkt.severity);
        }

        // --- Text message: show on LCD IMMEDIATELY ---
        if (pkt.msg_type == MSG_TEXT) {
            pkt.message[MSG_TEXT_LEN - 1] = '\0';
            strncpy(lastMsg, pkt.message, MSG_TEXT_LEN);
            lastMsgOrigin = pkt.origin_id;

            Serial.print(F("[RX] *** MESSAGE from "));
            Serial.print(pkt.origin_id);
            Serial.print(F(": \""));
            Serial.print(lastMsg);
            Serial.println(F("\""));

            // Force LCD to message screen immediately
            lcdScreen = 2;
            lastLcdCycleMs = now;
            updateLCD();

            // Short beep to notify
            digitalWrite(BUZZER_PIN, HIGH); delay(100);
            digitalWrite(BUZZER_PIN, LOW);
        }

        // Relay into mesh
        forwardPacket(pkt, seenCache, radio);
    }

    // =========================================================
    // 2. SERIAL MESSAGE INPUT
    // =========================================================
    checkSerialMessage();

    // =========================================================
    // 3. LCD ROTATION (every LCD_CYCLE_MS)
    // =========================================================
    if (now - lastLcdCycleMs >= LCD_CYCLE_MS) {
        lastLcdCycleMs = now;
        lcdScreen = (lcdScreen + 1) % 3;   // 0→1→2→0
        updateLCD();
    }

    // =========================================================
    // 4. NON-BLOCKING ALERT BUZZER
    // =========================================================
    updateAlertBuzzer();

    // =========================================================
    // 5. ADAPTIVE SF
    // =========================================================
    if (now - lastAdaptSFMs >= ADAPT_SF_INTERVAL_MS) {
        lastAdaptSFMs = now;
        radio.adaptSF();
    }
}

// ============================================================================
void handleAlert(uint8_t severity) {
    currentAlertSev = severity;

    if (severity >= SEV_DANGER) {
        digitalWrite(BUZZER_PIN, HIGH);      // continuous
    } else if (severity >= SEV_WARNING) {
        // Pulsing handled in updateAlertBuzzer()
    } else {
        digitalWrite(BUZZER_PIN, LOW);
    }
}

void updateAlertBuzzer() {
    if (currentAlertSev != SEV_WARNING) return;

    uint32_t now = millis();
    if (now - lastBlinkMs >= 500UL) {
        lastBlinkMs = now;
        ledBlinkState = !ledBlinkState;
        digitalWrite(BUZZER_PIN, ledBlinkState ? HIGH : LOW);
    }
}

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
        Serial.println(F("[Msg] Radio down"));
        return;
    }

    MeshPacket pkt = {};
    pkt.origin_id = MY_NODE_ID;
    pkt.seq_num   = seqCounter++;
    pkt.ttl       = MAX_TTL;
    pkt.msg_type  = MSG_TEXT;
    pkt.node_type = NODE_CIVILIAN;
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

    // Show on LCD
    strncpy(lastMsg, pkt.message, MSG_TEXT_LEN);
    lastMsgOrigin = MY_NODE_ID;
    lcdScreen = 2;
    updateLCD();

    // Confirmation beep
    if (ok) {
        for (int i = 0; i < 2; i++) {
            digitalWrite(BUZZER_PIN, HIGH); delay(60);
            digitalWrite(BUZZER_PIN, LOW);  delay(60);
        }
    }
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
    // Row 0: P=1013.2 T=28.5C
    snprintf(row0, sizeof(row0), "P%6.1f T%4.1fC", rxPressure, rxTemp);
    // Row 1: H=82% V=0.01 W=45
    snprintf(row1, sizeof(row1), "H%3.0f%% V%4.2f W%3.0f",
             rxHumidity, rxVibration, rxWaterLevel);
    lcdPrint(row0, row1);
}

void lcdShowLinkQuality() {
    char row0[17], row1[17];
    // Row 0: TTL:8 SF:9
    snprintf(row0, sizeof(row0), "TTL:%-2d SF:%-2d",
             MAX_TTL, radio.currentSF());
    // Row 1: RS:-085 SN:+07
    snprintf(row1, sizeof(row1), "RS:%4d SN:%+3d",
             radio.lastRssi(), radio.lastSnr());
    lcdPrint(row0, row1);
}

void lcdShowLastMessage() {
    char row0[17], row1[17];
    snprintf(row0, sizeof(row0), "MSG<%04u:", lastMsgOrigin);
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

// ============================================================================
void printSepLine() {
    Serial.println(F("--------------------------------------------"));
}

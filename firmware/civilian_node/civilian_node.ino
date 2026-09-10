// =============================================================================
// civilian_node.ino — Flood-mesh Civilian Node
//
// Hardware:
//   • MCU    : Arduino Nano / Uno
//   • Radio  : SX1278 LoRa  (NSS=10, DIO0=2, RST=9, DIO1=3)
//   • LED    : status LED on LED_PIN  (alert indicator)
//   • Buzzer : passive/active buzzer on BUZZER_PIN
//   • Button : distress button on BUTTON_PIN (active LOW, internal pull-up)
//
// Role in mesh:
//   • Receives MSG_ALERT / MSG_TEXT from the mesh and signals the civilian
//     (LED blink for WARNING, buzzer for DANGER/EMERGENCY)
//   • On button press: broadcasts a MSG_TEXT distress call into the mesh
//     so it can reach the Gateway / Anchor and then operators
//   • Relays packets (acts as a passive mesh repeater with TTL decrement)
//   • Has NO sensors — does not generate telemetry
//   • Full Serial debug output for all events
// =============================================================================

#include <RadioLib.h>
#include "../common/mesh_packet.h"
#include "../common/radio.h"

// ---- Node identity (unique per unit, set at flash time) --------------------
#define MY_NODE_ID   3001U

// ---- Pin assignments -------------------------------------------------------
#define NSS_PIN     10
#define DIO0_PIN     2
#define RST_PIN      9
#define DIO1_PIN     3
#define LED_PIN      6
#define BUZZER_PIN   5
#define BUTTON_PIN   7   // active LOW — civilian presses to call for help

// ---- Timing & behaviour ----------------------------------------------------
#define BUTTON_DEBOUNCE_MS   50UL
#define ALERT_LED_BLINK_MS   300UL    // blink period for WARNING
#define DISTRESS_MSG         "HELP - CIVILIAN NODE"
#define ADAPT_SF_INTERVAL_MS (3UL * 60UL * 1000UL)   // adapt SF every 3 min

// ---- Hardware instances ----------------------------------------------------
SX1278    radioModule = new Module(NSS_PIN, DIO0_PIN, RST_PIN, DIO1_PIN);
MeshRadio radio(radioModule);
SeenCache seenCache;

// ---- State -----------------------------------------------------------------
bool     radioOk        = false;
uint32_t seqCounter     = 0;
uint32_t lastAdaptSFMs  = 0;

// Alert display state (non-blocking LED blink)
uint8_t  currentAlertSev    = SEV_INFO;
uint32_t lastBlinkMs        = 0;
bool     ledState           = false;

// Button debounce
bool     buttonWasLow       = false;

// ---- Forward declarations --------------------------------------------------
void handleAlert(uint8_t severity);
void sendDistressMessage();
void updateAlertIndicator();
void printSepLine();

// ============================================================================
void setup() {
    Serial.begin(115200);
    while (!Serial);

    printSepLine();
    Serial.println(F("=== Civilian Node Boot ==="));
    Serial.print(F("  Node ID : "));
    Serial.println(MY_NODE_ID);
    Serial.print(F("  Compiled: "));
    Serial.print(F(__DATE__));
    Serial.print(F(" "));
    Serial.println(F(__TIME__));
    printSepLine();

    pinMode(LED_PIN,    OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    digitalWrite(LED_PIN,    LOW);
    digitalWrite(BUZZER_PIN, LOW);

    randomSeed(analogRead(A0));

    radioOk = radio.begin();
    if (!radioOk) {
        Serial.println(F("[Setup] *** Radio FAILED — check wiring ***"));
        // Blink rapidly to signal hardware fault
        while (true) {
            digitalWrite(LED_PIN, HIGH); delay(100);
            digitalWrite(LED_PIN, LOW);  delay(100);
        }
    }

    Serial.println(F("[Setup] Boot complete. Listening for alerts..."));
    printSepLine();

    // Startup blink — 3 quick flashes to confirm alive
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, HIGH); delay(150);
        digitalWrite(LED_PIN, LOW);  delay(150);
    }
}

// ============================================================================
void loop() {
    uint32_t now = millis();

    // =========================================================
    // 1. CONTINUOUS RECEIVE
    // =========================================================
    MeshPacket pkt;
    if (radio.receive(pkt, 0) == RADIOLIB_ERR_NONE) {
        Serial.print(F("[RX] Packet  origin="));
        Serial.print(pkt.origin_id);
        Serial.print(F("  seq="));
        Serial.print(pkt.seq_num);
        Serial.print(F("  ttl="));
        Serial.print(pkt.ttl);
        Serial.print(F("  type="));
        Serial.print(pkt.msg_type);
        Serial.print(F("  sev="));
        Serial.print(pkt.severity);
        Serial.print(F("  RSSI="));
        Serial.print(radio.lastRssi());
        Serial.print(F(" dBm  SNR="));
        Serial.print(radio.lastSnr());
        Serial.print(F(" dB  SF="));
        Serial.println(radio.currentSF());

        // --- Handle alerts ---
        if (pkt.msg_type == MSG_ALERT) {
            Serial.print(F("[RX] *** ALERT  sev="));
            Serial.println(pkt.severity);
            handleAlert(pkt.severity);
        }

        // --- Handle text messages (print immediately) ---
        if (pkt.msg_type == MSG_TEXT) {
            pkt.message[MSG_TEXT_LEN - 1] = '\0';
            Serial.print(F("[RX] *** MESSAGE from node "));
            Serial.print(pkt.origin_id);
            Serial.print(F(": \""));
            Serial.print(pkt.message);
            Serial.println(F("\""));
            // Also beep briefly to notify civilian
            digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW);
        }

        // Relay packet into mesh
        forwardPacket(pkt, seenCache, radio);
    }

    // =========================================================
    // 2. BUTTON — send distress MSG_TEXT
    // =========================================================
    bool buttonNowLow = (digitalRead(BUTTON_PIN) == LOW);
    if (buttonNowLow && !buttonWasLow) {
        delay(BUTTON_DEBOUNCE_MS);
        if (digitalRead(BUTTON_PIN) == LOW) {
            Serial.println(F("[BTN] Distress button pressed"));
            sendDistressMessage();
        }
    }
    buttonWasLow = buttonNowLow;

    // =========================================================
    // 3. NON-BLOCKING ALERT INDICATOR (LED blink / buzzer)
    // =========================================================
    updateAlertIndicator();

    // =========================================================
    // 4. ADAPTIVE SF
    // =========================================================
    if (now - lastAdaptSFMs >= ADAPT_SF_INTERVAL_MS) {
        lastAdaptSFMs = now;
        radio.adaptSF();
    }
}

// ============================================================================
// handleAlert — set alert severity level, drive initial indicator
// ============================================================================
void handleAlert(uint8_t severity) {
    currentAlertSev = severity;

    switch (severity) {
        case SEV_INFO:
            Serial.println(F("[Alert] SEV_INFO — no action"));
            digitalWrite(LED_PIN,    LOW);
            digitalWrite(BUZZER_PIN, LOW);
            break;

        case SEV_WARNING:
            Serial.println(F("[Alert] SEV_WARNING — LED blinking"));
            // LED blink is handled in updateAlertIndicator()
            break;

        case SEV_DANGER:
            Serial.println(F("[Alert] SEV_DANGER — LED ON + buzzer ON"));
            digitalWrite(LED_PIN,    HIGH);
            digitalWrite(BUZZER_PIN, HIGH);
            break;

        case SEV_EMERGENCY:
            Serial.println(F("[Alert] SEV_EMERGENCY — LED ON + buzzer ON (continuous)"));
            digitalWrite(LED_PIN,    HIGH);
            digitalWrite(BUZZER_PIN, HIGH);
            break;

        default:
            Serial.print(F("[Alert] Unknown severity: "));
            Serial.println(severity);
            break;
    }
}

// ============================================================================
// updateAlertIndicator — non-blocking blink for SEV_WARNING
// ============================================================================
void updateAlertIndicator() {
    if (currentAlertSev != SEV_WARNING) return;

    uint32_t now = millis();
    if (now - lastBlinkMs >= ALERT_LED_BLINK_MS) {
        lastBlinkMs = now;
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
}

// ============================================================================
// sendDistressMessage — broadcast MSG_TEXT into the mesh
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
    pkt.node_type = NODE_CIVILIAN;
    pkt.severity  = SEV_WARNING;
    pkt.rssi_dbm  = radio.lastRssi();
    pkt.snr_db    = radio.lastSnr();
    pkt.sf        = radio.currentSF();
    strncpy(pkt.message, DISTRESS_MSG, MSG_TEXT_LEN - 1);
    pkt.message[MSG_TEXT_LEN - 1] = '\0';

    seenCache.add(pkt.origin_id, pkt.seq_num);

    Serial.print(F("[Msg] Sending distress: \""));
    Serial.print(pkt.message);
    Serial.println(F("\""));

    bool ok = radio.sendWithCAD(pkt);
    Serial.println(ok ? F("[Msg] TX OK") : F("[Msg] TX FAILED"));

    // Confirmation beep
    if (ok) {
        for (int i = 0; i < 2; i++) {
            digitalWrite(BUZZER_PIN, HIGH); delay(80);
            digitalWrite(BUZZER_PIN, LOW);  delay(80);
        }
    }
}

// ============================================================================
void printSepLine() {
    Serial.println(F("--------------------------------------------"));
}

// =============================================================================
// anchor.ino — Flood-mesh Anchor Node
//
// Hardware:
//   • MCU     : Arduino Mega / Nano / similar
//   • Radio   : SX1278 LoRa  (NSS=10, DIO0=2, RST=9, DIO1=3)
//   • UART    : Serial (USB / UART0) → Gateway / Raspberry Pi
//
// Role in mesh:
//   • Relay: receives ALL packet types from the mesh and forwards them
//   • Bridge: passes every received packet to the Gateway over UART
//   • Downlink: receives alert / text commands from Gateway over UART
//             and injects them into the mesh
//   • Does NOT have sensors (no BMP180/MPU6050 required)
//   • Logs RSSI/SNR for every received packet for diagnostics
//
// UART frame format (same as reciever.ino / uart_bridge.h):
//   [0xAA][len:1][MeshPacket:len]
// =============================================================================

#include <RadioLib.h>
#include "../common/mesh_packet.h"
#include "../common/radio.h"
#include "uart_bridge.h"

// ---- Node identity (change per physical unit at flash time) ----------------
#define MY_NODE_ID   2001U

// ---- Pin assignments -------------------------------------------------------
#define NSS_PIN   10
#define DIO0_PIN   2
#define RST_PIN    9
#define DIO1_PIN   3
#define LED_PIN    6    // blinks on every packet relayed

// ---- UART ------------------------------------------------------------------
#define UART_BAUD  115200

// ---- Hardware instances ----------------------------------------------------
SX1278      radioModule = new Module(NSS_PIN, DIO0_PIN, RST_PIN, DIO1_PIN);
MeshRadio   radio(radioModule);
UartBridge  gateway(Serial);
SeenCache   seenCache;

// ---- State -----------------------------------------------------------------
bool     radioOk        = false;
uint32_t rxCount        = 0;
uint32_t fwdCount       = 0;
uint32_t lastAdaptSFMs  = 0;
#define ADAPT_SF_INTERVAL_MS  (2UL * 60UL * 1000UL)

// ---- Forward declaration ---------------------------------------------------
void printSepLine();

// ============================================================================
void setup() {
    Serial.begin(UART_BAUD);
    while (!Serial);

    printSepLine();
    Serial.println(F("=== Anchor Node Boot ==="));
    Serial.print(F("  Node ID : "));
    Serial.println(MY_NODE_ID);
    Serial.print(F("  Compiled: "));
    Serial.print(F(__DATE__));
    Serial.print(F(" "));
    Serial.println(F(__TIME__));
    printSepLine();

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    randomSeed(analogRead(A0));

    // Note: gateway.begin() is called with same Serial used for debug.
    // On a Mega, use Serial1/Serial2 for UART bridge and keep Serial0 for debug.
    // For Nano/Uno, we share Serial for both (UART bridge takes priority).
    gateway.begin(UART_BAUD);
    Serial.println(F("[Setup] UART bridge initialised (115200 baud)"));

    radioOk = radio.begin();
    if (!radioOk) {
        Serial.println(F("[Setup] *** Radio FAILED — halting ***"));
        while (true) {
            digitalWrite(LED_PIN, HIGH); delay(100);
            digitalWrite(LED_PIN, LOW);  delay(100);
        }
    }

    Serial.println(F("[Setup] Boot complete. Anchor running."));
    printSepLine();
}

// ============================================================================
void loop() {
    uint32_t now = millis();

    // =========================================================
    // 1. MESH → GATEWAY direction
    // =========================================================
    MeshPacket pkt;
    if (radio.receive(pkt, 0) == RADIOLIB_ERR_NONE) {
        rxCount++;
        lastAdaptSFMs = now;   // reset adapt timer on activity

        Serial.print(F("[RX] #"));
        Serial.print(rxCount);
        Serial.print(F("  origin="));
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

        if (pkt.msg_type == MSG_TEXT) {
            pkt.message[MSG_TEXT_LEN - 1] = '\0';
            Serial.print(F("[RX] MSG_TEXT: \""));
            Serial.print(pkt.message);
            Serial.println(F("\""));
        }

        // Stamp link quality before bridging to gateway
        pkt.rssi_dbm = radio.lastRssi();
        pkt.snr_db   = radio.lastSnr();
        pkt.sf       = radio.currentSF();

        // Forward ALL packet types to Gateway over UART
        gateway.sendToGateway(pkt);
        Serial.println(F("[Anchor] Packet forwarded to Gateway via UART"));

        // Relay into mesh (alerts/texts with priority jitter, telemetry normal)
        bool forwarded = forwardPacket(pkt, seenCache, radio);
        if (forwarded) {
            fwdCount++;
            Serial.print(F("[Anchor] Relayed into mesh  total_fwd="));
            Serial.println(fwdCount);
            // Blink LED briefly
            digitalWrite(LED_PIN, HIGH);
            delay(20);
            digitalWrite(LED_PIN, LOW);
        }
    }

    // =========================================================
    // 2. GATEWAY → MESH direction (alert / text injection)
    // =========================================================
    MeshPacket alertPkt;
    if (gateway.pollFromGateway(alertPkt)) {
        // Ensure injected packet has correct routing metadata
        alertPkt.node_type = NODE_ANCHOR;
        alertPkt.ttl       = MAX_TTL;
        // msg_type is already set by gateway (MSG_ALERT or MSG_TEXT)
        seenCache.add(alertPkt.origin_id, alertPkt.seq_num);

        Serial.print(F("[GW→Mesh] Received from Gateway  type="));
        Serial.print(alertPkt.msg_type);
        Serial.print(F("  sev="));
        Serial.println(alertPkt.severity);

        if (alertPkt.msg_type == MSG_TEXT) {
            alertPkt.message[MSG_TEXT_LEN - 1] = '\0';
            Serial.print(F("[GW→Mesh] Text: \""));
            Serial.print(alertPkt.message);
            Serial.println(F("\""));
        }

        bool ok = radio.sendWithCAD(alertPkt);
        Serial.println(ok ? F("[GW→Mesh] Injected OK") : F("[GW→Mesh] Inject FAILED"));
    }

    // =========================================================
    // 3. ADAPTIVE SF
    // =========================================================
    if (now - lastAdaptSFMs >= ADAPT_SF_INTERVAL_MS) {
        lastAdaptSFMs = now;
        radio.adaptSF();
    }
}

// ============================================================================
void printSepLine() {
    Serial.println(F("--------------------------------------------"));
}

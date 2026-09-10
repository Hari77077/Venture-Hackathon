// =============================================================================
// anchor.ino — Flood-mesh Anchor Node  (Arduino Uno)
//
// Hardware:
//   MCU    : Arduino Uno (ATmega328P)
//   Radio  : SX1278 LoRa
//     SCK  = Pin 13  (5V→3.3V level shift)
//     MISO = Pin 12  (direct — 3.3V output reads as HIGH on Uno)
//     MOSI = Pin 11  (5V→3.3V level shift)
//     NSS  = Pin 10  (5V→3.3V level shift)
//     RST  = Pin 9   (5V→3.3V level shift)
//     DIO0 = Pin 2   (direct — interrupt 0)
//   UART   : Serial (USB) → Gateway (Raspberry Pi / PC)
//
// Role:
//   • Relay: receives ALL packet types from mesh and forwards them
//   • Bridge: passes every received packet to Gateway over UART
//   • Downlink: receives commands from Gateway over UART → mesh
// =============================================================================

#include <RadioLib.h>
#include "../common/mesh_packet.h"
#include "../common/radio.h"
#include "uart_bridge.h"

// ---- Node identity --------------------------------------------------------
#define MY_NODE_ID   2001U

// ---- Pin assignments (Arduino Uno, default SPI) ---------------------------
// SCK=13, MISO=12, MOSI=11 are hardware SPI defaults — no remapping needed.
#define LORA_NSS     10
#define LORA_DIO0     2
#define LORA_RST      9

// ---- UART ------------------------------------------------------------------
#define UART_BAUD  115200

// ---- Hardware instances ----------------------------------------------------
SX1278      radioModule = new Module(LORA_NSS, LORA_DIO0, LORA_RST, RADIOLIB_NC);
MeshRadio   radio(radioModule);
UartBridge  gateway(Serial);
SeenCache   seenCache;

// ---- State -----------------------------------------------------------------
bool     radioOk        = false;
uint32_t rxCount        = 0;
uint32_t fwdCount       = 0;
uint32_t lastAdaptSFMs  = 0;
#define ADAPT_SF_INTERVAL_MS  (2UL * 60UL * 1000UL)

// ---- Forward declarations --------------------------------------------------
void printSepLine();

// ============================================================================
void setup() {
    Serial.begin(UART_BAUD);

    printSepLine();
    Serial.println(F("=== Anchor Node Boot (Uno) ==="));
    Serial.print(F("  Node ID : ")); Serial.println(MY_NODE_ID);
    Serial.print(F("  Compiled: ")); Serial.print(F(__DATE__));
    Serial.print(F(" ")); Serial.println(F(__TIME__));
    printSepLine();

    // Note: on Uno, pin 13 (LED_BUILTIN) is also SCK — can't use for LED.
    randomSeed(analogRead(A0));

    gateway.begin(UART_BAUD);
    Serial.println(F("[Setup] UART bridge ready"));

    radioOk = radio.begin();
    if (!radioOk) {
        Serial.println(F("[Setup] *** Radio FAILED ***"));
        while (true) { delay(1000); }   // halt
    }

    Serial.println(F("[Setup] Boot complete. Anchor running."));
    printSepLine();
}

// ============================================================================
void loop() {
    uint32_t now = millis();

    // =========================================================
    // 1. MESH → GATEWAY
    // =========================================================
    MeshPacket pkt;
    if (radio.receive(pkt, 0) == RADIOLIB_ERR_NONE) {
        rxCount++;

        // Stamp link quality
        pkt.rssi_dbm = radio.lastRssi();
        pkt.snr_db   = radio.lastSnr();
        pkt.sf       = radio.currentSF();

        Serial.print(F("[RX] #")); Serial.print(rxCount);
        Serial.print(F("  origin=")); Serial.print(pkt.origin_id);
        Serial.print(F("  seq=")); Serial.print(pkt.seq_num);
        Serial.print(F("  ttl=")); Serial.print(pkt.ttl);
        Serial.print(F("  type=")); Serial.print(pkt.msg_type);
        Serial.print(F("  RSSI=")); Serial.print(radio.lastRssi());
        Serial.print(F("  SNR=")); Serial.print(radio.lastSnr());
        Serial.print(F("  SF=")); Serial.println(radio.currentSF());

        if (pkt.msg_type == MSG_TEXT) {
            pkt.message[MSG_TEXT_LEN - 1] = '\0';
            Serial.print(F("[RX] MSG: \"")); Serial.print(pkt.message); Serial.println(F("\""));
        }

        // Forward to gateway via UART
        gateway.sendToGateway(pkt);
        Serial.println(F("[Anchor] → Gateway"));

        // Relay into mesh
        bool fwd = forwardPacket(pkt, seenCache, radio);
        if (fwd) fwdCount++;
    }

    // =========================================================
    // 2. GATEWAY → MESH
    // =========================================================
    MeshPacket alertPkt;
    if (gateway.pollFromGateway(alertPkt)) {
        alertPkt.node_type = NODE_ANCHOR;
        alertPkt.ttl       = MAX_TTL;
        seenCache.add(alertPkt.origin_id, alertPkt.seq_num);

        Serial.print(F("[GW→Mesh] type=")); Serial.print(alertPkt.msg_type);
        Serial.print(F("  sev=")); Serial.println(alertPkt.severity);

        bool ok = radio.sendWithCAD(alertPkt);
        Serial.println(ok ? F("[GW→Mesh] OK") : F("[GW→Mesh] FAILED"));
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

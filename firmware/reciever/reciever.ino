// =============================================================================
// reciever.ino — Flood-mesh Gateway Receiver / UART Bridge  (Arduino Uno)
//
// Hardware:
//   MCU    : Arduino Uno (ATmega328P)
//   Radio  : SX1278 LoRa
//     SCK  = Pin 13   (hardware SPI)
//     MISO = Pin 12
//     MOSI = Pin 11
//     NSS  = Pin 10
//     RST  = Pin 9
//     DIO0 = Pin 2    (interrupt 0)
//   UART   : USB serial → PC running Go backend
//
// Role:
//   • Edge receiver: captures ALL mesh packets and bridges to PC via USB serial
//   • Stamps RSSI/SNR/SF before forwarding
//   • Accepts injected packets from the PC and broadcasts them into the mesh
//   • Does NOT relay packets back into the mesh (edge node, not interior relay)
//
// NOTE: On Arduino Uno, Serial is shared between debug output and the UART
// bridge. The Go backend's serial ingest must scan for 0xAA frame markers and
// skip any debug text. Consider using Arduino Mega with Serial1 for the bridge
// if this causes issues.
// =============================================================================

#include <RadioLib.h>
#include "../common/mesh_packet.h"
#include "../common/radio.h"
#include "uart_bridge.h"

// ---- Node identity --------------------------------------------------------
#define MY_NODE_ID   4001U

// ---- Pin assignments (Arduino Uno default SPI) ----------------------------
#define LORA_NSS     10
#define LORA_DIO0     2
#define LORA_RST      9

// ---- UART ------------------------------------------------------------------
#define UART_BAUD  115200

// ---- Hardware instances ----------------------------------------------------
SX1278     radioModule = new Module(LORA_NSS, LORA_DIO0, LORA_RST, RADIOLIB_NC);
MeshRadio  radio(radioModule);
UartBridge gateway(Serial);
SeenCache  seenCache;

// ---- State -----------------------------------------------------------------
bool     radioOk       = false;
uint32_t rxCount       = 0;
uint32_t lastAdaptSFMs = 0;
#define ADAPT_SF_INTERVAL_MS  (2UL * 60UL * 1000UL)

// ---- Forward declarations --------------------------------------------------
void printSepLine();

// ============================================================================
void setup() {
    Serial.begin(UART_BAUD);

    printSepLine();
    Serial.println(F("=== Receiver / UART Bridge Boot (Uno) ==="));
    Serial.print(F("  Node ID : ")); Serial.println(MY_NODE_ID);
    Serial.print(F("  Compiled: ")); Serial.print(F(__DATE__));
    Serial.print(F(" ")); Serial.println(F(__TIME__));
    printSepLine();

    randomSeed(analogRead(A0));

    gateway.begin(UART_BAUD);
    Serial.println(F("[Setup] UART bridge ready"));

    radioOk = radio.begin();
    if (!radioOk) {
        Serial.println(F("[Setup] *** Radio FAILED ***"));
        while (true) { delay(1000); }
    }

    Serial.println(F("[Setup] Boot complete. Bridging mesh → PC."));
    printSepLine();
}

// ============================================================================
void loop() {
    uint32_t now = millis();

    // =========================================================
    // 1. MESH → PC (via UART)
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
        Serial.print(F("  node=")); Serial.print(pkt.node_type);
        Serial.print(F("  sev=")); Serial.print(pkt.severity);
        Serial.print(F("  P=")); Serial.print(pkt.pressure_hpa, 2);
        Serial.print(F("  T=")); Serial.print(pkt.temp_c, 2);
        Serial.print(F("  H=")); Serial.print(pkt.humidity_pct, 1);
        Serial.print(F("  Vib=")); Serial.print(pkt.vibration_g, 3);
        Serial.print(F("  WL=")); Serial.print(pkt.water_level_cm, 1);
        Serial.print(F("  RSSI=")); Serial.print(pkt.rssi_dbm);
        Serial.print(F("  SNR=")); Serial.print(pkt.snr_db);
        Serial.print(F("  SF=")); Serial.println(pkt.sf);

        if (pkt.msg_type == MSG_TEXT) {
            pkt.message[MSG_TEXT_LEN - 1] = '\0';
            Serial.print(F("[RX] MSG: \"")); Serial.print(pkt.message); Serial.println(F("\""));
        }

        // Bridge to PC
        gateway.sendToGateway(pkt);
    }

    // =========================================================
    // 2. PC → MESH INJECTION
    // =========================================================
    MeshPacket inject;
    if (gateway.pollFromGateway(inject)) {
        inject.node_type = NODE_RECEIVER;
        inject.ttl       = MAX_TTL;

        Serial.print(F("[PC→Mesh] type=")); Serial.print(inject.msg_type);
        Serial.print(F("  sev=")); Serial.println(inject.severity);

        seenCache.add(inject.origin_id, inject.seq_num);
        bool ok = radio.sendWithCAD(inject);
        Serial.println(ok ? F("[PC→Mesh] OK") : F("[PC→Mesh] FAILED"));
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

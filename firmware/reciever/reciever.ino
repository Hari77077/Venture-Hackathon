// =============================================================================
// reciever.ino — Flood-mesh Gateway Receiver / UART Bridge Node
//
// Hardware:
//   • MCU    : Arduino Mega / Nano (on Mega: use Serial1 for radio, Serial for PC)
//   • Radio  : SX1278 LoRa  (NSS=10, DIO0=2, RST=9, DIO1=3)
//   • UART   : Serial → Gateway (Raspberry Pi / PC)
//
// Role in mesh:
//   • Passively receives ALL packet types from the mesh
//   • Forwards every packet to the connected Gateway over UART
//     (framing: [0xAA][len][MeshPacket bytes])
//   • Listens for injected packets from the Gateway and broadcasts them
//   • Does NOT relay packets back into the mesh (it's an edge receiver, not
//     an interior relay — unlike the Anchor which does both)
//   • Full Serial debug for every RX/TX event
// =============================================================================

#include <RadioLib.h>
#include "../common/mesh_packet.h"
#include "../common/radio.h"
#include "uart_bridge.h"

// ---- Node identity ---------------------------------------------------------
#define MY_NODE_ID   4001U

// ---- Pin assignments -------------------------------------------------------
#define NSS_PIN   10
#define DIO0_PIN   2
#define RST_PIN    9
#define DIO1_PIN   3
#define LED_PIN    6

// ---- UART ------------------------------------------------------------------
#define UART_BAUD  115200

// ---- Hardware instances ----------------------------------------------------
SX1278     radioModule = new Module(NSS_PIN, DIO0_PIN, RST_PIN, DIO1_PIN);
MeshRadio  radio(radioModule);
UartBridge gateway(Serial);
SeenCache  seenCache;   // used only for downlink injection dedup

// ---- State -----------------------------------------------------------------
bool     radioOk    = false;
uint32_t rxCount    = 0;
uint32_t lastAdaptSFMs = 0;
#define ADAPT_SF_INTERVAL_MS  (2UL * 60UL * 1000UL)

// ---- Forward declarations --------------------------------------------------
void printSepLine();

// ============================================================================
void setup() {
    Serial.begin(UART_BAUD);
    while (!Serial);

    printSepLine();
    Serial.println(F("=== Receiver / UART Bridge Boot ==="));
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

    gateway.begin(UART_BAUD);
    Serial.println(F("[Setup] UART bridge ready (115200 baud)"));

    radioOk = radio.begin();
    if (!radioOk) {
        Serial.println(F("[Setup] *** Radio FAILED — halting ***"));
        while (true) {
            digitalWrite(LED_PIN, HIGH); delay(150);
            digitalWrite(LED_PIN, LOW);  delay(150);
        }
    }

    Serial.println(F("[Setup] Boot complete. Bridging mesh → UART."));
    printSepLine();
}

// ============================================================================
void loop() {
    uint32_t now = millis();

    // =========================================================
    // 1. MESH → UART GATEWAY
    // =========================================================
    MeshPacket pkt;
    if (radio.receive(pkt, 0) == RADIOLIB_ERR_NONE) {
        rxCount++;

        // Stamp last-hop link quality so the Gateway sees RF conditions
        pkt.rssi_dbm = radio.lastRssi();
        pkt.snr_db   = radio.lastSnr();
        pkt.sf       = radio.currentSF();

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
        Serial.print(F("  node_type="));
        Serial.print(pkt.node_type);
        Serial.print(F("  sev="));
        Serial.print(pkt.severity);
        Serial.print(F("  P="));
        Serial.print(pkt.pressure_hpa, 2);
        Serial.print(F("hPa  T="));
        Serial.print(pkt.temp_c, 2);
        Serial.print(F("°C  Vib="));
        Serial.print(pkt.vibration_g, 3);
        Serial.print(F("g  RSSI="));
        Serial.print(pkt.rssi_dbm);
        Serial.print(F("dBm  SNR="));
        Serial.print(pkt.snr_db);
        Serial.print(F("dB  SF="));
        Serial.println(pkt.sf);

        if (pkt.msg_type == MSG_TEXT) {
            pkt.message[MSG_TEXT_LEN - 1] = '\0';
            Serial.print(F("[RX] MSG_TEXT: \""));
            Serial.print(pkt.message);
            Serial.println(F("\""));
        }

        // Bridge to Gateway (no dedup — the Gateway handles that)
        gateway.sendToGateway(pkt);
        Serial.println(F("[Bridge] Sent to Gateway via UART"));

        // LED blink on each bridged packet
        digitalWrite(LED_PIN, HIGH);
        delay(10);
        digitalWrite(LED_PIN, LOW);
    }

    // =========================================================
    // 2. GATEWAY → MESH INJECTION (alerts / text from server)
    // =========================================================
    MeshPacket inject;
    if (gateway.pollFromGateway(inject)) {
        inject.node_type = NODE_RECEIVER;
        inject.ttl       = MAX_TTL;

        Serial.print(F("[GW→Mesh] Injecting  type="));
        Serial.print(inject.msg_type);
        Serial.print(F("  sev="));
        Serial.println(inject.severity);

        if (inject.msg_type == MSG_TEXT) {
            inject.message[MSG_TEXT_LEN - 1] = '\0';
            Serial.print(F("[GW→Mesh] Text: \""));
            Serial.print(inject.message);
            Serial.println(F("\""));
        }

        seenCache.add(inject.origin_id, inject.seq_num);
        bool ok = radio.sendWithCAD(inject);
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

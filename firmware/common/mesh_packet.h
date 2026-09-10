#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Mesh packet shared across ALL node types.
// Keep fields packed and ordered from largest to smallest alignment to avoid
// silent compiler padding that would break sizeof() comparisons on the other
// end of the UART bridge.
// ---------------------------------------------------------------------------

#define SEEN_CACHE_SIZE  64
#define MAX_TTL           8
#define MSG_TEXT_LEN     32   // max chars in a human-readable message payload

// ---- Message types --------------------------------------------------------
enum MsgType : uint8_t {
    MSG_TELEMETRY = 0,   // periodic sensor reading (sentinel / anchor)
    MSG_ALERT     = 1,   // severity-coded alert injected by gateway or fallback
    MSG_TEXT      = 2    // short human-readable message (civilian distress / ops)
};

// ---- Severity (meaningful for MSG_ALERT and MSG_TEXT) ---------------------
enum Severity : uint8_t {
    SEV_INFO      = 0,
    SEV_WARNING   = 1,
    SEV_DANGER    = 2,
    SEV_EMERGENCY = 3
};

// ---- Node roles (self-declared in every packet) ---------------------------
enum NodeType : uint8_t {
    NODE_SENTINEL = 0,
    NODE_ANCHOR   = 1,
    NODE_CIVILIAN = 2,
    NODE_RECEIVER = 3   // gateway-side receiver / UART bridge
};

// ---------------------------------------------------------------------------
// MeshPacket — the single on-air structure for ALL message types.
//
// Telemetry fields (pressure_hpa, temp_c, vibration_g) are populated by
// sentinel / anchor nodes and left 0 by civilian nodes.
//
// Link quality fields (rssi, snr, sf) are filled by the *receiving* node
// before it rebroadcasts — so the gateway sees the last-hop RF quality.
//
// message[] is used for MSG_TEXT; always null-terminated, empty string
// for telemetry / alerts.
// ---------------------------------------------------------------------------
struct MeshPacket {
    // --- Routing header (8 bytes) ---
    uint16_t  origin_id;        // unique node ID, set once at flash time
    uint32_t  seq_num;          // rolling counter, wraps at 2^32
    uint8_t   ttl;              // decremented each hop; drop when 0
    uint8_t   msg_type;         // MsgType enum
    uint8_t   node_type;        // NodeType enum
    uint8_t   severity;         // Severity enum (meaningful for ALERT/TEXT)

    // --- Sensor payload (12 bytes) ---
    float     pressure_hpa;     // absolute pressure (BMP180)
    float     temp_c;           // temperature (BMP180)
    float     vibration_g;      // accel magnitude minus gravity (MPU6050)

    // --- Link quality — filled by last forwarder (4 bytes) ----------------
    int16_t   rssi_dbm;         // RSSI in dBm (e.g. -85)
    int8_t    snr_db;           // SNR in dB, signed
    uint8_t   sf;               // LoRa spreading factor (7–12)

    // --- Text payload (MSG_TEXT only) (32 bytes) --------------------------
    char      message[MSG_TEXT_LEN];

} __attribute__((packed));

// Total: 2+4+1+1+1+1 + 4+4+4 + 2+1+1 + 32 = 58 bytes

// ---------------------------------------------------------------------------
// SeenCache — circular duplicate-suppression cache.
// Nodes check this before forwarding to break routing loops.
// ---------------------------------------------------------------------------
struct SeenEntry {
    uint16_t origin_id;
    uint32_t seq_num;
};

class SeenCache {
public:
    bool isDuplicate(uint16_t origin_id, uint32_t seq_num) const {
        for (const auto &e : entries) {
            if (e.origin_id == origin_id && e.seq_num == seq_num) return true;
        }
        return false;
    }

    void add(uint16_t origin_id, uint32_t seq_num) {
        entries[idx] = {origin_id, seq_num};
        idx = (idx + 1) % SEEN_CACHE_SIZE;
    }

private:
    SeenEntry entries[SEEN_CACHE_SIZE] = {};
    uint8_t   idx = 0;
};

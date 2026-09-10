#pragma once
#include <RadioLib.h>
#include "mesh_packet.h"

// ---------------------------------------------------------------------------
// MeshRadio — wraps a RadioLib SX1278 with:
//   • CAD (channel activity detection) before every transmit
//   • Non-blocking receive with a caller-supplied timeout
//   • Link-quality accessors (RSSI, SNR, SF) for the last received frame
// ---------------------------------------------------------------------------
//
// SX1278 pin mapping (matches all nodes in this project):
//   NSS  = 10   (CS)
//   DIO0 = 2    (interrupt / done)
//   RST  = 9
//   DIO1 = 3    (FSK / LoRa mode select — tie to GND if unused)
//
// Frequency: 433 MHz (legal ISM band for India)
// Default SF:  9  (balanced range vs. airtime)
// BW:        125 kHz
// CR:         4/5
// ---------------------------------------------------------------------------

#define LORA_FREQ_MHZ    433.0f
#define LORA_BW_KHZ      125.0f
#define LORA_SF_DEFAULT    9
#define LORA_CR            5      // 4/5
#define LORA_PREAMBLE     8
#define LORA_POWER_DBM   17      // SX1278 max for 433 MHz PA

class MeshRadio {
public:
    explicit MeshRadio(SX1278 &radio) : _radio(radio) {}

    // Call once in setup(). Returns true if radio came up cleanly.
    bool begin() {
        int state = _radio.begin(
            LORA_FREQ_MHZ,
            LORA_BW_KHZ,
            LORA_SF_DEFAULT,
            LORA_CR,
            RADIOLIB_SX127X_SYNC_WORD,
            LORA_POWER_DBM,
            LORA_PREAMBLE
        );
        if (state == RADIOLIB_ERR_NONE) {
            _currentSF = LORA_SF_DEFAULT;
            Serial.print(F("[Radio] OK  freq="));
            Serial.print(LORA_FREQ_MHZ);
            Serial.print(F(" MHz  SF="));
            Serial.print(_currentSF);
            Serial.print(F("  BW="));
            Serial.print(LORA_BW_KHZ);
            Serial.println(F(" kHz"));
        } else {
            Serial.print(F("[Radio] INIT FAILED  state="));
            Serial.println(state);
        }
        return state == RADIOLIB_ERR_NONE;
    }

    // ---- CAD-guarded transmit --------------------------------------------
    // Returns true on clean send. On a busy channel, backs off once then
    // tries again; gives up rather than blocking the loop.
    bool sendWithCAD(const MeshPacket &pkt) {
        if (_channelBusy()) {
            uint32_t backoff = random(20, 120);
            Serial.print(F("[Radio] CAD busy, backing off "));
            Serial.print(backoff);
            Serial.println(F(" ms"));
            delay(backoff);
            if (_channelBusy()) {
                Serial.println(F("[Radio] CAD still busy, dropping TX"));
                return false;
            }
        }

        int state = _radio.transmit((uint8_t *)&pkt, sizeof(MeshPacket));
        if (state == RADIOLIB_ERR_NONE) {
            Serial.print(F("[Radio] TX OK  origin="));
            Serial.print(pkt.origin_id);
            Serial.print(F("  seq="));
            Serial.print(pkt.seq_num);
            Serial.print(F("  ttl="));
            Serial.print(pkt.ttl);
            Serial.print(F("  type="));
            Serial.println(pkt.msg_type);
        } else {
            Serial.print(F("[Radio] TX FAILED  state="));
            Serial.println(state);
        }
        return state == RADIOLIB_ERR_NONE;
    }

    // ---- Non-blocking receive --------------------------------------------
    // Polls for a complete packet; returns RADIOLIB_ERR_NONE on success.
    // timeoutMs = 0  → pure poll (returns immediately if nothing pending)
    // timeoutMs > 0  → wait up to that many milliseconds
    int receive(MeshPacket &pkt, uint32_t timeoutMs = 0) {
        // Switch to continuous-receive mode if not already there.
        // RadioLib's receive(timeout) with timeout=0 is non-blocking.
        int state = _radio.receive((uint8_t *)&pkt, sizeof(MeshPacket),
                                   (uint32_t)timeoutMs);
        if (state == RADIOLIB_ERR_NONE) {
            // Cache link quality for the caller / forwardPacket()
            _lastRssi = (int16_t)_radio.getRSSI();
            _lastSnr  = (int8_t) _radio.getSNR();

            Serial.print(F("[Radio] RX OK  origin="));
            Serial.print(pkt.origin_id);
            Serial.print(F("  seq="));
            Serial.print(pkt.seq_num);
            Serial.print(F("  ttl="));
            Serial.print(pkt.ttl);
            Serial.print(F("  type="));
            Serial.print(pkt.msg_type);
            Serial.print(F("  RSSI="));
            Serial.print(_lastRssi);
            Serial.print(F(" dBm  SNR="));
            Serial.print(_lastSnr);
            Serial.print(F(" dB  SF="));
            Serial.println(_currentSF);
        }
        return state;
    }

    // ---- Link-quality accessors (call after receive()) ------------------
    int16_t lastRssi() const { return _lastRssi; }
    int8_t  lastSnr()  const { return _lastSnr;  }
    uint8_t currentSF() const { return _currentSF; }

    // ---- Adaptive SF (call periodically to tune range vs. airtime) ------
    // Simple heuristic: if recent RSSI is strong, lower SF to save airtime;
    // if weak, raise SF to buy range. Clamps to [7, 12].
    void adaptSF() {
        uint8_t newSF = _currentSF;
        if (_lastRssi > -70 && _currentSF > 7) {
            newSF = _currentSF - 1;
        } else if (_lastRssi < -110 && _currentSF < 12) {
            newSF = _currentSF + 1;
        }
        if (newSF != _currentSF) {
            int state = _radio.setSpreadingFactor(newSF);
            if (state == RADIOLIB_ERR_NONE) {
                Serial.print(F("[Radio] SF adapted: "));
                Serial.print(_currentSF);
                Serial.print(F(" → "));
                Serial.println(newSF);
                _currentSF = newSF;
            } else {
                Serial.print(F("[Radio] SF adapt FAILED  state="));
                Serial.println(state);
            }
        }
    }

private:
    SX1278 &_radio;
    int16_t _lastRssi  = 0;
    int8_t  _lastSnr   = 0;
    uint8_t _currentSF = LORA_SF_DEFAULT;

    bool _channelBusy() {
        int state = _radio.scanChannel();
        return state == RADIOLIB_LORA_DETECTED;
    }
};

// ---------------------------------------------------------------------------
// forwardPacket() — shared relay logic used by ALL node types.
//
// Stamps the last-hop link quality from `radio` into the packet before
// retransmitting, so the gateway always sees how the packet arrived at the
// most-recent relay — useful for network-topology inference.
// ---------------------------------------------------------------------------
inline bool forwardPacket(MeshPacket pkt, SeenCache &cache, MeshRadio &radio) {
    if (cache.isDuplicate(pkt.origin_id, pkt.seq_num)) {
        Serial.print(F("[Fwd] DUP dropped  origin="));
        Serial.print(pkt.origin_id);
        Serial.print(F("  seq="));
        Serial.println(pkt.seq_num);
        return false;
    }
    cache.add(pkt.origin_id, pkt.seq_num);

    if (pkt.ttl == 0) {
        Serial.print(F("[Fwd] TTL=0 dropped  origin="));
        Serial.println(pkt.origin_id);
        return false;
    }
    pkt.ttl--;

    // Stamp last-hop link quality into packet
    pkt.rssi_dbm = radio.lastRssi();
    pkt.snr_db   = radio.lastSnr();
    pkt.sf       = radio.currentSF();

    // Alerts / text get a short jitter (priority); telemetry gets a longer
    // jitter to spread rebroadcasts and reduce collision probability.
    uint32_t jitter_ms = (pkt.msg_type == MSG_ALERT || pkt.msg_type == MSG_TEXT)
                         ? random(10, 60)
                         : random(50, 300);

    Serial.print(F("[Fwd] forwarding  origin="));
    Serial.print(pkt.origin_id);
    Serial.print(F("  seq="));
    Serial.print(pkt.seq_num);
    Serial.print(F("  ttl="));
    Serial.print(pkt.ttl);
    Serial.print(F("  jitter="));
    Serial.print(jitter_ms);
    Serial.println(F(" ms"));

    delay(jitter_ms);
    return radio.sendWithCAD(pkt);
}
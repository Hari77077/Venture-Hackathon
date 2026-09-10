#pragma once
#include <Arduino.h>
#include "../common/mesh_packet.h"

// ---------------------------------------------------------------------------
// UartBridge — framed serial protocol between Anchor/Receiver and Gateway.
//
// Frame format:
//   [0xAA]          — start marker (1 byte)
//   [len]           — payload length = sizeof(MeshPacket) (1 byte)
//   [payload]       — raw MeshPacket bytes (len bytes)
//
// Note: No CRC for hackathon simplicity. Add a CRC-8 byte before shipping
// production hardware over long/noisy UART runs.
//
// Important on Arduino Nano/Uno: Serial0 is shared for both debug output
// and this bridge. On Arduino Mega, wire the Gateway to Serial1 and change
// the constructor argument accordingly.
// ---------------------------------------------------------------------------

class UartBridge {
public:
    explicit UartBridge(HardwareSerial &serial) : _serial(serial) {}

    void begin(unsigned long baud) {
        _serial.begin(baud);
    }

    // Send a MeshPacket to the Gateway.
    void sendToGateway(const MeshPacket &pkt) {
        uint8_t len = (uint8_t)sizeof(MeshPacket);
        _serial.write((uint8_t)0xAA);
        _serial.write(len);
        _serial.write((const uint8_t *)&pkt, len);
    }

    // Non-blocking: returns true and fills `out` if a complete, valid frame
    // was waiting in the UART buffer. Call every loop() iteration.
    //
    // Handles desync: if the first byte isn't 0xAA, it is dropped and the
    // function returns false so the caller retries next iteration.
    bool pollFromGateway(MeshPacket &out) {
        // Need at least the start marker before doing anything
        if (_serial.available() < 1) return false;

        if (_serial.peek() != (int)0xAA) {
            uint8_t dropped = (uint8_t)_serial.read();
            Serial.print(F("[UartBridge] Desync — dropped byte 0x"));
            Serial.println(dropped, HEX);
            return false;
        }

        // Need start marker + length byte before we know payload size
        if (_serial.available() < 2) return false;

        _serial.read();               // consume 0xAA
        uint8_t len = (uint8_t)_serial.read();

        if (len != sizeof(MeshPacket)) {
            Serial.print(F("[UartBridge] Bad length byte: "));
            Serial.print(len);
            Serial.print(F(" (expected "));
            Serial.print((int)sizeof(MeshPacket));
            Serial.println(F(") — dropping frame"));
            return false;
        }

        // Wait for the full payload
        if (_serial.available() < (int)len) {
            // Partial frame this iteration — drop rather than buffer.
            // A production build should buffer partial frames across loop()
            // calls instead of dropping them.
            Serial.println(F("[UartBridge] Partial frame — dropping"));
            return false;
        }

        _serial.readBytes((char *)&out, len);
        Serial.print(F("[UartBridge] Received frame from Gateway  origin="));
        Serial.print(out.origin_id);
        Serial.print(F("  type="));
        Serial.print(out.msg_type);
        Serial.print(F("  sev="));
        Serial.println(out.severity);
        return true;
    }

private:
    HardwareSerial &_serial;
};
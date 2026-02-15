#pragma once
#include <Arduino.h>

// ============================================================
// Status LED (WS2812 on GPIO48)
// Non-blocking blink/pulse patterns for system state indication
//
// Usage:
//   statusled::begin();
//   statusled::setState(statusled::ST_BOOTING);
//   // In loop():
//   statusled::update();
// ============================================================

namespace statusled {

// --- LED States ---
enum State : uint8_t {
    ST_OFF,              // LED aus
    ST_BOOTING,          // Weiss, schnell blinken (100ms)
    ST_AP_ONLY,          // Gelb, langsam pulsieren (2s Zyklus)
    ST_WIFI_CONNECTING,  // Blau, schnell blinken (200ms)
    ST_WIFI_NO_NTP,      // Cyan, Dauerlicht
    ST_READY,            // Gruen, Dauerlicht
    ST_RELAY_ACTIVE,     // Gruen, langsam blinken (1s)
    ST_MCP_ERROR,        // Rot, schnell blinken (200ms)
    ST_CONFIG_ERROR,     // Rot, Dauerlicht
    ST_OTA_UPDATE,       // Magenta, schnell pulsieren (500ms)
    ST_WS_CLIENT,        // Gruen, kurzer Blitz (50ms alle 3s)
};

// Initialize WS2812 on GPIO48
void begin(uint8_t brightness = 20);

// Set current state (changes LED pattern)
void setState(State state);
State getState();

// Call in loop() - drives the blink/pulse patterns (non-blocking)
void update();

// Set brightness (0-255)
void setBrightness(uint8_t brightness);

} // namespace statusled

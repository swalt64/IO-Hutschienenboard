#pragma once
#include <stdint.h>

// ============================================================
// Pin Configuration for IO-Hutschienenboard
// ESP32-S3-WROOM-1 (DUBEUYEW)
// ============================================================
//
// Architecture:
//   12 AC Inputs  -> Optocoupler -> ESP32 GPIO (direct, fast edge detection)
//   12 Bistable Relays -> 2x MCP23017 via I2C
//
// MCP23017 address scheme:
//   0x20 (A2=0, A1=0, A0=0) : Relay SET coils
//   0x21 (A2=0, A1=0, A0=1) : Relay RESET coils
//   0x22 (A2=0, A1=1, A0=0) : Top board buttons and LEDs
//

// --- I2C Bus ---
static const uint8_t I2C_SDA_PIN = 11;
static const uint8_t I2C_SCL_PIN = 12;

// --- MCP23017 I2C addresses ---
static const uint8_t MCP_ADDR_SET      = 0x21;
static const uint8_t MCP_ADDR_RESET    = 0x20;
static const uint8_t MCP_ADDR_TOPBOARD = 0x22;

// --- Top Board interrupt ---
static const uint8_t MCP_INT_PIN = 13;

// --- Top Board buttons (active low) ---
static const uint8_t BTN_ENTER = 0;
static const uint8_t BTN_UP    = 1;
static const uint8_t BTN_DOWN  = 2;
static const uint8_t BTN_LEFT  = 3;
static const uint8_t BTN_RIGHT = 4;

// --- Top Board status LEDs (active low) ---
static const uint8_t LED_WLAN_PIN   =  8;
static const uint8_t LED_OUTPUT_PIN =  9;
static const uint8_t LED_RUN_PIN    = 10;

// --- 12 digital inputs ---
static const uint8_t INPUT_PINS[12] = {
    4,
    5,
    6,
    7,
    15,
    16,
    17,
    18,
    8,
    3,
    9,
    10,
};

// --- Relay pin mapping ---
static const uint8_t RELAY_PINS[12] = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
};

// MCP array indices
static const uint8_t MCP_SET   = 0;
static const uint8_t MCP_RESET = 1;

// Bistable relay pulse
static const uint16_t RELAY_PULSE_MS = 100;

// Number of channels
static const uint8_t NUM_CHANNELS = 12;

// Peripheral reset (active low, MCP23017 + OLED)
static const uint8_t RESET_PERIPHERIE_PIN = 2;

// OLED display
static const uint8_t OLED_ADDR       = 0x3C;
static const uint8_t CH_NAME_MAX_LEN = 20;

// Free ESP32 GPIOs:
// GPIO0, 1, 2, 14, 21, 35, 36, 37, 38, 39, 40, 41, 42, 45, 46, 47, 48

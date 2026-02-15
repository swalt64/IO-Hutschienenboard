#pragma once
#include <stdint.h>

// ============================================================
// Pin Configuration for IO-Hutschienenboard
// ESP32-S3-WROOM-1 (DUBEUYEW)
// ============================================================
//
// Architecture:
//   12 AC Inputs  -> Optocoupler -> ESP32 GPIO (direct, for fast edge detection)
//   12 Bistable Relays -> 2x MCP23017 via I2C (24 outputs: SET + RESET per relay)
//
// MCP23017 #1 (0x20): Relay 1-8 SET (GPA0-7) + Relay 1-8 RESET (GPB0-7)
// MCP23017 #2 (0x21): Relay 9-12 SET (GPA0-3) + Relay 9-12 RESET (GPB0-3)
//                      GPA4-7, GPB4-7 = 8 spare I/Os for future use
//
// ============================================================

// --- I2C Bus for MCP23017 ---
static const uint8_t I2C_SDA_PIN = 11;
static const uint8_t I2C_SCL_PIN = 12;

// --- MCP23017 I2C Addresses ---
static const uint8_t MCP_ADDR_1 = 0x20;  // Relay 1-8
static const uint8_t MCP_ADDR_2 = 0x21;  // Relay 9-12

// --- 12 Digital Inputs (from optocoupler outputs, directly on ESP32) ---
static const uint8_t INPUT_PINS[12] = {
    4,   // GPIO4  - Input 1
    5,   // GPIO5  - Input 2
    6,   // GPIO6  - Input 3
    7,   // GPIO7  - Input 4
    15,  // GPIO15 - Input 5
    16,  // GPIO16 - Input 6
    17,  // GPIO17 - Input 7
    18,  // GPIO18 - Input 8
    8,   // GPIO8  - Input 9
    3,   // GPIO3  - Input 10
    9,   // GPIO9  - Input 11
    10,  // GPIO10 - Input 12
};

// --- Relay Pin Mapping on MCP23017 ---
// Each relay has a SET pin and RESET pin on the MCP23017
// Format: {mcp_index (0 or 1), set_pin (0-15), reset_pin (0-15)}
struct RelayPinDef {
    uint8_t mcpIndex;   // 0 = MCP_ADDR_1, 1 = MCP_ADDR_2
    uint8_t setPin;     // MCP23017 pin number (0-15, 0-7=GPA, 8-15=GPB)
    uint8_t resetPin;   // MCP23017 pin number
};

static const RelayPinDef RELAY_PINS[12] = {
    // MCP23017 #1 (0x20): Relays 1-8
    {0,  0,  8},  // Relay 1:  SET=GPA0, RESET=GPB0
    {0,  1,  9},  // Relay 2:  SET=GPA1, RESET=GPB1
    {0,  2, 10},  // Relay 3:  SET=GPA2, RESET=GPB2
    {0,  3, 11},  // Relay 4:  SET=GPA3, RESET=GPB3
    {0,  4, 12},  // Relay 5:  SET=GPA4, RESET=GPB4
    {0,  5, 13},  // Relay 6:  SET=GPA5, RESET=GPB5
    {0,  6, 14},  // Relay 7:  SET=GPA6, RESET=GPB6
    {0,  7, 15},  // Relay 8:  SET=GPA7, RESET=GPB7
    // MCP23017 #2 (0x21): Relays 9-12
    {1,  0,  8},  // Relay 9:  SET=GPA0, RESET=GPB0
    {1,  1,  9},  // Relay 10: SET=GPA1, RESET=GPB1
    {1,  2, 10},  // Relay 11: SET=GPA2, RESET=GPB2
    {1,  3, 11},  // Relay 12: SET=GPA3, RESET=GPB3
};

// Bistable relay pulse duration in milliseconds
static const uint16_t RELAY_PULSE_MS = 50;

// Number of channels
static const uint8_t NUM_CHANNELS = 12;

// --- Free ESP32 GPIOs (not used, available for future expansion) ---
// GPIO0, 1, 2, 13, 14, 21, 35, 36, 37, 38, 39, 40, 41, 42, 45, 46, 47, 48
// = 18 spare pins!

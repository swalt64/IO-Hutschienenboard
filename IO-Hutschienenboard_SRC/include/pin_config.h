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
// MCP23017 Adress-Schema:
//   0x20 (A2=0, A1=0, A0=0) : Relay SET  coils  — alle 12 SET-Spulen
//   0x21 (A2=0, A1=0, A0=1) : Relay RESET coils — alle 12 RESET-Spulen
//   0x22 (A2=0, A1=1, A0=0) : Top Board LEDs    (Top Board PCB: A1=HIGH)
//   0x23 (A2=0, A1=1, A0=1) : Top Board Buttons (Top Board PCB: A1=HIGH, A0=HIGH)
//
// Relay Pin Mapping (identisch auf SET-MCP und RESET-MCP):
//   GPA0-7 : Relais  1-8  (pin  0-7)
//   GPB0-3 : Relais  9-12 (pin  8-11)
//   GPB4-7 : frei
//
// Relay Typ: Panasonic DSP1A-L2-DC12V (bistabil, 2 Spulen)
//   SET-Spule:   Pin 15(+) -> 12V, Pin 16(-) -> ULN2803A -> MCP SET-GPA/GPB
//   RESET-Spule: Pin  2(+) -> 12V, Pin  1(-) -> ULN2803A -> MCP RESET-GPA/GPB
//   Kontakt NO:  Pin 5 | COM: Pin 8
//   Spulenwiderstand: 480Ω | Strom: 25mA | Puls: min. 10ms
//
// ============================================================

// --- I2C Bus ---
static const uint8_t I2C_SDA_PIN = 11;
static const uint8_t I2C_SCL_PIN = 12;

// --- MCP23017 I2C Adressen ---
static const uint8_t MCP_ADDR_SET   = 0x20;  // SET-Spulen  (A2=0, A1=0, A0=0)
static const uint8_t MCP_ADDR_RESET = 0x21;  // RESET-Spulen(A2=0, A1=0, A0=1)
// Top Board (Info, nicht in Firmware genutzt):
// static const uint8_t MCP_ADDR_LED = 0x22;  // Top Board LEDs    (A1=HIGH)
// static const uint8_t MCP_ADDR_BTN = 0x23;  // Top Board Buttons (A1=HIGH, A0=HIGH)

// --- 12 Digital Inputs (Optokoppler -> ESP32 GPIO) ---
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

// --- Relay Pin Mapping ---
// SET-Spule  -> mcp[MCP_SET]  (0x20), Pin = relayPin
// RESET-Spule-> mcp[MCP_RESET](0x21), Pin = relayPin (gleiche Pinnummer)
static const uint8_t RELAY_PINS[12] = {
    0,   // Relay  1: GPA0
    1,   // Relay  2: GPA1
    2,   // Relay  3: GPA2
    3,   // Relay  4: GPA3
    4,   // Relay  5: GPA4
    5,   // Relay  6: GPA5
    6,   // Relay  7: GPA6
    7,   // Relay  8: GPA7
    8,   // Relay  9: GPB0
    9,   // Relay 10: GPB1
    10,  // Relay 11: GPB2
    11,  // Relay 12: GPB3
};

// MCP Array-Indices
static const uint8_t MCP_SET   = 0;  // mcp[0] = SET  MCP (0x20)
static const uint8_t MCP_RESET = 1;  // mcp[1] = RESET MCP (0x21)

// Bistabiler Relais-Impuls
static const uint16_t RELAY_PULSE_MS = 15;  // 15ms > 10ms Min. laut Datenblatt

// Anzahl Kanäle
static const uint8_t NUM_CHANNELS = 12;

// --- Freie ESP32 GPIOs ---
// GPIO0, 1, 2, 13, 14, 21, 35, 36, 37, 38, 39, 40, 41, 42, 45, 46, 47, 48

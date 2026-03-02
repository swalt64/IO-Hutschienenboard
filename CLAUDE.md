# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 based 12-channel I/O DIN-rail controller (Hutschienenboard) with web UI. Hardware consists of a base board (inputs, relays, ESP32) and a top board (LEDs, buttons).

- 12 galvanically isolated S0 inputs (optocoupler ILD213T, supports DC dry contacts and 8VAC detection)
- 12 monostable relay outputs (via MCP23017 + ULN2803A driver)
- 12 status LEDs + 12 buttons + 1 reset button (top board, via MCP23017)
- ESP32-S3 powered directly via 3V3 (5V pin unused)

## Build Commands

All commands run from `IO-Hutschienenboard_SRC/`:

```bash
# Build firmware
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" run

# Upload firmware
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" run -t upload

# Upload web UI (LittleFS filesystem)
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" run -t uploadfs

# Serial monitor (COM3, 115200 baud, raw mode)
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" device monitor -e esp32s3
```

Build system: PlatformIO with Arduino framework, targeting ESP32-S3 (custom board profile `esp32-s3-devkitc-1-n16r8`, 16MB Flash QIO, 8MB PSRAM OPI).

## Architecture

### Source Layout

- `IO-Hutschienenboard_SRC/` — PlatformIO project root
- `src/main.cpp` — Core application: WiFi, AsyncWebServer, WebSocket, MCP23017 relay control, input edge detection, auto-off timers, NVS config persistence
- `src/statusled.cpp` — Non-blocking WS2812 RGB LED patterns reflecting system state
- `src/swtools.cpp` — Debug logging with categories (SYSTEM, WIFI, NTP, MCP, RELAY, INPUT, WEB, CONFIG, TIMER), ANSI colors, NTP timestamps
- `include/pin_config.h` — GPIO pin mapping, MCP23017 addresses, relay-to-pin tables
- `data/index.html` — Single-page web UI (vanilla JS, WebSocket, dark theme)
- `boards/` — Custom PlatformIO board definition JSON

### Key Design Patterns

- **Non-blocking loop:** All timing via `millis()`, no `delay()` in `loop()`
- **Namespace isolation:** Modules use `dbg::`, `statusled::` namespaces
- **Dual WiFi:** Simultaneous AP (192.168.50.1, SSID "IO-Hutschiene") + optional STA client
- **WebSocket protocol:** Bidirectional JSON — commands: `toggle`, `set`, `map`, `timer`, `wifi`, `alloff`; server pushes full state as JSON arrays
- **Bistable relays:** Panasonic DSP1A-L2-DC12V, 2 Spulen — 15ms SET-Impuls (MCP 0x20) oder RESET-Impuls (MCP 0x21)
- **S0 input detection:** Rising-edge polling in loop() triggers mapped relay toggle; supports both DC (constant HIGH) and AC (50Hz square wave detection via `isInputActive()`)
- **Top board buttons:** MCP23017 #3 with interrupt-on-change, INTA+INTB mirrored (open-drain) to ESP32
- **Configuration persistence:** NVS via Preferences API, namespace "io-config" (WiFi creds, input-output mappings, auto-off timers)
- **Hardware simulation:** Build flag `SIMULATE_HW=1` (currently enabled in platformio.ini) allows testing without physical MCP23017 hardware

### Hardware Pin Summary

**ESP32-S3 GPIO Allocation (verified from netlist):**
- S0 inputs (12×, accent from SN74HCT14DR outputs):
  - U200: GPIO4(Inp1), GPIO5(Inp2), GPIO6(Inp3), GPIO7(Inp4), GPIO15(Inp5), GPIO16(Inp6)
  - U201: GPIO17(Inp7), GPIO18(Inp8), GPIO8(Inp9), GPIO3(Inp10), GPIO9(Inp11), GPIO10(Inp12)
- I2C bus: SDA=GPIO11, SCL=GPIO12 (R104/R105 4k7 pull-ups to 3V3p)
- MCP23017 reset (active-low): GPIO2 (RESET_PERIPHERIE) → R302 (1k0) → I2C_RESET net → U300 pin 18
- Top board connector (J202): GPIO13, GPIO14, GPIO21, GPIO38 (via R248-R251 3k3 series resistors)
- Hardware reset: EN/RST pin with R103 (4k7) pull-up to 3V3p, S100 (430182050816) reset button to GND
- USB (internal): GPIO19/20
- Reserved (not available): GPIO26-32 (flash), GPIO33-37 (octal PSRAM)
- Free GPIOs: 0, 1, 39, 40, 41, 42, 45, 46, 47, 48

**Power Supply:**
- 12V → U101 (AP62200WU-7) buck converter → L102 (ASPI-4020S-3R3M-T) → L100 ferrite → 3V3p
- 12V → U102 (SL4U-1205) isolated DC/DC → L103 ferrite → I-5Vp (S0 input optocouplers)
- I-GND galvanically isolated from main GND, coupled only via C110 (4700pF/1000V)
- 12V directly to relay coils (K300-K311 DSP1a-DC12V) and ULN2803A COM pins

**I2C Bus — MCP23017 Adress-Schema:**
- 0x20 (A2=0,A1=0,A0=0): SET-Spulen  — GPA0-7=K1-8 SET,  GPB0-3=K9-12 SET
- 0x21 (A2=0,A1=0,A0=1): RESET-Spulen — GPA0-7=K1-8 RESET, GPB0-3=K9-12 RESET
- 0x22 (A2=0,A1=1,A0=0): Top Board LEDs    (via J202, A1=HIGH)
- 0x23 (A2=0,A1=1,A0=1): Top Board Buttons (via J202, A1=HIGH, A0=HIGH)
- ULN2803A outputs → ferrite beads (L300-L311, 742792651) → relay coils (DSP1A-L2-DC12V)
- Relay Typ: Panasonic DSP1A-L2-DC12V, SET-Pin 15(+)/16(-), RESET-Pin 2(+)/1(-), 480Ω, 25mA

**Connectors:**
- J200 (MCV 1,5/9-G-3,5): S0 inputs 1-8 (pins 1-8), I-GND (pin 9)
- J201 (MCV 1,5/9-G-3,5): S0 inputs 9-12 (pins 1-4), 12V power (pins 5-6), spare (pins 7-8), I-GND (pin 9)
- J202 (MCV 1,5/9-G-3,5): I2C SDA (pin 1), SCL (pin 2), GND (pin 3), RESET (pin 4), GPIO13 (pin 5), GPIO14 (pin 6), GPIO21 (pin 7), GPIO38 (pin 8), GND (pin 9)
- J300/J301/J302 (MSTBVA 2,5/8-G): relay contacts K1-K12 (Schließer/Öffner pairs)

**S0 Input Circuit (per channel, verified from netlist):**
- 6× ILD213T (V200-V205), each provides 2 S0 channels
- Input path: I-5V → R (3k3) → L (B82422 330µH) → D (BAS316, series) → J200/J201 → external S0 → I-GND
- LED2 cathode path: I-5V → ILD213T LED anode → LED cathode → R (560Ω) → same node as input path
- SMBJ20A-TR antiparallel to each LED for reverse voltage protection
- Output: emitter follower — 3V3p → collector, emitter → R (1k0) to GND + R (3k3) + C (470n) RC filter → SN74HCT14DR Schmitt inverter
- 2× SN74HCT14DR (U200, U201) powered from **3V3p** (not 5V)

### Key Libraries

- `ESP Async WebServer` (mathieucarbou) — async HTTP + WebSocket
- `ArduinoJson` — JSON serialization for WebSocket messages and REST API (`/api/state`)
- `Adafruit MCP23017` — I2C I/O expander for relay control
- `Freenove WS2812 Lib` — RGB status LED

## Altium Designer Workflows

### PCB Component Placement — Move Component to Cursor

**Goal:** Place a component at the exact cursor position without the view jumping to the component.

**Workflow:**
1. In PCB editor, position cursor where you want to place the component
2. Press `M` (Move) → a selection dialog appears
3. Type or filter the component designator (e.g., `R100`, `U1`)
4. Dialog shows matching components
5. Select the component → click **"Move Component to Cursor"**
6. Component jumps to cursor position (view stays centered)
7. Click to confirm placement, press `Space` to rotate if needed

**Keyboard:** `M` → type designator → Tab to navigate → Enter to confirm option

### ESP32-S3 Symbol Verification

**ESP32-S3-WROOM-1 pinout (DUBEUYEW variant, 44-pin):**

Critical pins for this project:
- GPIO1–GPIO10: ADC1 channels (Ch0–Ch9)
- GPIO11–GPIO20: ADC2 channels (Ch0–Ch9)
- GPIO19: USB_D− (not USB_D+!)
- GPIO20: USB_D+ (not USB_D−!)
- GPIO47/GPIO48: RGB LED (WS2812) clock lines
- GPIO11/GPIO12: I2C bus (SDA/SCL to MCP23017)

**Common symbol errors to watch for:** ADC channel mismatches, swapped USB D±, JTAG pin order (GPIO39–42 = MTCK/MTDO/MTDI/MTMS).

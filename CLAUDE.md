# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 based 12-channel I/O DIN-rail controller (Hutschienenboard) with web UI. Hardware consists of a base board (inputs, relays, ESP32) and a top board (OLED display, LEDs, buttons).

- 12 galvanically isolated S0 inputs (optocoupler ILD213T, supports DC dry contacts and 8VAC detection)
- 12 bistable relay outputs (Panasonic DSP1A-L2-DC12V, via 2× MCP23017 + ULN2803A driver)
- OLED display 128×64 (JMD0.96-1, SSD1306, I2C 0x3C) auf dem Top Board
- 5 Navigationstasten (ENTER, UP, DOWN, LEFT, RIGHT) via MCP23017 GPA auf dem Top Board
- 3 Status-LEDs (WLAN, OUTPUT, RUN) via MCP23017 GPB auf dem Top Board
- ESP32-S3 powered directly via 3V3 (5V pin unused)

## Build Commands

All commands run from `IO-Hutschienenboard_SRC/`:

```bash
# Build firmware
"/c/Users/stwal/.platformio/penv/Scripts/platformio.exe" run

# Upload firmware
"/c/Users/stwal/.platformio/penv/Scripts/platformio.exe" run -t upload

# Upload web UI (LittleFS filesystem)
"/c/Users/stwal/.platformio/penv/Scripts/platformio.exe" run -t uploadfs

# Serial monitor (COM3, 115200 baud, raw mode)
"/c/Users/stwal/.platformio/penv/Scripts/platformio.exe" device monitor -e esp32s3
```

Build system: PlatformIO with Arduino framework, targeting ESP32-S3 (custom board profile `esp32-s3-devkitc-1-n16r8`, 16MB Flash QIO, 8MB PSRAM OPI).

## Architecture

### Source Layout

- `IO-Hutschienenboard_SRC/` — PlatformIO project root
- `src/main.cpp` — Core application: WiFi, AsyncWebServer, WebSocket, MCP23017 relay control, input edge detection, auto-off timers, channel names, button navigation, NVS config persistence
- `src/display.h` / `src/display.cpp` — OLED display via Adafruit SSD1306 (128×64); namespace `display::`: `begin()`, `showBoot()`, `show()`, `address()`, `controller()`
- `src/statusled.cpp` — Non-blocking WS2812 RGB LED patterns reflecting system state
- `src/swtools.cpp` — Debug logging with categories (SYSTEM, WIFI, NTP, MCP, RELAY, INPUT, WEB, CONFIG, TIMER), ANSI colors, NTP timestamps
- `include/pin_config.h` — GPIO pin mapping, MCP23017 addresses, relay-to-pin tables, `OLED_ADDR` (0x3C), `RESET_PERIPHERIE_PIN` (GPIO2), `CH_NAME_MAX_LEN`
- `data/index.html` — Single-page web UI (vanilla JS, WebSocket, dark theme); Kanalnamen editierbar
- `boards/` — Custom PlatformIO board definition JSON

### Key Design Patterns

- **Non-blocking loop:** All timing via `millis()`, no `delay()` in `loop()`
- **Namespace isolation:** Modules use `dbg::`, `statusled::`, `display::` namespaces
- **Dual WiFi:** Simultaneous AP (192.168.50.1, SSID "IO-Hutschiene") + optional STA client
- **WebSocket protocol:** Bidirectional JSON — commands: `toggle`, `set`, `map`, `timer`, `wifi`, `alloff`, `name`; server pushes full state as JSON arrays (incl. `names`)
- **Bistable relays:** Panasonic DSP1A-L2-DC12V, 2 Spulen — 15ms SET-Impuls (MCP 0x20) oder RESET-Impuls (MCP 0x21)
- **S0 input detection:** Rising-edge polling in loop() triggers mapped relay toggle; supports both DC (constant HIGH) and AC (50Hz square wave detection)
- **Top board buttons:** MCP23017 (0x22) GPA0-4, interrupt-on-change via INTA → GPIO13; Entprellung 50ms
- **Button navigation:** UP/DOWN scrollt Kanäle 1-12; ENTER togglet Relais; LEFT/RIGHT ändert Restlaufzeit ±1 min (temporär, kein NVS); LEFT bei rem≤60s → Relais AUS
- **Display:** `displayChannel` (0-11) bestimmt welcher Kanal angezeigt wird; `updateDisplay()` wird von `setRelay()`, Button-Aktionen und loop() (1s-Takt) aufgerufen
- **Temporäre Zeitkorrektur:** `tempTimeAdjustMs[12]` (int32_t) addiert zu autoOffSeconds×1000; wird bei jedem `setRelay()`-Aufruf auf 0 zurückgesetzt; nicht in NVS gespeichert
- **Configuration persistence:** NVS via Preferences API, namespace "io-config"
  - Keys: `ssid`, `pass`, `map0`–`map11`, `auto0`–`auto11`, `name0`–`name11`
  - Defaults: Kanalnamen "Ausgang 1"–"Ausgang 12"
- **Hardware simulation:** Build flag `SIMULATE_HW=1` (currently enabled in platformio.ini) allows testing without physical MCP23017/OLED hardware; Wire wird trotzdem initialisiert (inkl. I2C-Bus-Recovery) damit das OLED auch im Sim-Modus funktioniert
- **OLED Hardtest:** Build flag `OLED_HARDTEST=1` — Setup initialisiert nur I2C + Wire und sendet raw SSD1306-Befehle (alle Pixel AN); loop() ist inaktiv; nützlich zum Verifizieren der Adresse ohne Firmware-Stack
- **I2C Bus Recovery:** `i2cBusRecover()` sendet 9 SCL-Pulse + STOP-Condition vor `Wire.begin()` um SDA-stuck nach Soft-Reset zu beheben; `Wire.setTimeOut(10)` verhindert 145s Blockierung falls Gerät nicht antwortet
- **Peripheral Reset:** GPIO2 (`RESET_PERIPHERIE_PIN`) treibt RESET_PERIPHERIE-Netz (aktiv-low, R302 1kΩ); wird in setup() auf HIGH gesetzt bevor I2C-Init

### Display Layout (128×64 px)

```
y= 9  [6x10]   "01/12  Pumpe Keller"   ← Kanal-Nr. + Name (max 13 Zeichen)
y=12           ──────────────────────  ← Trennlinie
y=33  [10x20]  "EIN" oder "AUS"        ← Status, zentriert (x=49)
y=49  [6x10]   "Noch: 15:23"           ← Restzeit | "Dauerbetrieb" | "--"
y=62  [5x7]    "^v=Ch OK=Tog <>= 1m"  ← Hint
```

Fonts: `u8g2_font_6x10_tf` (Zeile 1, 3), `u8g2_font_10x20_tf` (Status), `u8g2_font_5x7_tf` (Hint)
Remaining-Format: `MM:SS` (<1h) oder `HH:MM:SS` (≥1h)

### Hardware Pin Summary

**ESP32-S3 GPIO Allocation (verified from netlist):**
- S0 inputs (12×, output of SN74HCT14DR Schmitt inverters):
  - U200: GPIO4(Inp1), GPIO5(Inp2), GPIO6(Inp3), GPIO7(Inp4), GPIO15(Inp5), GPIO16(Inp6)
  - U201: GPIO17(Inp7), GPIO18(Inp8), GPIO8(Inp9), GPIO3(Inp10), GPIO9(Inp11), GPIO10(Inp12)
- I2C bus: SDA=GPIO11, SCL=GPIO12 (R104/R105 4k7 pull-ups to 3V3p)
- Peripheral reset (active-low): GPIO2 (`RESET_PERIPHERIE_PIN`) → R302 (1k0) → RESET_PERIPHERIE-Netz → MCP23017 pin 18; in setup() auf HIGH gesetzt
- Top board connector (J202): GPIO13 (MCP_INT_PIN/INTA), GPIO14, GPIO21, GPIO38 (via R248-R251 3k3)
- Hardware reset: EN/RST pin with R103 (4k7) pull-up, S100 reset button to GND
- USB (internal): GPIO19 (USB_D−), GPIO20 (USB_D+)
- Reserved: GPIO26-32 (flash), GPIO33-37 (octal PSRAM)
- Free GPIOs: 0, 1, 39, 40, 41, 42, 45, 46, 47, 48

**Power Supply:**
- 12V → U101 (AP62200WU-7) buck converter → L102 (ASPI-4020S-3R3M-T) → L100 ferrite → 3V3p
- 12V → U102 (SL4U-1205) isolated DC/DC → L103 ferrite → I-5Vp (S0 input optocouplers)
- I-GND galvanically isolated from main GND, coupled only via C110 (4700pF/1000V)
- 12V directly to relay coils (K300-K311) and ULN2803A COM pins

**I2C Bus — Adress-Schema:**
- 0x20 (A2=0,A1=0,A0=0): Relay SET-Spulen  — GPA0-7=K1-8 SET,  GPB0-3=K9-12 SET
- 0x21 (A2=0,A1=0,A0=1): Relay RESET-Spulen — GPA0-7=K1-8 RESET, GPB0-3=K9-12 RESET
- 0x22 (A2=0,A1=1,A0=0): Top Board MCP — GPA0-4 Taster (aktiv-low, INPUT_PULLUP), GPB0-2 LEDs (aktiv-low, OUTPUT)
- 0x3C: OLED SSD1306 128×64 (JMD0.96-1, SA0=GND)

**Top Board MCP23017 (0x22) Pin-Belegung:**
- GPA0 = BTN_ENTER, GPA1 = BTN_UP, GPA2 = BTN_DOWN, GPA3 = BTN_LEFT, GPA4 = BTN_RIGHT
- GPA5-7 = frei (INPUT_PULLUP)
- GPB0 = LED_WLAN (rot), GPB1 = LED_OUTPUT (gelb), GPB2 = LED_RUN (grün)
- GPB3-7 = frei (OUTPUT HIGH)
- INTA → GPIO13 (aktiv-low, Push-Pull)

**Relay Hardware:**
- Typ: Panasonic DSP1A-L2-DC12V (bistabil, 2 Spulen)
- SET-Spule: Pin 15(+)/16(-), RESET-Spule: Pin 2(+)/1(-)
- Spulenwiderstand: 480Ω, Strom: 25mA, Mindestpuls: 10ms (Firmware: 15ms)
- ULN2803A outputs → Ferrit-Perlen (L300-L311, 742792651) → Relay-Spulen

**Connectors:**
- J200 (MCV 1,5/9-G-3,5): S0 inputs 1-8 (pins 1-8), I-GND (pin 9)
- J201 (MCV 1,5/9-G-3,5): S0 inputs 9-12 (pins 1-4), 12V power (pins 5-6), spare (pins 7-8), I-GND (pin 9)
- J202 (MCV 1,5/9-G-3,5): I2C SDA (pin 1), SCL (pin 2), GND (pin 3), RESET (pin 4), GPIO13 (pin 5), GPIO14 (pin 6), GPIO21 (pin 7), GPIO38 (pin 8), GND (pin 9)
- J300/J301/J302 (MSTBVA 2,5/8-G): relay contacts K1-K12 (Schließer/Öffner pairs)

**S0 Input Circuit (per channel, verified from netlist):**
- 6× ILD213T (V200-V205), each provides 2 S0 channels
- Input path: I-5V → R (3k3) → L (B82422 330µH) → D (BAS316) → J200/J201 → external S0 → I-GND
- SMBJ20A-TR antiparallel to each LED for reverse voltage protection
- Output: emitter follower — 3V3p → collector, emitter → R (1k0) to GND + R (3k3) + C (470n) RC filter → SN74HCT14DR Schmitt inverter
- 2× SN74HCT14DR (U200, U201) powered from **3V3p** (not 5V)

### Key Libraries

- `ESP Async WebServer` (mathieucarbou) — async HTTP + WebSocket
- `ArduinoJson` — JSON serialization for WebSocket messages and REST API (`/api/state`)
- `Adafruit MCP23017` — I2C I/O expander for relay control and top board
- `Freenove WS2812 Lib` — RGB status LED (WS2812 on ESP32-S3)
- `Adafruit SSD1306` + `Adafruit GFX Library` — OLED display driver, SSD1306 128×64, HW I2C; `begin(SSD1306_SWITCHCAPVCC, addr)` mit internem Charge-Pump

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
- GPIO11/GPIO12: I2C bus (SDA/SCL)

**Common symbol errors to watch for:** ADC channel mismatches, swapped USB D±, JTAG pin order (GPIO39–42 = MTCK/MTDO/MTDI/MTMS).

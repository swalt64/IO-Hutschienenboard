# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 based 12-channel I/O DIN-rail controller (Hutschienenboard) with web UI. Hardware consists of a base board (inputs, relays, ESP32) and a top board (OLED display, LEDs, buttons).

- 12 galvanically isolated S0 inputs (optocoupler ILD213T, supports DC dry contacts and 8VAC detection)
- 12 bistable relay outputs (Panasonic DSP1A-L2-DC12V, via 2× MCP23017 + ULN2803A driver)
- OLED display 128×64 (JMD0.96-1, SSD1306, I2C 0x3C) auf dem Top Board
- 5 Navigationstasten (ENTER, UP, DOWN, LEFT, RIGHT) via MCP23017 GPA auf dem Top Board
- 3 Status-LEDs (WLAN, OUTPUT, RUN) via MCP23017 GPB auf dem Top Board
- ESP32-S3 (DUBEUYEW ESP32-S3, basierend auf ESP32-S3-WROOM-1-N16R8, 44-pin DIL Through-Hole-Modul) powered directly via 3V3 (5V pin unused)

## Build Commands

All commands run from `IO-Hutschienenboard_SRC/`:

```bash
# Build firmware
"/c/Users/stwal/.platformio/penv/Scripts/platformio.exe" run

# Upload firmware
"/c/Users/stwal/.platformio/penv/Scripts/platformio.exe" run -t upload

# Upload web UI (LittleFS filesystem)
"/c/Users/stwal/.platformio/penv/Scripts/platformio.exe" run -t uploadfs

# OTA-Upload über WLAN (ArduinoOTA, Ziel-IP = upload_port in platformio.ini)
"/c/Users/stwal/.platformio/penv/Scripts/platformio.exe" run -e esp32s3-ota -t upload

# Serial monitor (COM10, 115200 baud, raw mode)
"/c/Users/stwal/.platformio/penv/Scripts/platformio.exe" device monitor -e esp32s3
```

Build system: PlatformIO with Arduino framework, targeting ESP32-S3 (custom board profile `esp32-s3-devkitc-1-n16r8`, 16MB Flash QIO, 8MB PSRAM OPI).

**Plattform ist bewusst auf `espressif32@6.12.0` gepinnt** (Arduino-Core 2.x). Den Pin nicht entfernen: mit espressif32 55.x (Core 3.x) bricht der Build an zwei Stellen — `Freenove WS2812 1.x` nutzt die entfernte RMT-API (`rmtSetTick`/`rmt_send`, behoben in Lib-Version 2.x), und `dhcp_search_ip_on_mac()` in `main.cpp` hat in IDF 5.x einen zusätzlichen ersten Parameter `dhcps_t*` (Ersatz: `esp_netif_dhcps_get_clients_by_mac()`). Ein Core-3.x-Umstieg ist eine eigene Aufgabe inkl. Hardware-Nachtest.

Environments: `esp32s3` (USB, COM10) und `esp32s3-ota` (espota). Beide bumpen via `scripts/bump_version.py` die Version in `include/version.h` vor jedem Upload; `esp32s3-ota` liest zusätzlich das OTA-Passwort aus `ota_pass.txt` (gitignored, Fallback `admin`).

## Architecture

### Source Layout

- `IO-Hutschienenboard_SRC/` — PlatformIO project root
- `src/main.cpp` — Core application: WiFi, AsyncWebServer, WebSocket, MCP23017 relay control, input edge detection, auto-off timers, channel names, button navigation, NVS config persistence
- `src/display.h` / `src/display.cpp` — OLED display via Adafruit SSD1306 (128×64); namespace `display::`: `begin()`, `showBoot()`, `show()`, `address()`, `controller()`
- `src/statusled.cpp` — Non-blocking WS2812 RGB LED patterns reflecting system state
- `src/swtools.cpp` — Debug logging with categories (SYSTEM, WIFI, NTP, MCP, RELAY, INPUT, WEB, CONFIG, TIMER), ANSI colors, NTP timestamps
- `include/pin_config.h` — GPIO pin mapping, MCP23017 addresses, relay-to-pin tables, `OLED_ADDR` (0x3C), `RESET_PERIPHERIE_PIN` (GPIO2), `CH_NAME_MAX_LEN` (60), `RELAY_PULSE_MS` (20)
- `include/version.h` — `FW_VERSION`, automatisch von `scripts/bump_version.py` hochgezählt
- `data/` — LittleFS-Web-Assets: `index.html` (Bedienung), `config.html` (Konfiguration), `mapping.html` (Eingang→Ausgang), `status.html` (Systemstatus), `manifest.webmanifest` + `sw.js` (PWA)
- `scripts/` — Build-Hooks: `bump_version.py`, `read_ota_pass.py`
- `boards/` — Custom PlatformIO board definition JSON

### Key Design Patterns

- **Non-blocking loop:** All timing via `millis()`, no `delay()` in `loop()`
- **Namespace isolation:** Modules use `dbg::`, `statusled::`, `display::` namespaces
- **Dual WiFi:** Simultaneous AP (192.168.50.1, SSID "IO-Hutschiene") + optional STA client
- **AP-Auswahl im STA-Modus:** Vor `WiFi.begin()` **müssen** `WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN)` und `WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL)` gesetzt sein. Ohne sie nutzt der ESP32 `WIFI_FAST_SCAN` und verbindet sich mit dem *ersten* gefundenen AP der SSID statt mit dem stärksten — im Zielnetz existieren mehrere APs mit gleicher SSID. Zusammen mit `WiFi.persistent(false)` (kein BSSID-Cache) führte das dazu, dass das Board nach jedem Neustart zufällig auf einem entfernten AP landete (gemessen −70 dBm statt −10 dBm). Nicht entfernen.
- **WebSocket protocol:** Bidirectional JSON — commands: `toggle`, `set`, `map`, `timer`, `wifi`, `alloff`, `name`, `iname`, `out_mode`, `in_mode`, `set_out_order`, `set_out_enabled`, `set_out_protected`, `set_out_alloff`, `set_web_creds`, `set_ota_pass`, `load_config`, `reset_io`; server pushes full state as JSON arrays (`inputs`, `outputs`, `mappings`, `timers`, `remaining`, `names`, `input_names`, `last_input`, `last_output`, `out_modes`, `in_modes`, `out_order`, `out_enabled`, `out_protected`, `out_alloff`, `mcp`) plus `time`, `ntp`, `wifi_rssi`
- **REST-API:** `GET /api/state` (Gesamtzustand), `GET /api/cmd?cmd=toggle|set|alloff&ch=…&val=…&armed=…`
- **WLAN-Diagnosefelder:** `wifi_rssi` (dBm), `wifi_bssid`, `wifi_channel` — in beiden State-Buildern, nur wenn STA verbunden. Prozentformel überall identisch: `pct = clamp(2 × (RSSI + 100), 0, 100)` → 100 % ab −50 dBm, 50 % bei −75 dBm. Beim Verbindungsaufbau wird dieselbe Info auch geloggt (`CAT_WIFI`).
- **Achtung — zwei getrennte State-Builder:** `buildStateJson()` (~main.cpp:498, für WebSocket) und der `/api/state`-Handler (~main.cpp:1144) bauen ihr JSON unabhängig voneinander und sind auseinandergelaufen. Nur der WebSocket liefert `last_input`, `last_output`, `mcp`; nur `/api/state` liefert `ap_ip`, `sta_ip`, `sta_ssid`, `sta_pass`, `version`, `mcp1`, `mcp2`. **Neue Felder immer in beiden ergänzen** — oder besser den Handler auf `buildStateJson()` umstellen.
- **Authentifizierung:** HTTP Basic Auth auf `/api/state`, `/api/cmd`, allen statischen Dateien, dem WebSocket und ElegantOTA; Credentials `ui_user`/`ui_pass` (Default `admin`/`admin`), zur Laufzeit über `set_web_creds` änderbar → `applyAuth()` setzt sie auf allen Handlern neu
- **OTA:** ElegantOTA über die Web-UI (`/update`, `setAutoReboot(true)`) **und** ArduinoOTA (Hostname `HS-IO`, Port 3232, Passwort `ota_pass`)
- **mDNS:** `MDNS.begin("HS-IO")` → `hs-io.local`, HTTP-Service auf Port 80
- **Bistable relays:** Panasonic DSP1A-L2-DC12V, 2 Spulen — `RELAY_PULSE_MS` = **20 ms**; **SET = MCP 0x21 (`MCP_ADDR_SET`), RESET = MCP 0x20 (`MCP_ADDR_RESET`)**
- **S0 input detection:** Flankenerkennung in loop(); unterstützt DC (konstant HIGH) und AC (50Hz-Rechteck). Betriebsart je Eingang über `inputMode[]` (0=Toggle, 1=Taster); im Toggle-Modus löst zusätzlich ein Long-Press (`longPressFired[]`) aus
- **Input mapping ist eine Bitmaske:** `inputMapping[i]` ist `uint16_t` — ein Eingang kann mehrere Ausgänge schalten (NVS-Key `mm<i>`, **nicht** `map<i>`)
- **Ausgangs-Optionen je Kanal:** `outputMode[]` (0=Toggle, 1=Taster), `outChEnabled[]` (Kanal freigegeben), `outDisplayOrder[]` (Anzeigereihenfolge), `outRemoteProtected[]` (Fernsteuer-Schutz — `isRemoteProtected()` blockt `toggle`/`set` ohne `armed`-Flag), `outAllOffEnabled[]` (bei „Alles aus" mit ausschalten)
- **„Alles aus" zentral in `allOutputsOff(reason)`:** einziger Ort, an dem mehrere Ausgänge gemeinsam abgeschaltet werden; überspringt Kanäle mit `outAllOffEnabled[i] == false` (z. B. Klimaanlage). Alle Auslöser laufen darüber: Eingangs-Langdruck (2 s, nur im Eingangs-Togglemodus) und `pendingAllOff` aus WebSocket-`alloff` / REST-`alloff` / App-Langdruck. **Ausnahme bewusst nicht angewendet:** `reset_io` (Werkseinstellung) schaltet weiterhin *alle* Relais ab. Neue Sammel-Abschaltungen immer über `allOutputsOff()` führen, nicht per eigener Schleife.
- **Top board buttons:** MCP23017 (0x22) GPA0-4, interrupt-on-change via INTA → GPIO13; Entprellung 50ms; LEFT/RIGHT mit Auto-Repeat bei gehaltener Taste
- **Button navigation:** UP → nächster Kanal, DOWN → vorheriger Kanal (modulo 12); ENTER togglet Relais; LEFT/RIGHT verstellt die Restlaufzeit nur bei eingeschaltetem Relais:
  - erster Druck ohne laufenden Timer: Startwert **LEFT = 1 h, RIGHT = 6 h**
  - danach Schrittweite abhängig von der Restzeit (`btnStepMs()`), gerundet auf das Raster; Obergrenze 7 Tage (604800 s)
  - erreicht der Wert 0 → Relais AUS
  - „Timer-Freeze": `timerFreezeStart/Expiry[]` frieren die Restzeit 5 s ein, damit das Verstellen nicht gegen den laufenden Countdown arbeitet
- **Boot-Tastenkombinationen** (in setup(), je 5 s halten, Fortschritt auf dem OLED, Loslassen = Abbruch):
  - **ENTER** → `ui_user`/`ui_pass`/`ota_pass` zurück auf `admin`
  - **DOWN** → E/A-Werkseinstellung: Zuordnungen, Timer, Kanal- und Eingangsnamen zurücksetzen
- **Display:** `displayChannel` (0-11) bestimmt welcher Kanal angezeigt wird; `updateDisplay()` wird von `setRelay()`, Button-Aktionen und loop() (1s-Takt) aufgerufen; `display::tick()` treibt das Namens-Scrolling
- **Temporäre Zeitkorrektur:** `tempTimeAdjustMs[12]` (int32_t) addiert zu autoOffSeconds×1000; wird bei jedem `setRelay()`-Aufruf auf 0 zurückgesetzt; nicht in NVS gespeichert
- **Configuration persistence:** NVS via Preferences API, namespace "io-config"
  - Keys: `ssid`, `pass`, `ui_user`, `ui_pass`, `ota_pass`, `mm<i>` (uint16 Bitmaske), `auto<i>` (uint32), `outmode<i>`, `inmode<i>`, `ord<i>`, `ena<i>`, `prot<i>`, `aoff<i>` (je uint8), `name<i>`, `iname<i>` (String)
  - `aoff<i>` Default 1 → bestehende Installationen schalten wie bisher alles ab, bis ein Kanal bewusst ausgenommen wird
  - Defaults: Kanalnamen "Ausgang 1"–"Ausgang 12", Eingangsnamen "Eingang 1"–"Eingang 12"
  - `load_config` importiert eine komplette Konfiguration als JSON, `reset_io` setzt E/A-Einstellungen zurück
- **Hardware simulation:** Build flag `SIMULATE_HW=1` erlaubt Test ohne physische MCP23017/OLED-Hardware; Wire wird trotzdem initialisiert (inkl. I2C-Bus-Recovery) damit das OLED auch im Sim-Modus funktioniert. **Derzeit NICHT gesetzt** — `build_flags` in platformio.ini enthält nur `BOARD_HAS_PSRAM`, `ARDUINO_USB_CDC_ON_BOOT=1`, `ELEGANTOTA_USE_ASYNC_WEBSERVER=1`
- **I2C Bus Recovery:** `i2cBusRecover()` sendet 9 SCL-Pulse + STOP-Condition vor `Wire.begin()` um SDA-stuck nach Soft-Reset zu beheben; `Wire.setTimeOut(10)` verhindert 145s Blockierung falls Gerät nicht antwortet
- **Peripheral Reset:** GPIO2 (`RESET_PERIPHERIE_PIN`) treibt RESET_PERIPHERIE-Netz (aktiv-low, R302 1kΩ); wird in setup() auf HIGH gesetzt bevor I2C-Init

### Display Layout (128×64 px)

Gerendert mit **Adafruit GFX Built-in-Font** (`setTextSize(1|2)`) — kein u8g2.

```
y= 1  size1  "01/12  Pumpe Keller"   ← Kanal-Nr. + Name (Fenster von 13 Zeichen)
y=11         ──────────────────────  ← Trennlinie (drawLine 0,11 → 127,11)
y=20  size2  "EIN" oder "AUS"        ← Status, x=34
y=46  size1  "Noch: 15:23"           ← Restzeit | "Dauerbetrieb" | "--" | leer wenn AUS
y=57         ▣▢▢▣▢▢▢▢▢▢▢▢            ← 12-Kanal-Statusleiste, 8×6 px + 1 px Gap
```

- Restzeit-Format: `MM:SS` (<1h) oder `HH:MM:SS` (≥1h)
- Statusleiste: gefüllt = Ausgang EIN, Rahmen = AUS; der aktuell angezeigte Kanal ist invertiert umrandet
- Namen länger als 13 Zeichen scrollen automatisch hin und her (`SCROLL_STEP_MS` 400 ms, 3 Pause-Ticks an den Enden), getrieben von `display::tick()`
- UTF-8 → CP437-Konvertierung für deutsche Umlaute (`utf8ToDisplay()`), nicht abbildbare Zeichen werden `?`
- Weitere Screens: `showBoot(version)`, `showMessage(title, line1, line2)` (u. a. für die Boot-Reset-Dialoge), `showIP(apIP, staIP)`

### Hardware Pin Summary

**ESP32-S3 GPIO Allocation (verified from netlist):**
- S0 inputs (12×, output of SN74HCT14DR Schmitt inverters):
  - U200: GPIO4(Inp1), GPIO5(Inp2), GPIO6(Inp3), GPIO7(Inp4), GPIO15(Inp5), GPIO16(Inp6)
  - U201: GPIO17(Inp7), GPIO18(Inp8), GPIO8(Inp9), GPIO3(Inp10), GPIO9(Inp11), GPIO10(Inp12)
- I2C bus: SDA=GPIO11, SCL=GPIO12 (R104/R105 4k7 pull-ups to 3V3p)
- Peripheral reset (active-low): GPIO2 (`RESET_PERIPHERIE_PIN`) → R302/R306 (je 1k0) → RESET_PERIPHERIE-Netz → alle drei MCP23017 (0x20, 0x21, 0x22) RESET-Pin; auch auf J202 Pin 4 geführt (für Top Board und externe Erweiterungsmodule); in setup() auf HIGH gesetzt vor I2C-Init
- Board-to-board connector (P101 → Top Board P1, 14-polig): 3V3p (Pin 1,3), I2C SDA (Pin 2), I2C SCL (Pin 4), 12V (Pin 5,7), GPIO13 (Pin 6, via R248), GPIO14 (Pin 8, via R249), GND (Pin 9,11,13), GPIO21 (Pin 10), TASTER RESET (Pin 12), RESET_PERIPHERIE/GPIO2 (Pin 14)
- Hardware reset: EN/RST pin with R103 (4k7) pull-up, S100 reset button to GND
- USB (internal): GPIO19 (USB_D−), GPIO20 (USB_D+)
- Reserved: GPIO26-32 (flash), GPIO33-34 (octal PSRAM)
- Free GPIOs (laut `pin_config.h`): 0, 1, 2, 14, 21, 35, 36, 37, 38, 39, 40, 41, 42, 45, 46, 47, 48 — GPIO2 ist dabei durch `RESET_PERIPHERIE_PIN` belegt, GPIO14/21 liegen auf dem Board-to-Board-Stecker

**Power Supply:**
- 12V → U101 (AP62200WU-7) buck converter → L102 (ASPI-4020S-3R3M-T) → L100 ferrite → 3V3p
- 12V → U102 (SL4U-1205) isolated DC/DC → L103 ferrite → I-5Vp (S0 input optocouplers)
- I-GND galvanically isolated from main GND, coupled only via C110 (4700pF/1000V)
- 12V directly to relay coils (K300-K311) and ULN2803A COM pins

**I2C Bus — Adress-Schema:**
- 0x20 (A2=0,A1=0,A0=0): Relay **RESET**-Spulen (`MCP_ADDR_RESET`) — GPA0-7=K1-8, GPB0-3=K9-12
- 0x21 (A2=0,A1=0,A0=1): Relay **SET**-Spulen (`MCP_ADDR_SET`) — GPA0-7=K1-8, GPB0-3=K9-12
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
- Spulenwiderstand: 480Ω, Strom: 25mA, Mindestpuls: 10ms (Firmware: `RELAY_PULSE_MS` = 20ms)
- ULN2803A outputs → Ferrit-Perlen (L300-L311, 742792651) → Relay-Spulen

**Connectors:**
- J200 (MCV 1,5/9-G-3,5): S0 inputs 1-8 (pins 1-8), I-GND (pin 9)
- J201 (MCV 1,5/9-G-3,5): S0 inputs 9-12 (pins 1-4), 12V power (pins 5-6), spare (pins 7-8), I-GND (pin 9)
- J202 (MCV 1,5/9-G-3,5): externe Buchse, normalerweise unbestückt — I2C SDA (pin 1, via R252), SCL (pin 2, via R253), GND (pin 3), RESET_PERIPHERIE/GPIO2 (pin 4), GPIO13 (pin 5, via R248), GPIO14 (pin 6, via R249), 12V (pin 7), 3V3p (pin 8), GND (pin 9)
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
- `Adafruit SSD1306` + `Adafruit GFX Library` — OLED display driver, SSD1306 128×64, HW I2C; `begin(SSD1306_SWITCHCAPVCC, addr, false, false)` mit internem Charge-Pump, ohne Reset und ohne erneutes `Wire.begin()`
- `ElegantOTA` (ayushsharma82) — Firmware-Update über die Web-UI (`/update`), mit `ELEGANTOTA_USE_ASYNC_WEBSERVER=1`

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

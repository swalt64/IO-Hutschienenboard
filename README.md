# IO-Hutschienenboard

ESP32-S3 basierter 12-Kanal-I/O-Hutschienencontroller mit Web-UI, OLED-Display und Android-App.

Aktuelle Firmware-Version: siehe `IO-Hutschienenboard_SRC/include/version.h` (wird bei jedem Upload automatisch hochgezählt).

## Hardware

Zwei-Platinen-Aufbau (Base Board + Top Board) mit ESP32-S3-WROOM-1 (N16R8, DUBEUYEW 44-pin DIL-Modul, Versorgung direkt über 3V3).

**Base Board:**
- **12× S0-Eingänge** — galvanisch getrennt (SL4U-1205 DC/DC), 6× ILD213T Optokoppler, 2× SN74HCT14DR Schmitt-Trigger; unterstützt DC-Kontakte und 8VAC-Erkennung
- **12× Relaisausgänge** — Panasonic DSP1A-L2-DC12V, **bistabil mit zwei Spulen**; angesteuert über **zwei** MCP23017 (0x21 = SET-Spulen, 0x20 = RESET-Spulen) → ULN2803A-Treiber → Ferritperlen → Relaisspulen; Impulsdauer 20 ms
- **Versorgung** — 12V-Eingang, AP62200WU-7 Buck-Wandler → 3V3p; SL4U-1205 isolierter DC/DC → I-5Vp für die Optokoppler
- **Peripherie-Reset** — GPIO2 (aktiv-low) auf alle drei MCP23017
- **Steckverbinder** — J200/J201 (S0-Eingänge + I-GND), J202 (externe Buchse, normalerweise unbestückt: I2C, GPIO13/14, Reset, 12V, 3V3p), J300–J302 (Relaiskontakte)

**Top Board (über 14-poligen Board-to-Board-Stecker P101 → P1):**
- **OLED-Display 128×64** — JMD0.96-1 (SSD1306) an I2C 0x3C
- **5 Navigationstasten** — ENTER, UP, DOWN, LEFT, RIGHT an MCP23017 0x22, GPA0–GPA4 (aktiv-low), Interrupt via INTA → GPIO13
- **3 Status-LEDs** — WLAN (rot), OUTPUT (gelb), RUN (grün) an MCP23017 0x22, GPB0–GPB2
- **Reset-Taster** auf dem EN-Pin des ESP32

**I2C-Adressen:** 0x20 (Relais RESET), 0x21 (Relais SET), 0x22 (Top Board), 0x3C (OLED). Bus: SDA = GPIO11, SCL = GPIO12, 100 kHz.

## Projektstruktur

- `IO-Hutschienenboard_SRC/` PlatformIO-Projektwurzel
  - `src/main.cpp` Kernanwendung: WiFi, AsyncWebServer, WebSocket, Relaissteuerung, Eingangs-Flankenerkennung, Auto-Off-Timer, Kanalnamen, Tastennavigation, NVS-Persistenz
  - `src/display.cpp` OLED-Ausgabe (Adafruit SSD1306), Namespace `display::`
  - `src/statusled.cpp` WS2812-RGB-LED-Muster (non-blocking)
  - `src/swtools.cpp` Debug-Logging mit Kategorien, ANSI-Farben, NTP-Zeitstempel
  - `include/pin_config.h` GPIO-Mapping, MCP-Adressen, Relaistabellen
  - `data/` LittleFS-Web-Assets: `index.html` (Bedienung), `config.html` (Konfiguration), `mapping.html` (Eingang→Ausgang), `status.html` (Systemstatus), PWA-Manifest + Service Worker
  - `boards/` eigenes PlatformIO-Board-Profil (`esp32-s3-devkitc-1-n16r8`)
  - `scripts/` Build-Hooks (Versions-Bump, OTA-Passwort einlesen)
- `AndroidApp/` native Android-App (siehe [AndroidApp/README.md](AndroidApp/README.md))
- `HARDWARE/PCB/` Altium-Designdaten (Base Board + Top Board)
- `DOKU/` Dokumentation und LTspice-Simulationen

## Funktionen

**Bedienung und Netzwerk**
- AP-Modus (`IO-Hutschiene`, 192.168.50.1) mit DHCP, parallel optionaler STA-Modus mit gespeicherten Zugangsdaten
- mDNS-Hostname `HS-IO` (`hs-io.local`)
- Web-UI mit WebSocket-Live-Updates, HTTP-Basic-Auth auf allen Seiten, der REST-API und dem WebSocket (Default `admin`/`admin`, in der UI änderbar)
- REST-API: `GET /api/state` (Gesamtzustand als JSON, inkl. `wifi_rssi`, `wifi_bssid`, `wifi_channel`), `GET /api/cmd` (`toggle`, `set`, `alloff`)
- Verbindet sich beim Start mit dem **stärksten** Access Point der SSID (relevant bei Repeater/Mesh mit gleicher SSID)
- PWA-fähig (Manifest + Service Worker), Android-App über dasselbe WebSocket-Protokoll

**Ausgänge**
- Auto-Off-Timer je Kanal mit Live-Countdown in Web-UI und Display
- Betriebsart je Ausgang: Toggle oder Taster
- Kanäle einzeln aktivierbar, frei sortierbar (Anzeigereihenfolge)
- Fernsteuer-Schutz je Ausgang: geschützte Kanäle schalten nur mit gesetztem `armed`-Flag
- „Alles aus" per Langdruck (2 s) auf einen Eingangs-Taster, über die Web-UI oder per langem Tastendruck in der App
- **Je Ausgang konfigurierbar, ob er bei „Alles aus" mitgeschaltet wird** — Spalte „Alles aus" in der Konfigurationsseite. Damit lassen sich Verbraucher wie eine Klimaanlage von der Sammelabschaltung ausnehmen; sie bleiben dann an, egal ob die Abschaltung vom Taster, der Web-UI oder der App kommt. Standard: alle Ausgänge werden mitgeschaltet.

**Eingänge**
- Flankenerkennung im `loop()`, gemapptes Relais wird geschaltet
- Betriebsart je Eingang: Toggle oder Taster
- DC- und AC-Erkennung (50 Hz Rechteck)
- Eigene Namen je Eingang

**Vor-Ort-Bedienung**
- OLED zeigt Kanalnummer, Name, EIN/AUS und Restlaufzeit
- UP/DOWN blättert Kanäle, ENTER schaltet, LEFT/RIGHT ändert die Restlaufzeit um ±1 min (temporär, nicht in NVS)
- WLAN-LED signalisiert Verbindungszustand und Signalqualität über die Blinkrate (Dauerlicht ab RSSI ≥ 50 %)

**Persistenz**
- NVS via Preferences, Namespace `io-config`: `ssid`, `pass`, `ui_user`, `ui_pass`, `ota_pass`, `mm*` (Eingangs-Zuordnung als Bitmaske), `auto*`, `outmode*`, `inmode*`, `ord*`, `ena*`, `prot*`, `name*`, `iname*`
- Konfiguration in der Web-UI exportier- und importierbar (`load_config`), Werksreset über `reset_io`
- **Reset per Taste beim Booten** (je 5 s halten, Fortschritt auf dem OLED, Loslassen bricht ab):
  - **ENTER** → Web- und OTA-Passwort zurück auf `admin`/`admin`
  - **DOWN** → E/A-Werkseinstellung: Zuordnungen, Timer und Namen zurücksetzen

**Update**
- ElegantOTA über die Web-UI (`/update`, passwortgeschützt)
- ArduinoOTA über PlatformIO-Environment `esp32s3-ota` (Port 3232)

## Build und Flash

Aus `IO-Hutschienenboard_SRC/` ausführen:

```powershell
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" run
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" run -t upload
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" run -t uploadfs
```

Nach Änderungen an `data/` muss `uploadfs` ausgeführt werden, sonst bleibt die alte Web-UI im Flash.

Die Plattform ist in `platformio.ini` fest auf `espressif32@6.12.0` gepinnt. Der Pin ist notwendig — mit dem neueren Arduino-Core 3.x kompilieren die WS2812-Bibliothek und die DHCP-Abfrage in `main.cpp` nicht mehr.

### OTA-Upload über WLAN

```powershell
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" run -e esp32s3-ota -t upload
```

Das OTA-Passwort wird aus `ota_pass.txt` gelesen (gitignored, Fallback `admin`). Nach einer Passwortänderung in der Web-UI muss diese Datei auf dem Entwicklungsrechner aktualisiert werden. Die Ziel-IP steht als `upload_port` in `platformio.ini`.

## Serieller Monitor

`platformio.ini` ist auf Rohausgabe konfiguriert: `monitor_speed = 115200`, `monitor_port = COM10`, `monitor_raw = yes`.

```powershell
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" device monitor -e esp32s3
```

## Bibliotheken

`ESP Async WebServer` (mathieucarbou) · `ArduinoJson` · `Adafruit MCP23017` · `Adafruit SSD1306` + `Adafruit GFX` · `Freenove WS2812 Lib for ESP32` · `ElegantOTA`

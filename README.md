# IO-Hutschienenboard

ESP32-S3 based 12-channel I/O DIN-rail controller with web UI.

## Hardware

Two-board design (base board + top board) with ESP32-S3-WROOM-1 (N16R8).

**Base Board:**
- **12× S0 inputs** — galvanically isolated (SL4U-1205 DC/DC), 6× ILD213T optocouplers, 2× SN74HCT14DR Schmitt triggers, supports DC dry contacts and 8VAC detection
- **12× relay outputs** — DSP1a-DC12V monostable relays (12V coil), MCP23017 (U300, 0x20) → 4k7 arrays → 2× ULN2803AFWG drivers → ferrite beads → relay coils
- **Power** — 12V input, AP62200WU-7 buck converter → 3V3p, SL4U-1205 isolated DC/DC → I-5Vp
- **Connectors** — J200/J201 (S0 inputs + I-GND), J202 (I2C + top board), J300-J302 (relay contacts)

**Top Board (via J202 connector):**
- **12× status LEDs** — via MCP23017 (0x21)
- **12× buttons + 1 reset button** — via MCP23017 (0x22) with interrupt, reset button on ESP32 EN pin

## Project Layout

- `IO-Hutschienenboard_SRC/` PlatformIO project root
- `IO-Hutschienenboard_SRC/src/` firmware source
- `IO-Hutschienenboard_SRC/data/` LittleFS web assets
- `IO-Hutschienenboard_SRC/boards/` custom PlatformIO board profile (`esp32-s3-devkitc-1-n16r8`)
- `HARDWARE/PCB/` Altium PCB design files (base board + top board)

## Features

- AP mode (`IO-Hutschiene`) with DHCP and web interface
- Optional STA mode using saved WiFi credentials
- WebSocket-based live state updates
- Auto-off timers per relay channel
- Live countdown in web UI until relay auto-off
- S0 input supports both DC and AC detection
- Top board buttons with interrupt-driven detection
- AP client debug output includes MAC and assigned IPv4

## Build and Flash

Run from `IO-Hutschienenboard_SRC/`:

```powershell
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" run
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" run -t upload
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" run -t uploadfs
```

## Serial Monitor

`platformio.ini` is configured for raw monitor output:

- `monitor_speed = 115200`
- `monitor_port = COM3`
- `monitor_raw = yes`

Start monitor:

```powershell
& "C:\Users\stwal\.platformio\penv\Scripts\platformio.exe" device monitor -e esp32s3
```

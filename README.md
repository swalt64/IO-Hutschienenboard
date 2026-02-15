# IO-Hutschienenboard

ESP32-S3 based 12-channel I/O DIN-rail controller with web UI.

## Project Layout

- `IO-Hutschienenboard_SRC/` PlatformIO project root
- `IO-Hutschienenboard_SRC/src/` firmware source
- `IO-Hutschienenboard_SRC/data/` LittleFS web assets
- `IO-Hutschienenboard_SRC/boards/` custom PlatformIO board profile (`esp32-s3-devkitc-1-n16r8`)

## Features

- AP mode (`IO-Hutschiene`) with DHCP and web interface
- Optional STA mode using saved WiFi credentials
- WebSocket-based live state updates
- Auto-off timers per relay channel
- Live countdown in web UI until relay auto-off
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
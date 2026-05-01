# HS-IO Android App

Native Android-App fuer das IO-Hutschienenboard.

## Funktionen

- Hauptansicht zeigt nur die 12 Ausgaenge.
- Links je Ausgang eine LED fuer den aktuellen Ausgangszustand.
- Rechts je Ausgang ein Button.
- Hochformat: einspaltige Liste.
- Querformat: zweispaltiges Raster.
- Verbindet sich per WebSocket mit `/ws` des ESP32.
- Verwendet dieselben Kommandos wie die Webseite:
  - Toggle-Ausgang: `{"cmd":"toggle","ch":n}`
  - Taster-Ausgang: `{"cmd":"set","ch":n,"val":true/false}`
  - langer Druck auf Toggle-Button: `{"cmd":"alloff"}`

## Build

In Android Studio den Ordner `AndroidApp` oeffnen und `app` bauen.

Standardverbindung:

- Host: `hs-io.local`
- Benutzer: `admin`
- Passwort: `admin`

Die Werte koennen in der App ueber das Menue `Einstellungen` geaendert werden.

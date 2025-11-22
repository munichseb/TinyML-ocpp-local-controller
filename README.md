# TinyML OCPP Local Controller

Dieses Repository enthält einen Arduino-Sketch für einen lokalen OCPP 1.6 Edge-Gateway-Prototypen auf Basis der **Arduino Nicla Vision**. Das Gateway stellt eine WebSocket-Brücke zwischen mehreren Wallboxen im lokalen Netz und einem frei konfigurierbaren Backend bereit und bietet zusätzlich ein leichtgewichtiges Web-Dashboard zur Konfiguration.

## Funktionsumfang
- Akzeptiert bis zu **10 gleichzeitige WebSocket-Verbindungen** von Wallboxen auf Port **8080** und leitet deren Nachrichten an ein zentrales Backend weiter.
- Baut eine **WebSocket-Verbindung zum Backend** auf und verteilt Antworten des Backends an alle verbundenen Wallboxen (Broadcast).
- Stellt einen **HTTP-Dashboard** auf Port **80** bereit, um Backend-Host/Port sowie WLAN-Zugangsdaten anzupassen und den Verbindungsstatus zu prüfen.
- Unterstützt eine **Fallback-Access-Point**-Funktion (SSID `NiclaGateway-Setup` / Passwort `setup1234`), falls der Verbindungsaufbau zum gewünschten WLAN scheitert.

## Projektstruktur
- `arduino/nicla_vision_ocpp_gateway/`: Hauptsketch `nicla_vision_ocpp_gateway.ino` für die Nicla Vision.
- `arduino/README.md`: Kurzanleitung für Arduino-spezifische Schritte.

## Voraussetzungen
- **Arduino IDE** mit Board-Unterstützung für **Nicla Vision**.
- Bibliotheken über den Arduino Library Manager installieren:
  - `WiFiNINA_Generic` (statt `WiFiNINA`, damit die Nicla Vision unter `mbed_nicla` erkannt wird)
  - `ArduinoWebsockets_Generic`

## Einrichtung
1. Arduino IDE öffnen und `arduino/nicla_vision_ocpp_gateway/nicla_vision_ocpp_gateway.ino` laden.
2. Board **Nicla Vision** und den korrekten Port auswählen.
3. Benötigte Bibliotheken installieren (siehe oben).
4. Sketch kompilieren und auf das Board hochladen.
5. Serielle Konsole mit **115200 Baud** öffnen, um IP-Adresse und WLAN-Status zu prüfen.

## Konfiguration & Nutzung
- **Standard-WLAN**: SSID `FiberWAN`, Passwort `sommer17`. Bei fehlgeschlagenem Verbindungsaufbau wird automatisch der Setup-Hotspot `NiclaGateway-Setup` mit Passwort `setup1234` aktiviert.
- **Dashboard aufrufen**: Browser aufrufen und `http://<device-ip>/` öffnen (IP aus der seriellen Konsole oder aus dem Setup-Hotspot ermitteln).
- **Backend konfigurieren**: Im Dashboard Host/IP und Port eintragen. Das Gerät baut dann automatisch eine WebSocket-Verbindung zum Backend auf.
- **Wallbox verbinden**: Wallbox per WebSocket auf `ws://<device-ip>:8080/` konfigurieren. Bis zu zehn Wallboxen können parallel verbunden werden.

## Beispiel OCPP-Backend-URL
Eine typische OCPP-1.6-Endpoint-URL für das Backend könnte wie folgt aussehen:

```
ws://192.168.0.100:8081/
```

Ersetzen Sie Host und Port gemäß Ihrer Backend-Installation. Die Nicla Vision leitet anschließend alle Wallbox-Nachrichten an diese Adresse weiter und verteilt Backend-Antworten an alle verbundenen Clients.

## Fehlerbehebung
- **Keine WLAN-Verbindung**: Prüfen, ob die Standard- oder konfigurierte SSID erreichbar ist. Andernfalls über den Setup-Hotspot verbinden und WLAN-Daten im Dashboard aktualisieren.
- **Keine Backend-Verbindung**: Stellen Sie sicher, dass der Backend-Host erreichbar ist und der Port offen ist. Die serielle Konsole gibt beim Verbindungsaufbau Hinweise aus.
- **Wallbox verbindet nicht**: Kontrollieren Sie, ob Port 8080 erreichbar ist und die Wallbox mit WebSocket (`ws://`) konfiguriert ist.

## Lizenz
Siehe individuelle Lizenzhinweise der Arduino-Beispiele und Bibliotheken. Dieses Repository enthält ausschließlich Beispielcode für Entwicklungs- und Demonstrationszwecke.

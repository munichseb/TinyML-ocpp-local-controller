# Arduino Sketches for OCPP Gateways

This folder contains Arduino IDE projects for the Nicla Vision- and Atom S3 Lite-based OCPP 1.6 gateway prototypes.

## ESP32-S3 3.5" Capacitive Touch Gateway

Sketch: `esp32_s3_ocpp_touch/esp32_s3_ocpp_touch.ino`

* Target: ESP32-S3 with 320x480 capacitive touch display (TFT_eSPI configured for your panel).
* Features: hosts a configuration web server on port 80, starts a fallback Wi-Fi AP for provisioning, accepts up to five OCPP 1.6 WebSocket wallbox connections on port 7020 and forwards them to a configurable backend, and shows Wi-Fi/connection status on the display.
* Libraries: `WiFi`, `WebServer`, `Preferences`, `WebSocketsServer`, `WebSocketsClient`, and `TFT_eSPI` (ensure `User_Setup` matches your display controller/pinout).

## Required libraries

### Nicla Vision (`nicla_vision_ocpp_gateway`)

Compile the `nicla_vision_ocpp_gateway` sketch with the following libraries from the Arduino Library Manager:

- **WiFiNINA** (built into the Nicla Vision core). Avoid installing `WiFiNINA_Generic`, which conflicts with the WiFiNINA classes used by the sketch and causes multiple-definition errors.
- **WebSockets2_Generic**
- **WiFiWebServer**

Make sure `Tools → Board` is set to **Nicla Vision** (mbed_nicla). If you see WebSockets errors about missing `WSDefaultTcpServer`, add the line `#define WEBSOCKETS_NETWORK_TYPE NETWORK_WIFI` before including `WebSockets2_Generic.h` as shown in the sketch.

### Atom S3 Lite (`atom_s3_ocpp_proxy`)

For the Atom S3 Lite OCPP proxy, install these libraries via the Library Manager:

- **WebSockets** by Markus Sattler
- **Preferences** (bundled with the ESP32 core)
- **WiFi** (bundled with the ESP32 core)

Select the board **M5Stack-ATOM (ESP32-S3)** or the appropriate Atom S3 Lite entry from the ESP32 boards package, then build and upload the `atom_s3_ocpp_proxy` sketch.


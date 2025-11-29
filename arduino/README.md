# Arduino Sketches for OCPP Gateways

This folder contains Arduino IDE projects for the Nicla Vision- and Atom S3 Lite-based OCPP 1.6 gateway prototypes.

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


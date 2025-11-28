# Arduino Sketches for Nicla Vision OCPP Gateway

This folder contains Arduino IDE projects for the Nicla Vision-based OCPP 1.6 edge gateway prototype.

## Required libraries

Compile the `nicla_vision_ocpp_gateway` sketch with the following libraries from the Arduino Library Manager:

- **WiFiNINA** (built into the Nicla Vision core). Avoid installing `WiFiNINA_Generic`, which conflicts with the WiFiNINA classes used by the sketch and causes multiple-definition errors.
- **WebSockets2_Generic**
- **WiFiWebServer**

Make sure `Tools → Board` is set to **Nicla Vision** (mbed_nicla). If you see WebSockets errors about missing `WSDefaultTcpServer`, add the line `#define WEBSOCKETS_NETWORK_TYPE NETWORK_WIFI` before including `WebSockets2_Generic.h` as shown in the sketch.


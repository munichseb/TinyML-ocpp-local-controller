# Arduino Sketches for Nicla Vision OCPP Gateway

This folder contains Arduino IDE projects for the Nicla Vision-based OCPP 1.6 edge gateway prototype.

## nicla_vision_ocpp_gateway

An OCPP 1.6 edge gateway sketch for the Nicla Vision that terminates up to 10 local WebSocket connections from wallboxes and forwards the traffic to a configurable backend.

### Features
- Default Wi‑Fi credentials: SSID `FiberWAN`, password `sommer17`.
- HTTP dashboard on port **80** to show the number of connected wallboxes, change backend host/port, and update Wi‑Fi credentials.
- WebSocket server for wallboxes on port **8080** with room for up to ten concurrent clients.
- Forwards messages from wallboxes to the configured backend and broadcasts backend responses back to all wallboxes.
- Falls back to a setup hotspot (`NiclaGateway-Setup` / `setup1234`) when the configured Wi‑Fi cannot be reached.

### Required Libraries
- **WiFiNINA** (Arduino Library Manager; bundled with Nicla Vision support when you install the board package — no `WiFiNINA_Generic` override required)
- **ArduinoWebsockets** (Arduino Library Manager, listed as *ArduinoWebsockets* by **gilmaimon**)

> 💡 If you come across older guides mentioning `WiFiNINA_Generic`, you can ignore that workaround on current Nicla Vision packages. The standard `WiFiNINA` works out of the box with **ArduinoWebsockets**.

### Uploading
1. Open `arduino/nicla_vision_ocpp_gateway/nicla_vision_ocpp_gateway.ino` in the Arduino IDE.
2. Select **Nicla Vision** as the board and choose the correct port.
3. Install the required libraries listed above.
4. Upload the sketch.
5. Open the serial monitor at 115200 baud to confirm network status and retrieve the assigned IP address.
6. From a browser on the same network (or the setup hotspot), navigate to `http://<device-ip>/` to access the dashboard.

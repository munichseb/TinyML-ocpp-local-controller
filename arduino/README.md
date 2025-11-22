# Arduino Sketches for Nicla Vision OCPP Gateway

This folder contains Arduino IDE projects for the Nicla Vision-based OCPP 1.6 edge gateway prototype.

## nicla_vision_ocpp_gateway

A minimal sketch that connects the Nicla Vision to Wi‑Fi and serves a simple web page.

### Features
- Connects to the `FiberWAN` network with password `sommer17`.
- Starts an HTTP server on port 80 that responds with a "Hello World" page.
- Prints network status information to the serial monitor.

### Uploading
1. Open `arduino/nicla_vision_ocpp_gateway/nicla_vision_ocpp_gateway.ino` in the Arduino IDE.
2. Select **Nicla Vision** as the board and choose the correct port.
3. Install the **WiFiNINA** library if it is not already available.
4. Upload the sketch.
5. Open the serial monitor at 115200 baud to confirm the Wi‑Fi connection and note the assigned IP address.
6. From a browser on the same network, navigate to `http://<device-ip>/` to see the "Hello World" page.

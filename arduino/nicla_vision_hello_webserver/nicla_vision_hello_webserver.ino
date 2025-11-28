#include <Arduino.h>
#include <WiFiNINA.h>

/*
 * Simple Wi-Fi web server for Arduino Nicla Vision.
 *
 * Fill in your Wi-Fi credentials below, upload the sketch, and open the
 * serial monitor at 115200 baud. The IP address of the board will be
 * printed once it connects. Point a browser at http://<board-ip>/ to see
 * the "Hello World" message served from the board.
 */

// TODO: Update these with your Wi-Fi network credentials.
const char WIFI_SSID[] = "YOUR_WIFI_SSID";
const char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";

WiFiServer server(80);

void connectToWifi() {
  while (WiFi.begin(WIFI_SSID, WIFI_PASSWORD) != WL_CONNECTED) {
    Serial.print(F("Attempting to connect to SSID: "));
    Serial.println(WIFI_SSID);
    delay(2000);
  }

  Serial.println();
  Serial.println(F("WiFi connected"));
  Serial.print(F("IP address: "));
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ;
  }

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println(F("Communication with WiFi module failed!"));
    while (true) {
      delay(1000);
    }
  }

  // Check the firmware version and warn if an update is needed.
  const String firmwareVersion = WiFi.firmwareVersion();
  Serial.print(F("WiFi firmware: "));
  Serial.println(firmwareVersion);

  connectToWifi();
  server.begin();
}

void loop() {
  WiFiClient client = server.available();
  if (!client) {
    return;
  }

  // Wait until the client sends data.
  while (!client.available()) {
    delay(1);
  }

  // Simple HTTP response with "Hello World".
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/plain");
  client.println("Connection: close");
  client.println();
  client.println("Hello World");

  client.stop();
}

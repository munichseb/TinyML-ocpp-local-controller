// ESP32-S3 OCPP 1.6 Proxy with Capacitive Touch Display (320x480)
//
// This sketch accepts up to five OCPP WebSocket connections from wallboxes
// on port 7020 and forwards them to a configurable backend. A small
// configuration web server runs on port 80. If Wi-Fi credentials are missing
// or invalid, the board exposes a fallback access point so credentials and the
// backend endpoint can be configured.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#define WEBSOCKETS_NETWORK_TYPE NETWORK_WIFI
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>

#include <TFT_eSPI.h>

static const uint8_t MAX_WALLBOX_CLIENTS = 5;
static const uint16_t WALLBOX_PORT = 7020;
static const uint16_t HTTP_PORT = 80;

static const char AP_SSID[] = "OCPP-Proxy-Setup";
static const char AP_PASSWORD[] = "setup1234";
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

struct ProxyConfig {
  char ssid[32];
  char password[64];
  char backendHost[64];
  uint16_t backendPort;
  char backendPath[64];
  bool valid;
};

ProxyConfig config;
Preferences prefs;

WebServer webServer(HTTP_PORT);
WebSocketsServer wallboxServer(WALLBOX_PORT);

struct ProxyEndpoint {
  bool active;
  uint8_t clientId;
  WebSocketsClient backend;
};

ProxyEndpoint endpoints[MAX_WALLBOX_CLIENTS];

bool apMode = false;
unsigned long lastWifiAttempt = 0;

TFT_eSPI tft = TFT_eSPI();
String lastDisplayStatus;
uint8_t lastConnectionCount = 0;

void handleBackendEvent(uint8_t idx, WStype_t type, uint8_t *payload, size_t length);
void drawStatus();

void loadConfig() {
  prefs.begin("ocpp-proxy", false);
  config.valid = prefs.getBool("valid", false);

  if (config.valid) {
    String ssid = prefs.getString("ssid", "");
    String password = prefs.getString("password", "");
    String host = prefs.getString("backendHost", "");
    String path = prefs.getString("backendPath", "");
    config.backendPort = prefs.getUShort("backendPort", 9000);

    strncpy(config.ssid, ssid.c_str(), sizeof(config.ssid) - 1);
    config.ssid[sizeof(config.ssid) - 1] = '\0';
    strncpy(config.password, password.c_str(), sizeof(config.password) - 1);
    config.password[sizeof(config.password) - 1] = '\0';
    strncpy(config.backendHost, host.c_str(), sizeof(config.backendHost) - 1);
    config.backendHost[sizeof(config.backendHost) - 1] = '\0';
    strncpy(config.backendPath, path.c_str(), sizeof(config.backendPath) - 1);
    config.backendPath[sizeof(config.backendPath) - 1] = '\0';
  } else {
    memset(&config, 0, sizeof(config));
    strncpy(config.backendHost, "backend.example.com", sizeof(config.backendHost) - 1);
    strncpy(config.backendPath, "/ocpp", sizeof(config.backendPath) - 1);
    config.backendPort = 9000;
  }
}

void saveConfig() {
  prefs.putBool("valid", true);
  prefs.putString("ssid", config.ssid);
  prefs.putString("password", config.password);
  prefs.putString("backendHost", config.backendHost);
  prefs.putUShort("backendPort", config.backendPort);
  prefs.putString("backendPath", config.backendPath);
}

void startAccessPoint() {
  Serial.println(F("Starting fallback access point"));
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  Serial.print(F("AP IP address: "));
  Serial.println(WiFi.softAPIP());
  drawStatus();
}

void connectWifi() {
  apMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.begin(config.ssid, config.password);
  Serial.print(F("Connecting to WiFi SSID: "));
  Serial.println(config.ssid);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi connected. IP="));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("Failed to connect to WiFi, enabling AP mode"));
    startAccessPoint();
  }
  drawStatus();
}

void ensureWifi() {
  if (apMode) return;
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiAttempt > WIFI_CONNECT_TIMEOUT_MS) {
    lastWifiAttempt = now;
    connectWifi();
  }
}

void handleRoot() {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><title>OCPP Proxy</title></head><body>");
  html += F("<h2>ESP32-S3 OCPP Proxy</h2><form method='POST' action='/save'>");
  html += F("<label>WiFi SSID: <input name='ssid' value='");
  html += String(config.ssid);
  html += F("'></label><br><label>WiFi Password: <input type='password' name='password' value='");
  html += String(config.password);
  html += F("'></label><br><label>Backend Host: <input name='backendHost' value='");
  html += String(config.backendHost);
  html += F("'></label><br><label>Backend Port: <input name='backendPort' type='number' value='");
  html += String(config.backendPort);
  html += F("'></label><br><label>Backend Path: <input name='backendPath' value='");
  html += String(config.backendPath);
  html += F("'></label><br><button type='submit'>Save</button></form>");
  html += F("<p>Device IP: ");
  html += apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  html += F("</p><p>Wallbox connections: ");
  html += String(lastConnectionCount);
  html += F("</p></body></html>");
  webServer.send(200, "text/html", html);
}

void handleSave() {
  if (webServer.hasArg("ssid")) {
    strncpy(config.ssid, webServer.arg("ssid").c_str(), sizeof(config.ssid) - 1);
    config.ssid[sizeof(config.ssid) - 1] = '\0';
  }
  if (webServer.hasArg("password")) {
    strncpy(config.password, webServer.arg("password").c_str(), sizeof(config.password) - 1);
    config.password[sizeof(config.password) - 1] = '\0';
  }
  if (webServer.hasArg("backendHost")) {
    strncpy(config.backendHost, webServer.arg("backendHost").c_str(), sizeof(config.backendHost) - 1);
    config.backendHost[sizeof(config.backendHost) - 1] = '\0';
  }
  if (webServer.hasArg("backendPort")) {
    config.backendPort = webServer.arg("backendPort").toInt();
  }
  if (webServer.hasArg("backendPath")) {
    strncpy(config.backendPath, webServer.arg("backendPath").c_str(), sizeof(config.backendPath) - 1);
    config.backendPath[sizeof(config.backendPath) - 1] = '\0';
  }

  config.valid = true;
  saveConfig();

  webServer.send(200, "text/plain", "Saved. Attempting to connect with new settings.");
  connectWifi();
}

int8_t findEndpointIndex(uint8_t clientId) {
  for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
    if (endpoints[i].active && endpoints[i].clientId == clientId) {
      return i;
    }
  }
  return -1;
}

void connectBackend(uint8_t idx) {
  endpoints[idx].backend.begin(config.backendHost, config.backendPort, config.backendPath);
  endpoints[idx].backend.onEvent([idx](WStype_t type, uint8_t *payload, size_t length) {
    handleBackendEvent(idx, type, payload, length);
  });
  endpoints[idx].backend.setReconnectInterval(3000);
  endpoints[idx].backend.enableHeartbeat(60, 3000, 2);
  endpoints[idx].backend.connect();
}

void handleBackendEvent(uint8_t idx, WStype_t type, uint8_t *payload, size_t length) {
  if (!endpoints[idx].active) return;
  uint8_t clientId = endpoints[idx].clientId;

  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("Backend connected for client %u\n", clientId);
      break;
    case WStype_DISCONNECTED:
      Serial.printf("Backend disconnected for client %u\n", clientId);
      break;
    case WStype_TEXT:
      wallboxServer.sendTXT(clientId, payload, length);
      break;
    case WStype_BIN:
      wallboxServer.sendBIN(clientId, payload, length);
      break;
    default:
      break;
  }
}

void handleWallboxEvent(uint8_t clientId, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      IPAddress ip = wallboxServer.remoteIP(clientId);
      Serial.printf("Wallbox connected: %u from %s\n", clientId, ip.toString().c_str());
      int8_t slot = findEndpointIndex(clientId);
      if (slot < 0) {
        for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
          if (!endpoints[i].active) {
            slot = i;
            break;
          }
        }
      }
      if (slot < 0) {
        Serial.println(F("No available backend slots; closing connection"));
        wallboxServer.disconnect(clientId);
        return;
      }
      endpoints[slot].active = true;
      endpoints[slot].clientId = clientId;
      connectBackend(slot);
      break;
    }
    case WStype_DISCONNECTED: {
      Serial.printf("Wallbox disconnected: %u\n", clientId);
      int8_t slot = findEndpointIndex(clientId);
      if (slot >= 0) {
        endpoints[slot].backend.disconnect();
        endpoints[slot].active = false;
      }
      break;
    }
    case WStype_TEXT: {
      int8_t slot = findEndpointIndex(clientId);
      if (slot >= 0) {
        endpoints[slot].backend.sendTXT(payload, length);
      }
      break;
    }
    case WStype_BIN: {
      int8_t slot = findEndpointIndex(clientId);
      if (slot >= 0) {
        endpoints[slot].backend.sendBIN(payload, length);
      }
      break;
    }
    default:
      break;
  }
  drawStatus();
}

uint8_t connectionCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
    if (endpoints[i].active) count++;
  }
  return count;
}

void drawStatus() {
  String status;
  if (apMode) {
    status = "AP mode: " + String(AP_SSID);
  } else if (WiFi.status() == WL_CONNECTED) {
    status = "WiFi: " + String(config.ssid) + " (" + WiFi.localIP().toString() + ")";
  } else {
    status = "WiFi: connecting...";
  }
  uint8_t count = connectionCount();

  if (status == lastDisplayStatus && count == lastConnectionCount) return;
  lastDisplayStatus = status;
  lastConnectionCount = count;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("ESP32-S3 OCPP Proxy");
  tft.setCursor(10, 40);
  tft.println(status);
  tft.setCursor(10, 70);
  tft.printf("Wallbox connections: %u\n", count);
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.onNotFound([]() { webServer.send(404, "text/plain", "Not found"); });
  webServer.begin();
}

void setupDisplay() {
  tft.init();
  tft.setRotation(1); // Landscape for 320x480
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Starting OCPP proxy...");
}

void setup() {
  Serial.begin(115200);
  setupDisplay();
  loadConfig();
  connectWifi();

  wallboxServer.begin();
  wallboxServer.onEvent(handleWallboxEvent);

  setupWebServer();
  drawStatus();
}

void loop() {
  ensureWifi();
  webServer.handleClient();
  wallboxServer.loop();

  for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
    if (endpoints[i].active) {
      endpoints[i].backend.loop();
    }
  }

  static unsigned long lastDisplayRefresh = 0;
  if (millis() - lastDisplayRefresh > 2000) {
    drawStatus();
    lastDisplayRefresh = millis();
  }
}

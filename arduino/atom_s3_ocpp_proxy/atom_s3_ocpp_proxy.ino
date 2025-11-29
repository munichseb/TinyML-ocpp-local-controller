#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include <Preferences.h>

#define WEBSOCKETS_NETWORK_TYPE NETWORK_WIFI

static const uint16_t WALLBOX_PORT = 7020;
static const uint8_t MAX_WALLBOX_CLIENTS = 5;
static const char *AP_SSID = "AtomS3-OCPP-Setup";
static const char *AP_PASSWORD = "setup1234";

struct GatewayConfig {
  char ssid[32];
  char password[64];
  char backendHost[64];
  uint16_t backendPort;
  bool valid;
};

Preferences prefs;
GatewayConfig config = {};
bool apMode = false;

WebServer httpServer(80);
WebSocketsServer wallboxServer(WALLBOX_PORT);
WebSocketsClient backendClient;
unsigned long lastBackendAttempt = 0;

static bool wallboxActive[MAX_WALLBOX_CLIENTS] = {false};

void saveConfig() {
  prefs.begin("ocppgw");
  prefs.putBytes("config", &config, sizeof(config));
  prefs.end();
}

void loadConfig() {
  prefs.begin("ocppgw", true);
  size_t size = prefs.getBytesLength("config");
  if (size == sizeof(config)) {
    prefs.getBytes("config", &config, sizeof(config));
  }
  prefs.end();

  if (!config.valid) {
    memset(&config, 0, sizeof(config));
    strncpy(config.backendHost, "ocpp.example.com", sizeof(config.backendHost) - 1);
    config.backendPort = 9000;
  }
}

void startAccessPoint() {
  Serial.println(F("Starting fallback access point"));
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  apMode = true;
  Serial.print(F("AP IP: "));
  Serial.println(WiFi.softAPIP());
}

void connectWiFi() {
  if (strlen(config.ssid) == 0 || strlen(config.password) == 0) {
    startAccessPoint();
    return;
  }

  Serial.print(F("Connecting to WiFi: "));
  Serial.println(config.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi connection failed, enabling AP"));
    startAccessPoint();
  } else {
    apMode = false;
    Serial.print(F("WiFi connected, IP: "));
    Serial.println(WiFi.localIP());
  }
}

String htmlForm() {
  String page;
  page += F("<!DOCTYPE html><html><head><meta charset='utf-8'><title>OCPP Proxy</title></head><body>");
  page += F("<h2>Atom S3 Lite OCPP Proxy</h2>");
  page += F("<form method='POST' action='/save'>");
  page += F("<label>WiFi SSID: <input name='ssid' value='");
  page += config.ssid;
  page += F("'></label><br>");
  page += F("<label>WiFi Password: <input type='password' name='password' value='");
  page += config.password;
  page += F("'></label><br>");
  page += F("<label>Backend Host: <input name='backend' value='");
  page += config.backendHost;
  page += F("'></label><br>");
  page += F("<label>Backend Port: <input name='port' type='number' value='");
  page += String(config.backendPort);
  page += F("'></label><br><button type='submit'>Save & Restart</button></form>");
  page += F("<p>Device IP: ");
  page += apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  page += F("</p></body></html>");
  return page;
}

void handleRoot() { httpServer.send(200, "text/html", htmlForm()); }

void handleSave() {
  if (httpServer.hasArg("ssid")) {
    memset(&config, 0, sizeof(config));
    strncpy(config.ssid, httpServer.arg("ssid").c_str(), sizeof(config.ssid) - 1);
    strncpy(config.password, httpServer.arg("password").c_str(), sizeof(config.password) - 1);
    strncpy(config.backendHost, httpServer.arg("backend").c_str(), sizeof(config.backendHost) - 1);
    config.backendPort = httpServer.arg("port").toInt();
    if (config.backendPort == 0) config.backendPort = 9000;
    config.valid = strlen(config.ssid) > 0;
    saveConfig();
    httpServer.send(200, "text/html", F("<p>Saved. Restarting WiFi...</p>"));
    delay(200);
    connectWiFi();
  } else {
    httpServer.send(400, "text/plain", "Missing parameters");
  }
}

void onBackendMessage(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_TEXT) {
    wallboxServer.broadcastTXT(payload, length);
  }
}

void ensureBackendConnected() {
  if (!config.valid) return;
  if (backendClient.isConnected()) return;
  if (millis() - lastBackendAttempt < 5000) return;

  lastBackendAttempt = millis();
  Serial.print(F("Connecting to backend ws://"));
  Serial.print(config.backendHost);
  Serial.print(':');
  Serial.println(config.backendPort);

  backendClient.begin(config.backendHost, config.backendPort, "/");
  backendClient.onEvent(onBackendMessage);
  backendClient.setReconnectInterval(5000);
}

uint8_t activeClientCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
    if (wallboxActive[i]) count++;
  }
  return count;
}

void wallboxEvent(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      if (activeClientCount() >= MAX_WALLBOX_CLIENTS) {
        wallboxServer.disconnect(client);
        Serial.println(F("Rejected wallbox: limit reached"));
        return;
      }
      wallboxActive[client % MAX_WALLBOX_CLIENTS] = true;
      Serial.print(F("Wallbox connected: #"));
      Serial.println(client);
      ensureBackendConnected();
      break;
    }
    case WStype_DISCONNECTED:
      wallboxActive[client % MAX_WALLBOX_CLIENTS] = false;
      Serial.print(F("Wallbox disconnected: #"));
      Serial.println(client);
      break;
    case WStype_TEXT:
      if (backendClient.isConnected()) {
        backendClient.sendTXT(payload, length);
      }
      break;
    default:
      break;
  }
}

void setupHttpServer() {
  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/save", HTTP_POST, handleSave);
  httpServer.begin();
  Serial.println(F("HTTP server started on port 80"));
}

void setup() {
  Serial.begin(115200);
  loadConfig();
  connectWiFi();
  setupHttpServer();

  wallboxServer.begin();
  wallboxServer.onEvent(wallboxEvent);
  Serial.println(F("Wallbox WebSocket server on port 7020"));
}

void loop() {
  if (WiFi.status() != WL_CONNECTED && !apMode) {
    connectWiFi();
  }

  ensureBackendConnected();

  httpServer.handleClient();
  wallboxServer.loop();
  backendClient.loop();
}

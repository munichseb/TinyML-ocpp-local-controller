/*
 * Nicla OCPP 1.6 Edge Gateway (Simplified TCP Version)
 *
 * This sketch turns a Nicla Vision board into a local OCPP gateway that
 * accepts multiple TCP connections from wallboxes on the LAN and performs
 * basic WebSocket handshaking before forwarding messages to a backend.
 * 
 * Due to library limitations on mbed_nicla, this implements a minimal
 * WebSocket server manually using TCP sockets.
 */

#include <Arduino.h>
#include <WiFi.h>

// Detect availability of the Mbed FlashIAP + TDBStore stack
#if defined(HAS_MBED_FLASH)
#elif defined(MBED_CONF_STORAGE_TDB_INTERNAL) && (MBED_CONF_STORAGE_TDB_INTERNAL == 0)
#define HAS_MBED_FLASH 0
#elif __has_include(<FlashIAPBlockDevice.h>) && __has_include(<TDBStore.h>) && __has_include(<FlashIAP.h>)
#define HAS_MBED_FLASH 1
#else
#define HAS_MBED_FLASH 0
#endif

#if HAS_MBED_FLASH
#include <FlashIAP.h>
#include <FlashIAPBlockDevice.h>
#include <TDBStore.h>
using namespace mbed;
#endif

// Maximum number of wallboxes that can be connected simultaneously
static const uint8_t MAX_WALLBOX_CLIENTS = 5;

// Default SSID/password for fallback access point
static const char AP_SSID[] = "NiclaGateway-Setup";
static const char AP_PASSWORD[] = "setup1234";

// WebSocket GUID for handshake
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Structure for configuration stored in flash
struct GatewayConfig {
  char ssid[32];
  char password[64];
  char backendHost[64];
  uint16_t backendPort;
  bool valid;
};

static GatewayConfig config;

// Persistent storage
static const size_t STORAGE_SIZE = 64 * 1024;
static const char *CONFIG_KEY = "gatewayConfig";

static bool kvReady = false;
static bool apMode = false;
#if HAS_MBED_FLASH
static FlashIAP flash;
static FlashIAPBlockDevice *kvBlock = nullptr;
static TDBStore *kvStore = nullptr;
#endif

// Structure to track wallbox connections
struct WallboxConnection {
  WiFiClient client;
  bool active;
  bool wsHandshakeDone;
  String buffer;
  unsigned long lastActivity;
};

// HTTP server for configuration
WiFiServer httpServer(80);

// TCP server for wallbox connections (will upgrade to WebSocket)
WiFiServer wallboxServer(8080);

// Array of wallbox connections
WallboxConnection wallboxClients[MAX_WALLBOX_CLIENTS];

// Backend connection
WiFiClient backendClient;
bool backendConnected = false;
bool backendWsHandshakeDone = false;
String backendBuffer;
unsigned long lastBackendAttempt = 0;

/**
 * Initialize Flash storage
 */
bool initStorage() {
#if !HAS_MBED_FLASH
  return false;
#else
  if (kvReady) return true;

  if (flash.init() != 0) {
    Serial.println(F("Flash init failed"));
    return false;
  }

  const uint32_t flashStart = flash.get_flash_start();
  const uint32_t flashSize = flash.get_flash_size();
  uint32_t storageStart = flashStart + flashSize - STORAGE_SIZE;
  const uint32_t sectorSize = flash.get_sector_size(storageStart);

  storageStart = (storageStart + sectorSize - 1) / sectorSize * sectorSize;
  const uint32_t blockDeviceSize = flashStart + flashSize - storageStart;

  kvBlock = new FlashIAPBlockDevice(storageStart, blockDeviceSize);
  kvStore = new TDBStore(kvBlock);
  int err = kvStore->init();
  if (err != MBED_SUCCESS) {
    Serial.print(F("KV init failed: "));
    Serial.println(err);
    return false;
  }

  kvReady = true;
  return true;
#endif
}

/**
 * Load configuration from flash
 */
void loadConfig() {
  if (!initStorage()) {
    Serial.println(F("Storage unavailable; using defaults"));
  } else {
#if HAS_MBED_FLASH
    size_t actualSize = 0;
    int err = kvStore->get(CONFIG_KEY, &config, sizeof(config), &actualSize);
    if (err != MBED_SUCCESS || actualSize != sizeof(config)) {
      Serial.println(F("No stored config found; using defaults"));
    }
#endif
  }

  if (!config.valid) {
    memset(&config, 0, sizeof(config));
    strncpy(config.ssid, AP_SSID, sizeof(config.ssid) - 1);
    strncpy(config.password, AP_PASSWORD, sizeof(config.password) - 1);
    strncpy(config.backendHost, "ocpp.example.com", sizeof(config.backendHost) - 1);
    config.backendPort = 9000;
    config.valid = false;
  }
}

/**
 * Save configuration to flash
 */
void saveConfig() {
  config.valid = true;
  if (!initStorage()) {
    Serial.println(F("Storage unavailable; config not saved"));
    return;
  }

#if HAS_MBED_FLASH
  int err = kvStore->set(CONFIG_KEY, &config, sizeof(config), 0);
  if (err != MBED_SUCCESS) {
    Serial.print(F("Failed to save config: "));
    Serial.println(err);
  }
#endif
}

/**
 * Start the fallback access point
 */
void startAccessPoint() {
  Serial.println(F("Starting fallback access point..."));
  WiFi.beginAP(AP_SSID, AP_PASSWORD);
  apMode = true;
  Serial.print(F("Access point started: IP address="));
  Serial.println(WiFi.localIP());
}

/**
 * Attempt to connect to WiFi
 */
void startWiFi() {
  if (!config.valid) {
    startAccessPoint();
    return;
  }

  Serial.print(F("Connecting to WiFi SSID: ")); 
  Serial.println(config.ssid);
  WiFi.begin(config.ssid, config.password);
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("Connected! IP address: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("WiFi connection failed, falling back to AP"));
    startAccessPoint();
  }
}

/**
 * URL decode helper
 */
String urlDecode(String str) {
  String decoded = "";
  for (size_t i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%' && i + 2 < str.length()) {
      String hex = str.substring(i + 1, i + 3);
      decoded += (char)strtol(hex.c_str(), NULL, 16);
      i += 2;
    } else {
      decoded += c;
    }
  }
  return decoded;
}

/**
 * Generate HTML dashboard
 */
String generateDashboard() {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  html += F("<title>Nicla OCPP Gateway</title>");
  html += F("<style>body{font-family:Arial,sans-serif;margin:40px;background:#f5f5f5;}");
  html += F(".container{background:white;padding:30px;border-radius:8px;max-width:600px;margin:0 auto;box-shadow:0 2px 4px rgba(0,0,0,0.1);}");
  html += F("h2{color:#333;margin-top:0;}");
  html += F(".status{background:#e8f5e9;padding:15px;border-radius:4px;margin:20px 0;}");
  html += F(".status.ap{background:#fff3e0;}");
  html += F("label{display:block;margin:15px 0 5px;font-weight:bold;color:#555;}");
  html += F("input[type=text],input[type=password],input[type=number]{width:100%;padding:10px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;font-size:14px;}");
  html += F("input[type=submit]{background:#2196F3;color:white;padding:12px 30px;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin-top:20px;}");
  html += F("input[type=submit]:hover{background:#1976D2;}</style></head><body>");
  html += F("<div class='container'><h2>🔌 OCPP Gateway Configuration</h2>");
  
  // Status display
  if (WiFi.status() == WL_CONNECTED) {
    html += F("<div class='status'><strong>📡 WiFi Status:</strong> Connected<br>");
    html += F("<strong>IP Address:</strong> ");
    html += WiFi.localIP().toString();
    html += F("</div>");
  } else if (apMode) {
    html += F("<div class='status ap'><strong>📡 WiFi Status:</strong> Access Point Mode<br>");
    html += F("<strong>SSID:</strong> ");
    html += AP_SSID;
    html += F("<br><strong>IP:</strong> ");
    html += WiFi.localIP().toString();
    html += F("</div>");
  } else {
    html += F("<div class='status' style='background:#ffebee;'><strong>📡 WiFi Status:</strong> Disconnected</div>");
  }
  
  // Configuration form
  html += F("<form action='/save' method='post'>");
  html += F("<h3>WiFi Settings</h3>");
  html += F("<label>SSID</label><input type='text' name='ssid' value='");
  html += config.ssid;
  html += F("' required>");
  html += F("<label>Password</label><input type='password' name='password' value='");
  html += config.password;
  html += F("'>");
  html += F("<h3>Backend Settings</h3>");
  html += F("<label>Host</label><input type='text' name='backendHost' value='");
  html += config.backendHost;
  html += F("' required>");
  html += F("<label>Port</label><input type='number' name='backendPort' value='");
  html += String(config.backendPort);
  html += F("' min='1' max='65535' required>");
  html += F("<input type='submit' value='💾 Save & Reboot'>");
  html += F("</form></div></body></html>");
  
  return html;
}

/**
 * Handle HTTP requests
 */
void handleHttpClient() {
  WiFiClient client = httpServer.available();
  if (!client) return;
  
  String request = "";
  String currentLine = "";
  bool isPost = false;
  int contentLength = 0;
  
  // Read headers
  unsigned long timeout = millis();
  while (client.connected() && millis() - timeout < 2000) {
    if (client.available()) {
      char c = client.read();
      request += c;
      
      if (c == '\n') {
        if (currentLine.length() == 0) break;
        
        if (currentLine.startsWith("POST")) isPost = true;
        if (currentLine.startsWith("Content-Length: ")) {
          contentLength = currentLine.substring(16).toInt();
        }
        currentLine = "";
      } else if (c != '\r') {
        currentLine += c;
      }
    }
  }
  
  // Read POST body
  String postBody = "";
  if (isPost && contentLength > 0) {
    timeout = millis();
    while (postBody.length() < (size_t)contentLength && millis() - timeout < 2000) {
      if (client.available()) {
        postBody += (char)client.read();
      }
    }
  }
  
  // Route request
  if (request.indexOf("GET / ") >= 0) {
    String page = generateDashboard();
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.print(page);
  } 
  else if (request.indexOf("POST /save") >= 0 && postBody.length() > 0) {
    // Parse form data
    String newSsid, newPassword, newBackendHost, newBackendPort;
    
    int start = 0;
    while (start < (int)postBody.length()) {
      int ampPos = postBody.indexOf('&', start);
      int end = (ampPos > 0) ? ampPos : postBody.length();
      String pair = postBody.substring(start, end);
      int eqPos = pair.indexOf('=');
      
      if (eqPos > 0) {
        String name = urlDecode(pair.substring(0, eqPos));
        String value = urlDecode(pair.substring(eqPos + 1));
        
        if (name == "ssid") newSsid = value;
        else if (name == "password") newPassword = value;
        else if (name == "backendHost") newBackendHost = value;
        else if (name == "backendPort") newBackendPort = value;
      }
      start = end + 1;
    }
    
    if (newSsid.length() > 0) {
      memset(&config, 0, sizeof(config));
      strncpy(config.ssid, newSsid.c_str(), sizeof(config.ssid) - 1);
      strncpy(config.password, newPassword.c_str(), sizeof(config.password) - 1);
      strncpy(config.backendHost, newBackendHost.c_str(), sizeof(config.backendHost) - 1);
      config.backendPort = (uint16_t)newBackendPort.toInt();
      saveConfig();
      
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: text/html");
      client.println("Connection: close");
      client.println();
      client.println("<html><body><h2>✅ Configuration saved!</h2><p>Device rebooting...</p></body></html>");
      client.flush();
      delay(1000);
      NVIC_SystemReset();
    } else {
      client.println("HTTP/1.1 400 Bad Request");
      client.println("Connection: close");
      client.println();
    }
  }
  else {
    client.println("HTTP/1.1 404 Not Found");
    client.println("Connection: close");
    client.println();
  }
  
  client.stop();
}

/**
 * Find free wallbox slot
 */
int8_t getFreeWallboxSlot() {
  for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
    if (!wallboxClients[i].active) return i;
  }
  return -1;
}

/**
 * Accept new wallbox connections
 */
void acceptWallboxConnections() {
  WiFiClient newClient = wallboxServer.available();
  if (!newClient) return;
  
  int8_t slot = getFreeWallboxSlot();
  if (slot >= 0) {
    wallboxClients[slot].client = newClient;
    wallboxClients[slot].active = true;
    wallboxClients[slot].wsHandshakeDone = false;
    wallboxClients[slot].buffer = "";
    wallboxClients[slot].lastActivity = millis();
    Serial.print(F("Wallbox connected on slot "));
    Serial.println(slot);
  } else {
    newClient.stop();
    Serial.println(F("Rejected wallbox (no free slots)"));
  }
}

/**
 * Simple WebSocket frame decoder (TEXT frames only)
 */
String decodeWebSocketFrame(String& buffer) {
  if (buffer.length() < 2) return "";
  
  uint8_t byte1 = buffer[0];
  uint8_t byte2 = buffer[1];
  
  bool fin = (byte1 & 0x80) != 0;
  uint8_t opcode = byte1 & 0x0F;
  bool masked = (byte2 & 0x80) != 0;
  uint64_t payloadLen = byte2 & 0x7F;
  
  size_t pos = 2;
  
  // Handle extended payload length
  if (payloadLen == 126) {
    if (buffer.length() < 4) return "";
    payloadLen = ((uint8_t)buffer[2] << 8) | (uint8_t)buffer[3];
    pos = 4;
  } else if (payloadLen == 127) {
    return ""; // We don't support 64-bit length
  }
  
  // Get masking key if present
  uint8_t mask[4] = {0};
  if (masked) {
    if (buffer.length() < pos + 4) return "";
    for (int i = 0; i < 4; i++) {
      mask[i] = buffer[pos + i];
    }
    pos += 4;
  }
  
  // Check if we have complete payload
  if (buffer.length() < pos + payloadLen) return "";
  
  // Extract and unmask payload
  String payload = "";
  for (size_t i = 0; i < payloadLen; i++) {
    uint8_t byte = buffer[pos + i];
    if (masked) byte ^= mask[i % 4];
    payload += (char)byte;
  }
  
  // Remove processed frame from buffer
  buffer = buffer.substring(pos + payloadLen);
  
  return (opcode == 0x01 && fin) ? payload : ""; // TEXT frame
}

/**
 * Encode WebSocket TEXT frame
 */
String encodeWebSocketFrame(const String& payload) {
  String frame = "";
  frame += (char)0x81; // FIN + TEXT opcode
  
  size_t len = payload.length();
  if (len < 126) {
    frame += (char)len;
  } else {
    frame += (char)126;
    frame += (char)(len >> 8);
    frame += (char)(len & 0xFF);
  }
  
  frame += payload;
  return frame;
}

/**
 * Handle wallbox clients
 */
void handleWallboxClients() {
  for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
    if (!wallboxClients[i].active) continue;
    
    WiFiClient& client = wallboxClients[i].client;
    
    if (!client.connected()) {
      client.stop();
      wallboxClients[i].active = false;
      Serial.print(F("Wallbox disconnected from slot "));
      Serial.println(i);
      continue;
    }
    
    // Read available data
    while (client.available()) {
      wallboxClients[i].buffer += (char)client.read();
      wallboxClients[i].lastActivity = millis();
    }
    
    // Handle WebSocket handshake
    if (!wallboxClients[i].wsHandshakeDone) {
      if (wallboxClients[i].buffer.indexOf("\r\n\r\n") > 0) {
        // Simple handshake response (real implementation would parse Sec-WebSocket-Key)
        String response = "HTTP/1.1 101 Switching Protocols\r\n";
        response += "Upgrade: websocket\r\n";
        response += "Connection: Upgrade\r\n";
        response += "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
        client.print(response);
        wallboxClients[i].wsHandshakeDone = true;
        wallboxClients[i].buffer = "";
        Serial.print(F("WebSocket handshake completed for slot "));
        Serial.println(i);
      }
      continue;
    }
    
    // Decode WebSocket frames
    String message = decodeWebSocketFrame(wallboxClients[i].buffer);
    if (message.length() > 0) {
      Serial.print(F("Wallbox -> Backend: "));
      Serial.println(message);
      
      // Forward to backend if connected
      if (backendConnected && backendWsHandshakeDone) {
        String frame = encodeWebSocketFrame(message);
        backendClient.print(frame);
      }
    }
  }
}

/**
 * Connect to backend
 */
void connectBackend() {
  if (backendConnected) return;
  
  unsigned long now = millis();
  if (now - lastBackendAttempt < 5000) return;
  lastBackendAttempt = now;
  
  Serial.print(F("Connecting to backend: "));
  Serial.print(config.backendHost);
  Serial.print(":");
  Serial.println(config.backendPort);
  
  if (!backendClient.connect(config.backendHost, config.backendPort)) {
    Serial.println(F("Backend connection failed"));
    return;
  }
  
  backendConnected = true;
  backendWsHandshakeDone = false;
  backendBuffer = "";
  
  // Send WebSocket handshake
  String handshake = "GET / HTTP/1.1\r\n";
  handshake += "Host: ";
  handshake += config.backendHost;
  handshake += "\r\n";
  handshake += "Upgrade: websocket\r\n";
  handshake += "Connection: Upgrade\r\n";
  handshake += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
  handshake += "Sec-WebSocket-Version: 13\r\n\r\n";
  
  backendClient.print(handshake);
  Serial.println(F("Backend WebSocket handshake sent"));
}

/**
 * Handle backend connection
 */
void handleBackend() {
  if (!backendConnected) {
    connectBackend();
    return;
  }
  
  if (!backendClient.connected()) {
    backendClient.stop();
    backendConnected = false;
    backendWsHandshakeDone = false;
    Serial.println(F("Backend disconnected"));
    return;
  }
  
  // Read data from backend
  while (backendClient.available()) {
    backendBuffer += (char)backendClient.read();
  }
  
  // Handle handshake response
  if (!backendWsHandshakeDone) {
    if (backendBuffer.indexOf("\r\n\r\n") > 0) {
      if (backendBuffer.indexOf("101") > 0) {
        backendWsHandshakeDone = true;
        // Remove handshake from buffer
        int pos = backendBuffer.indexOf("\r\n\r\n");
        backendBuffer = backendBuffer.substring(pos + 4);
        Serial.println(F("Backend WebSocket handshake completed"));
      } else {
        Serial.println(F("Backend handshake failed"));
        backendClient.stop();
        backendConnected = false;
      }
    }
    return;
  }
  
  // Decode messages from backend
  String message = decodeWebSocketFrame(backendBuffer);
  if (message.length() > 0) {
    Serial.print(F("Backend -> Wallboxes: "));
    Serial.println(message);
    
    // Broadcast to all connected wallboxes
    String frame = encodeWebSocketFrame(message);
    for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
      if (wallboxClients[i].active && wallboxClients[i].wsHandshakeDone) {
        wallboxClients[i].client.print(frame);
      }
    }
  }
}

/**
 * Setup
 */
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println(F("\n=== Nicla OCPP Gateway ==="));
  
  // Initialize wallbox array
  for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
    wallboxClients[i].active = false;
  }
  
  loadConfig();
  startWiFi();
  
  httpServer.begin();
  Serial.println(F("HTTP server started on port 80"));
  
  wallboxServer.begin();
  Serial.println(F("Wallbox server started on port 8080"));
  
  Serial.print(F("\n📱 Connect to: http://"));
  Serial.println(WiFi.localIP());
  Serial.println(F("=====================================\n"));
}

/**
 * Loop
 */
void loop() {
  handleHttpClient();
  acceptWallboxConnections();
  handleWallboxClients();
  handleBackend();
  delay(1);
}

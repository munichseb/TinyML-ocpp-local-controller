/*
 * Nicla OCPP 1.6 Edge Gateway
 *
 * This sketch turns a Nicla Vision board into a local OCPP gateway that
 * accepts multiple WebSocket connections from wallboxes on the LAN and
 * forwards all messages to a configurable backend.  Responses from the
 * backend are broadcast back to every connected wallbox.  A simple web
 * dashboard served on port 80 allows the user to configure Wi‑Fi and
 * backend connection parameters.  If the configured Wi‑Fi cannot be
 * reached, the device will fall back to an access point mode with
 * SSID “NiclaGateway‑Setup” and password “setup1234”.
 *
 * Important implementation details:
 *  - Uses the WebSockets2_Generic library to handle multiple server
 *    clients and a single client connection to the backend.  The
 *    examples for Teensy boards show how to accept new clients via
 *    server.poll() and server.accept() and how to store them in an array
 *    for later polling【267196380083254†L174-L205】.
 *  - Broadcasting messages from the backend to every connected wallbox is
 *    supported by the websockets library; you simply iterate over the
 *    connected clients and call send()【522192342060697†L28-L33】.
 *  - The fallback access point is started using WiFi.beginAP(), as
 *    documented in the WiFiNINA library【393347963076899†L114-L168】.
 */

#include <Arduino.h>

// Ensure WebSockets2_Generic selects the WiFiNINA-backed network stack
// on mbed-enabled Nicla boards.
#define WEBSOCKETS_NETWORK_TYPE NETWORK_WIFI

#include <WiFiNINA.h>
#include <WebSockets2_Generic.h>
#include <WiFiWebServer.h>

// Some versions of the WebSockets2_Generic library fail to define
// WSDefaultTcpServer on Nicla Vision builds, which leads to a
// "expected type-specifier" error when constructing the WebSockets
// server.  Fall back to the WiFiNINA-backed TcpServer implementation
// if the macro is missing after including the library headers.
#ifndef WSDefaultTcpServer
#define WSDefaultTcpServer websockets2_generic::network2_generic::WiFiNINATcpServer
#endif

// Detect availability of the Mbed FlashIAP + TDBStore stack.  Boards or cores
// that do not ship these headers will automatically disable flash-backed
// storage so the sketch can still compile.  Some platforms ship the headers
// but explicitly disable the TDB internal storage via the
// MBED_CONF_STORAGE_TDB_INTERNAL flag; treat that as unavailable to avoid
// referencing undefined types.
#if defined(HAS_MBED_FLASH)
// Honor a user-provided definition.
#elif defined(MBED_CONF_STORAGE_TDB_INTERNAL) && (MBED_CONF_STORAGE_TDB_INTERNAL == 0)
#define HAS_MBED_FLASH 0
#elif __has_include(<FlashIAPBlockDevice.h>) && __has_include(<TDBStore.h>)
#define HAS_MBED_FLASH 1
#else
#define HAS_MBED_FLASH 0
#endif

#if HAS_MBED_FLASH
#include <FlashIAPBlockDevice.h>
#include <TDBStore.h>

// Some Arduino Mbed cores expose the FlashIAP and storage classes in the
// global namespace instead of mbed::.  Pulling the mbed namespace into scope
// lets the sketch compile in either layout without sprinkling namespace
// conditionals everywhere.
using namespace mbed;
#endif

using namespace websockets2_generic;

// Maximum number of wallboxes (WebSocket clients) that can be
// connected simultaneously.
static const uint8_t MAX_WALLBOX_CLIENTS = 5;

// Default SSID/password for fallback access point
static const char AP_SSID[] = "NiclaGateway-Setup";
static const char AP_PASSWORD[] = "setup1234";

// Structure for configuration stored in flash via Mbed TDBStore.  The
// valid flag is checked on boot to decide whether to use saved
// credentials or fall back to the setup AP.
struct GatewayConfig {
  char ssid[32];           // Wi‑Fi SSID
  char password[64];       // Wi‑Fi password
  char backendHost[64];    // OCPP backend host
  uint16_t backendPort;    // OCPP backend port
  bool valid;              // flag indicating that the data is valid
};

static GatewayConfig config;

// Persistent storage using Mbed Flash + TDBStore.  A 64 kB slice of the
// internal flash near the end of the address space is reserved for the
// key/value store.
static const size_t STORAGE_SIZE = 64 * 1024;
static const char *CONFIG_KEY = "gatewayConfig";

static bool kvReady = false;
static bool apMode = false;
#if HAS_MBED_FLASH
static FlashIAP flash;
static FlashIAPBlockDevice *kvBlock = nullptr;
static TDBStore *kvStore = nullptr;
#endif

/**
 * Initialise the FlashIAPBlockDevice and TDBStore.  A region of flash
 * at the end of the device is aligned to the sector size and used by
 * the key/value store.  Returns true on success.
 */
bool initStorage() {
#if !HAS_MBED_FLASH
  // Storage not available on non-Mbed builds; treat as disabled.
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

  // Align start to sector boundary required by FlashIAPBlockDevice
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

// HTTP server on port 80 for configuration dashboard
WiFiWebServer httpServer(80);

// WebSockets server on port 8080 for wallboxes.  Recent versions of the
// WebSockets2_Generic library require explicitly passing a TcpServer
// instance to the constructor, so build one using the default network
// implementation selected via WEBSOCKETS_NETWORK_TYPE.
WebsocketsServer wsServer(new WSDefaultTcpServer());

// Array of client connections for wallboxes
WebsocketsClient wallboxClients[MAX_WALLBOX_CLIENTS];

// WebSocket client for the backend connection
WebsocketsClient backendClient;

// Timestamp for throttling backend reconnection attempts
unsigned long lastBackendAttempt = 0;

/**
 * Load configuration from flash (TDBStore).  If the valid flag is not
 * set or storage cannot be initialised, default values are used and the
 * valid flag remains false.  Defaults are AP credentials and a dummy
 * backend server to avoid accidental connections.
 */
void loadConfig() {
  if (!initStorage()) {
    Serial.println(F("Storage unavailable; using defaults"));
  } else {
    size_t actualSize = 0;
    int err = kvStore->get(CONFIG_KEY, &config, sizeof(config), &actualSize);
    if (err != MBED_SUCCESS || actualSize != sizeof(config)) {
      Serial.println(F("No stored config found; using defaults"));
    }
  }

  if (!config.valid) {
    // Populate default values
    memset(&config, 0, sizeof(config));
    strncpy(config.ssid, AP_SSID, sizeof(config.ssid) - 1);
    strncpy(config.password, AP_PASSWORD, sizeof(config.password) - 1);
    strncpy(config.backendHost, "ocpp.example.com", sizeof(config.backendHost) - 1);
    config.backendPort = 9000;
    config.valid = false;
  }
}

/**
 * Save configuration to flash via TDBStore and mark it as valid.
 */
void saveConfig() {
  config.valid = true;
  if (!initStorage()) {
    Serial.println(F("Storage unavailable; config not saved"));
    return;
  }

  int err = kvStore->set(CONFIG_KEY, &config, sizeof(config), 0);
  if (err != MBED_SUCCESS) {
    Serial.print(F("Failed to save config: "));
    Serial.println(err);
  }
}

/**
 * Start the fallback access point.  This is called when Wi‑Fi
 * association fails.  A static IP is automatically assigned by the
 * WiFi.beginAP() implementation【393347963076899†L114-L168】.
 */
void startAccessPoint() {
  Serial.println(F("Starting fallback access point..."));
  int status = WiFi.beginAP(AP_SSID, AP_PASSWORD);
  if (status != WL_AP_LISTENING) {
    Serial.println(F("Failed to start AP"));
  } else {
    apMode = true;
    Serial.print(F("Access point started: IP address="));
    Serial.println(WiFi.localIP());
  }
}

/**
 * Attempt to connect to the configured Wi‑Fi network.  If no valid
 * credentials exist or the connection cannot be established within
 * 15 seconds, a fallback access point is started instead.
 */
void startWiFi() {
  if (!config.valid) {
    // No saved credentials; start AP directly
    startAccessPoint();
    return;
  }

  Serial.print(F("Connecting to WiFi SSID: ")); Serial.println(config.ssid);
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
 * Generate the HTML dashboard showing current status and allowing the
 * user to change configuration.  This function uses the WiFiWebServer
 * class to send a simple page with a form.  Changing settings causes
 * the device to reboot so that new values take effect.
 */
String generateDashboard() {
  String html = F("<html><head><title>Nicla OCPP Gateway</title></head><body>");
  html += F("<h2>OCPP Gateway Configuration</h2>");
  // Show Wi‑Fi status
  if (WiFi.status() == WL_CONNECTED) {
    html += F("<p>WiFi Status: Connected</p>");
    html += F("<p>IP Address: "); html += WiFi.localIP().toString(); html += F("</p>");
  } else if (apMode) {
    html += F("<p>WiFi Status: Access Point (Setup mode)</p>");
    html += F("<p>AP SSID: "); html += AP_SSID; html += F("</p>");
  } else {
    html += F("<p>WiFi Status: Disconnected</p>");
  }
  // Form to edit settings
  html += F("<form action=\"/save\" method=\"post\">");
  html += F("<h3>WiFi Settings</h3>");
  html += F("SSID: <input type=\"text\" name=\"ssid\" value=\"");
  html += config.ssid;
  html += F("\"><br>Password: <input type=\"password\" name=\"password\" value=\"");
  html += config.password;
  html += F("\"><br>");
  html += F("<h3>Backend Settings</h3>");
  html += F("Host: <input type=\"text\" name=\"backendHost\" value=\"");
  html += config.backendHost;
  html += F("\"><br>Port: <input type=\"number\" name=\"backendPort\" value=\"");
  html += String(config.backendPort);
  html += F("\"><br><br><input type=\"submit\" value=\"Save &amp; Reboot\"></form>");
  html += F("</body></html>");
  return html;
}

/**
 * HTTP handler for the root page.  It simply serves the dashboard
 * generated by generateDashboard().
 */
void handleRoot() {
  String page = generateDashboard();
  httpServer.send(200, "text/html", page);
}

/**
 * HTTP handler for saving configuration.  This expects form fields
 * named ssid, password, backendHost and backendPort.  After
 * updating the config and writing it to EEPROM the board is
 * restarted.  Note: calling NVIC_SystemReset() is available on
 * Cortex‑M chips; for other architectures you may need to use
 * ESP.restart() or similar.  Here we conditionally call the
 * appropriate function based on the platform macros.
 */
void handleSave() {
  if (httpServer.hasArg("ssid")) {
    String newSsid = httpServer.arg("ssid");
    String newPassword = httpServer.arg("password");
    String newBackendHost = httpServer.arg("backendHost");
    String newBackendPort = httpServer.arg("backendPort");
    // Copy values into config struct
    memset(&config, 0, sizeof(config));
    strncpy(config.ssid, newSsid.c_str(), sizeof(config.ssid) - 1);
    strncpy(config.password, newPassword.c_str(), sizeof(config.password) - 1);
    strncpy(config.backendHost, newBackendHost.c_str(), sizeof(config.backendHost) - 1);
    config.backendPort = (uint16_t) newBackendPort.toInt();
    saveConfig();
    httpServer.send(200, "text/plain", "Configuration saved. Rebooting...");
    delay(1000);
    // Reset the board to apply new settings
    #if defined(NVIC_SystemReset)
      NVIC_SystemReset();
    #elif defined(ESP8266) || defined(ESP32)
      ESP.restart();
    #endif
  } else {
    httpServer.send(400, "text/plain", "Missing parameters");
  }
}

/**
 * Find an available slot in the wallboxClients array.  Returns
 * the index of the free slot or -1 if none is available.
 */
int8_t getFreeClientIndex() {
  for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
    // WebsocketsClient::available() returns true if the client is
    // connected.  We treat a non‑available slot as free.
    if (!wallboxClients[i].available()) {
      return i;
    }
  }
  return -1;
}

/**
 * Callback invoked when a wallbox sends a WebSocket message.
 * We forward the message to the backend without modification.
 */
void onWallboxMessage(WebsocketsClient &client, WebsocketsMessage message) {
  if (!backendClient.available()) return;
  backendClient.send(message.data());
}

/**
 * Callback invoked when a wallbox connection event occurs (e.g. open or
 * close).  Currently unused but could be extended for logging.
 */
void onWallboxEvent(WebsocketsClient &client, WebsocketsEvent event, String data) {
  (void)client;
  (void)event;
  (void)data;
}

/**
 * Callback invoked when the backend sends a WebSocket message.
 * The message is broadcast to all connected wallbox clients.  The
 * WebSockets2_Generic library makes this straightforward – we simply
 * iterate over the client array and call send()【522192342060697†L28-L33】.
 */
void onBackendMessage(WebsocketsMessage message) {
  String payload = message.data();
  for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
    if (wallboxClients[i].available()) {
      wallboxClients[i].send(payload);
    }
  }
}

/**
 * Accept new wallbox connections if the server has pending clients.
 * The server.poll() and server.accept() pattern comes from the
 * WebSockets2_Generic examples【267196380083254†L174-L205】.  Once a
 * client is accepted we attach message and event callbacks and store
 * the client in the array.
 */
void listenForWallboxes() {
  if (wsServer.poll()) {
    int8_t idx = getFreeClientIndex();
    if (idx >= 0) {
      WebsocketsClient newClient = wsServer.accept();
      newClient.onMessage(onWallboxMessage);
      newClient.onEvent(onWallboxEvent);
      wallboxClients[idx] = newClient;
      Serial.print(F("Wallbox connected on slot ")); Serial.println(idx);
    } else {
      // No free slot; accept and immediately close to free resources
      WebsocketsClient tmp = wsServer.accept();
      tmp.close();
      Serial.println(F("Rejected wallbox connection (max clients reached)"));
    }
  }
}

/**
 * Attempt to connect to the backend if not already connected.  The
 * connection attempt is throttled to avoid spamming when the backend
 * is unreachable.  When connected the backendClient is set up with
 * the appropriate callbacks.
 */
void connectBackend() {
  if (backendClient.available()) return;
  unsigned long now = millis();
  if (now - lastBackendAttempt < 5000) return;
  lastBackendAttempt = now;
  Serial.print(F("Connecting to backend ws://")); Serial.print(config.backendHost);
  Serial.print(':'); Serial.println(config.backendPort);
  bool connected = backendClient.connect(config.backendHost, config.backendPort, "/");
  if (connected) {
    Serial.println(F("Backend connected"));
    backendClient.onMessage(onBackendMessage);
  } else {
    Serial.println(F("Backend connection failed"));
  }
}

/**
 * Arduino setup() function.  Initializes serial, flash-backed storage, loads
 * configuration, starts Wi‑Fi (station or AP), configures the HTTP
 * server routes and starts the WebSocket server for wallboxes.
 */
void setup() {
  Serial.begin(115200);
  while (!Serial) { /* wait for Serial */ }
  loadConfig();
  startWiFi();
  // Setup HTTP routes
  httpServer.on("/", handleRoot);
  httpServer.on("/save", HTTP_POST, handleSave);
  httpServer.begin();
  Serial.println(F("HTTP server started"));
  // Start WebSocket server for wallboxes on port 8080
  wsServer.listen(8080);
  Serial.println(F("WebSocket server listening on port 8080"));
}

/**
 * Arduino loop() function.  Handles HTTP requests, accepts new
 * wallbox connections, polls existing WebSocket clients and keeps
 * the backend connection alive.
 */
void loop() {
  // Handle any pending HTTP requests for configuration
  httpServer.handleClient();
  // Accept new wallbox connections if available
  listenForWallboxes();
  // Poll wallbox clients
  for (uint8_t i = 0; i < MAX_WALLBOX_CLIENTS; i++) {
    if (wallboxClients[i].available()) {
      wallboxClients[i].poll();
    }
  }
  // Connect to backend if not connected and poll existing backend
  if (backendClient.available()) {
    backendClient.poll();
  } else {
    connectBackend();
  }
  delay(10); // allow other tasks to run
}

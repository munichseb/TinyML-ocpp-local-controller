/*
  WiFi Web Server LED Blink

 A simple web server that lets you blink an LED via the web.
 This sketch will print the IP address of your WiFi module (once connected)
 to the Serial Monitor. From there, you can open that address in a web browser
 to turn on and off the LED on pin 9.

 If the IP address of your board is yourAddress:
 http://yourAddress/H turns the LED on
 http://yourAddress/L turns it off

 This example is written for a network using WPA encryption. For
 WEP or WPA, change the WiFi.begin() call accordingly.

 Circuit:
 * Board with WiFi
 * LED attached to pin 9

 created 25 Nov 2012
 by Tom Igoe
 */

#include <WiFi.h>
#include <stdlib.h>

#if defined(ARDUINO_ARCH_MBED)
#include <FlashIAPBlockDevice.h>
#include <TDBStore.h>
#include <FlashIAP.h>
#define HAS_MBED_FLASH 1
#else
#define HAS_MBED_FLASH 0
#endif

// Access point credentials used for configuration when STA login fails
static const char FALLBACK_AP_SSID[] = "NiclaVision-Setup";
static const char FALLBACK_AP_PASSWORD[] = "nicla1234"; // minimum 8 chars

// Simple WiFi credentials stored in flash
struct WifiConfig {
  char ssid[32];
  char password[64];
  bool valid;
};

static WifiConfig wifiConfig;

static WiFiServer server(80);
static bool apMode = false;

#if HAS_MBED_FLASH
static const size_t STORAGE_SIZE = 32 * 1024;
static const char *CONFIG_KEY = "wifiConfig";
static FlashIAP flash;
static FlashIAPBlockDevice *kvBlock = nullptr;
static TDBStore *kvStore = nullptr;
static bool kvReady = false;
#endif

// Utility to URL-decode simple form values (handles %xx and '+').
String urlDecode(const String &input) {
  String decoded;
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%' && i + 2 < input.length()) {
      char hex[3] = {input[i + 1], input[i + 2], '\0'};
      decoded += (char) strtol(hex, nullptr, 16);
      i += 2;
    } else {
      decoded += c;
    }
  }
  return decoded;
}

#if HAS_MBED_FLASH
bool initStorage() {
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
}

void loadWifiConfig() {
  memset(&wifiConfig, 0, sizeof(wifiConfig));
  if (!initStorage()) return;

  size_t actualSize = 0;
  int err = kvStore->get(CONFIG_KEY, &wifiConfig, sizeof(wifiConfig), &actualSize);
  if (err != MBED_SUCCESS || actualSize != sizeof(wifiConfig)) {
    memset(&wifiConfig, 0, sizeof(wifiConfig));
  }
}

void saveWifiConfig() {
  if (!initStorage()) return;
  wifiConfig.valid = true;
  int err = kvStore->set(CONFIG_KEY, &wifiConfig, sizeof(wifiConfig), 0);
  if (err != MBED_SUCCESS) {
    Serial.print(F("Failed to save config: "));
    Serial.println(err);
  }
}
#else
void loadWifiConfig() { memset(&wifiConfig, 0, sizeof(wifiConfig)); }
void saveWifiConfig() {}
#endif

bool connectToWifi() {
  if (!wifiConfig.valid) return false;

  Serial.print(F("Attempting WiFi connection to: "));
  Serial.println(wifiConfig.ssid);
  int status = WiFi.begin(wifiConfig.ssid, wifiConfig.password);
  unsigned long start = millis();
  while (status != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    status = WiFi.status();
    Serial.print('.');
  }
  Serial.println();
  return status == WL_CONNECTED;
}

void startAccessPoint() {
  Serial.println(F("Starting fallback access point..."));
  int status = WiFi.beginAP(FALLBACK_AP_SSID, FALLBACK_AP_PASSWORD);
  if (status != WL_AP_LISTENING) {
    Serial.println(F("Failed to start AP"));
  } else {
    Serial.print(F("Connect to WiFi network: "));
    Serial.println(FALLBACK_AP_SSID);
    Serial.print(F("IP address: "));
    Serial.println(WiFi.localIP());
    apMode = true;
  }
  server.begin();
}

void setup() {
  Serial.begin(9600);
  pinMode(LED_BUILTIN, OUTPUT);

  loadWifiConfig();

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println(F("Communication with WiFi module failed!"));
    while (true);
  }

  if (connectToWifi()) {
    server.begin();
    apMode = false;
    printWifiStatus();
  } else {
    Serial.println(F("WiFi connection failed, starting AP for configuration"));
    startAccessPoint();
  }
}


void loop() {
  WiFiClient client = server.accept();
  if (client) {
    Serial.println(F("new client"));
    String currentLine = "";
    String requestLine = "";
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        if (c == '\n') {
          if (requestLine.length() == 0 && currentLine.length() > 0) {
            requestLine = currentLine;
          }
          if (currentLine.length() == 0) {
            // end of headers, serve response
            handleRequest(client, requestLine);
            break;
          } else {
            currentLine = "";
          }
        } else if (c != '\r') {
          currentLine += c;
        }
      }
    }
    client.stop();
    Serial.println(F("client disconnected"));
  }
}

void printWifiStatus() {
  // print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your board's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print the received signal strength:
  long rssi = WiFi.RSSI();
  Serial.print("signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
  // print where to go in a browser:
  Serial.print("To see this page in action, open a browser to http://");
  Serial.println(ip);
}

String settingsPage() {
  String page = F("<!doctype html><html><head><title>Nicla Vision Setup</title></head><body>");
  page += F("<h2>Nicla Vision WiFi Setup</h2>");
  if (WiFi.status() == WL_CONNECTED) {
    page += F("<p>Status: Connected</p><p>IP: ");
    page += WiFi.localIP().toString();
    page += F("</p>");
  } else if (apMode) {
    page += F("<p>Status: Access Point mode (setup)</p>");
  } else {
    page += F("<p>Status: Disconnected</p>");
  }

  page += F("<form action=\"/save\" method=\"get\">");
  page += F("SSID: <input type=\"text\" name=\"ssid\" value=\"");
  page += wifiConfig.ssid;
  page += F("\" required><br>\n");
  page += F("Password: <input type=\"password\" name=\"password\" value=\"");
  page += wifiConfig.password;
  page += F("\" required><br>\n");
  page += F("<input type=\"submit\" value=\"Save &amp; Connect\"></form><hr>");
  page += F("<p>Use this page to configure WiFi credentials. When saved, the device will attempt to connect and store the details in flash.</p>");
  page += F("</body></html>");
  return page;
}

void handleSaveRequest(const String &query) {
  int ssidIndex = query.indexOf("ssid=");
  int passIndex = query.indexOf("password=");
  if (ssidIndex == -1 || passIndex == -1) return;

  int ssidEnd = query.indexOf('&', ssidIndex);
  String ssidPart = query.substring(ssidIndex + 5, ssidEnd == -1 ? query.length() : ssidEnd);
  String passPart = query.substring(passIndex + 9);

  String newSsid = urlDecode(ssidPart);
  String newPass = urlDecode(passPart);

  memset(&wifiConfig, 0, sizeof(wifiConfig));
  strncpy(wifiConfig.ssid, newSsid.c_str(), sizeof(wifiConfig.ssid) - 1);
  strncpy(wifiConfig.password, newPass.c_str(), sizeof(wifiConfig.password) - 1);
  wifiConfig.valid = true;
  saveWifiConfig();

  Serial.print(F("Saved new WiFi credentials for SSID: "));
  Serial.println(wifiConfig.ssid);
}

void handleRequest(WiFiClient &client, const String &requestLine) {
  String path = "/";
  if (requestLine.startsWith("GET ")) {
    int start = 4;
    int end = requestLine.indexOf(' ', start);
    if (end > start) {
      path = requestLine.substring(start, end);
    }
  }

  if (path.startsWith("/save")) {
    int q = path.indexOf('?');
    if (q != -1) {
      handleSaveRequest(path.substring(q + 1));
      // Try to connect with the new credentials
      if (connectToWifi()) {
        apMode = false;
        server.begin();
      } else if (!apMode) {
        startAccessPoint();
      }
    }
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: text/html"));
    client.println();
    client.print(settingsPage());
    return;
  }

  // Default page with LED controls and WiFi form
  if (path.endsWith("/H")) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (path.endsWith("/L")) {
    digitalWrite(LED_BUILTIN, LOW);
  }

  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html"));
  client.println();
  client.print(settingsPage());
  client.println(F("<hr>Click <a href=\"/H\">here</a> to turn the LED on.<br>"));
  client.println(F("Click <a href=\"/L\">here</a> to turn the LED off."));
}

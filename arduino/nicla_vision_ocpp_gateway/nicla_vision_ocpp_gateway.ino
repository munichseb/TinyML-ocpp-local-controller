#include <Arduino.h>
#include <SPI.h>

#include <WiFiNINA.h>

#include <tiny_websockets/network/tcp_server.hpp>

// The ArduinoWebsockets library only defines a WSDefaultTcpServer for ESP and
// Teensy targets. For the Nicla Vision we need to provide our own type so the
// default constructor in server.hpp compiles. Define the adapter before
// including ArduinoWebsockets so WSDefaultTcpServer refers to a complete type
// when server.hpp is parsed.
#define WSDefaultTcpServer WiFiNinaTcpServer

// WiFiClient on the Nicla Vision does not support setNoDelay(). The generic ESP
// client wrapper used by ArduinoWebsockets expects it, so add a no-op stub.
class WiFiClientWithNoDelay : public WiFiClient
{
public:
  WiFiClientWithNoDelay() = default;
  WiFiClientWithNoDelay(const WiFiClient &client) : WiFiClient(client) {}

  void setNoDelay(bool) {}
};

// The ArduinoWebsockets library provides GenericEspTcpClient implementations for
// ESP8266/ESP32 targets, but it does not ship an adapter for WiFiNINA-based
// boards like the Nicla Vision. Provide a minimal TcpClient wrapper around
// WiFiClient so the gateway can accept and initiate websocket connections
// without relying on headers that are unavailable on this platform.
class WiFiNinaTcpClient : public websockets::network::TcpClient
{
public:
  WiFiNinaTcpClient() = default;
  explicit WiFiNinaTcpClient(const WiFiClientWithNoDelay &client) : client(client) {}

  bool poll() override
  {
    yield();
    return client.connected();
  }

  bool available() override
  {
    yield();
    return client.connected();
  }

  void close() override
  {
    yield();
    client.stop();
  }

  bool connect(const WSString &host, int port) override
  {
    yield();
    return client.connect(host.c_str(), port);
  }

  void send(const WSString &data) override
  {
    yield();
    client.write(reinterpret_cast<const uint8_t *>(data.c_str()), data.length());
  }

  void send(const WSString &&data) override
  {
    send(data);
  }

  void send(const uint8_t *data, const uint32_t len) override
  {
    yield();
    client.write(data, len);
  }

  WSString readLine() override
  {
    yield();
    return client.readStringUntil('\n');
  }

  uint32_t read(uint8_t *buffer, const uint32_t len) override
  {
    yield();
    int result = client.read(buffer, len);
    return result < 0 ? 0 : static_cast<uint32_t>(result);
  }

protected:
  int getSocket() const override
  {
    return -1;
  }

private:
  WiFiClientWithNoDelay client;
};

// The ArduinoWebsockets library does not ship a default TcpServer implementation
// for the Nicla Vision (WiFiNINA) platform, so we provide a minimal adapter that
// wraps WiFiServer/WiFiClient.
class WiFiNinaTcpServer : public websockets::network::TcpServer
{
public:
  // WiFiServer does not provide a default constructor, so initialize with a
  // dummy port and reassign when listen() is called.
  WiFiNinaTcpServer() : server(0) {}

  bool poll() override
  {
    yield();
    return server.available();
  }

  bool listen(const uint16_t port) override
  {
    yield();
    server = WiFiServer(port);
    server.begin();
    return available();
  }

  websockets::network::TcpClient *accept() override
  {
    while (available())
    {
      auto client = server.available();
      if (client)
      {
        return new WiFiNinaTcpClient(WiFiClientWithNoDelay(client));
      }
    }
    return new WiFiNinaTcpClient();
  }

  bool available() override
  {
    yield();
    return static_cast<bool>(server);
  }

  void close() override
  {
    yield();
    server.end();
  }

  virtual ~WiFiNinaTcpServer()
  {
    if (available())
    {
      close();
    }
  }

protected:
  int getSocket() const override
  {
    return -1;
  }

private:
  WiFiServer server;
};

#include <ArduinoWebsockets.h>

using namespace websockets;

const char *defaultSsid = "FiberWAN";
const char *defaultPassword = "winter28";

// Configuration that can be changed via the web UI
String configuredSsid = defaultSsid;
String configuredPassword = defaultPassword;
String backendHost = "192.168.0.100";
int backendPort = 8081;

// Networking
WiFiServer httpServer(80);
WiFiNinaTcpServer ninaTcpServer;
WebsocketsServer wsServer(&ninaTcpServer);
WebsocketsClient backendSocket;

// Wallbox connections
static const size_t MAX_WALLBOXES = 10;
WebsocketsClient wallboxClients[MAX_WALLBOXES];
bool wallboxActive[MAX_WALLBOXES];

// SoftAP fallback
const char *setupApSsid = "NiclaGateway-Setup";
const char *setupApPassword = "setup1234";

void printWiFiStatus()
{
  Serial.print("Connected to SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

bool connectToWiFi(const String &ssid, const String &password)
{
  Serial.print("Connecting to WiFi SSID '");
  Serial.print(ssid);
  Serial.println("'...");

  int status = WL_IDLE_STATUS;
  unsigned long startAttemptTime = millis();
  while (status != WL_CONNECTED && millis() - startAttemptTime < 20000)
  {
    status = WiFi.begin(ssid.c_str(), password.c_str());
    delay(5000);
  }

  if (status == WL_CONNECTED)
  {
    Serial.println("WiFi connected!");
    printWiFiStatus();
    return true;
  }

  Serial.println("WiFi connection failed.");
  return false;
}

void startSetupHotspot()
{
  Serial.print("Starting setup hotspot '" + String(setupApSsid) + "'...");
  if (WiFi.beginAP(setupApSsid, setupApPassword))
  {
    Serial.println(" started.");
    printWiFiStatus();
  }
  else
  {
    Serial.println(" failed to start AP mode.");
  }
}

void ensureWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }

  if (!connectToWiFi(configuredSsid, configuredPassword))
  {
    startSetupHotspot();
  }
}

void resetWallboxSlots()
{
  for (size_t i = 0; i < MAX_WALLBOXES; i++)
  {
    wallboxActive[i] = false;
  }
}

size_t activeWallboxCount()
{
  size_t count = 0;
  for (size_t i = 0; i < MAX_WALLBOXES; i++)
  {
    if (wallboxActive[i])
    {
      count++;
    }
  }
  return count;
}

void ensureBackendConnected()
{
  if (backendHost.length() == 0)
  {
    return;
  }

  if (backendSocket.available())
  {
    return;
  }

  String backendUrl = "ws://" + backendHost + ":" + String(backendPort) + "/";
  Serial.print("Connecting to backend at ");
  Serial.println(backendUrl);
  backendSocket.onMessage([](WebsocketsMessage message) {
    // Broadcast backend messages to all wallboxes
    for (size_t i = 0; i < MAX_WALLBOXES; i++)
    {
      if (wallboxActive[i])
      {
        wallboxClients[i].send(message.data());
      }
    }
  });

  backendSocket.connect(backendUrl);
}

void forwardToBackend(const String &payload)
{
  ensureBackendConnected();
  if (backendSocket.available())
  {
    backendSocket.send(payload);
  }
}

void handleWallboxMessage(size_t index, WebsocketsMessage message)
{
  Serial.print("Wallbox message from slot ");
  Serial.print(index);
  Serial.print(": ");
  Serial.println(message.data());
  forwardToBackend(message.data());
}

void acceptWallboxConnections()
{
  auto client = wsServer.accept();
  if (client.available())
  {
    for (size_t i = 0; i < MAX_WALLBOXES; i++)
    {
      if (!wallboxActive[i])
      {
        wallboxClients[i] = client;
        wallboxActive[i] = true;
        wallboxClients[i].onMessage([i](WebsocketsMessage message) {
          handleWallboxMessage(i, message);
        });
        wallboxClients[i].onEvent([i](WebsocketsEvent event, String data) {
          if (event == WebsocketsEvent::ConnectionClosed)
          {
            wallboxActive[i] = false;
            Serial.print("Wallbox disconnected from slot ");
            Serial.println(i);
          }
        });
        Serial.print("Wallbox connected in slot ");
        Serial.println(i);
        return;
      }
    }

    // No free slot
    client.close();
  }
}

String htmlHeader()
{
  return "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>Nicla OCPP Gateway</title><style>body{font-family:Arial,sans-serif;margin:1rem;}label{display:block;margin-top:0.5rem;}input{width:100%;padding:0.4rem;margin-top:0.2rem;}button{margin-top:0.7rem;padding:0.5rem 0.8rem;}section{margin-bottom:1rem;padding:0.8rem;border:1px solid #ddd;border-radius:6px;}small{color:#555;}</style></head><body><h1>Nicla Vision OCPP Gateway</h1>";
}

String htmlFooter()
{
  return "<p><small>WiFi SSID: " + String(WiFi.SSID()) + " — IP: " + WiFi.localIP().toString() + "</small></p></body></html>";
}

void serveStatusPage(WiFiClient &client, const String &message = "")
{
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.print(htmlHeader());
  client.print("<section><h2>Status</h2><p>Verbundene Wallboxen: <strong>");
  client.print(activeWallboxCount());
  client.print(" / ");
  client.print(MAX_WALLBOXES);
  client.print("</strong></p>");
  if (message.length() > 0)
  {
    client.print("<p><strong>");
    client.print(message);
    client.print("</strong></p>");
  }
  client.print("</section>");

  client.print("<section><h2>Backend</h2><form method=\"GET\" action=\"/configure\"><label>Host/IP<input name=\"backendHost\" value=\"");
  client.print(backendHost);
  client.print("\"></label><label>Port<input name=\"backendPort\" type=\"number\" value=\"");
  client.print(backendPort);
  client.print("\"></label><button type=\"submit\">Speichern</button></form></section>");

  client.print("<section><h2>WLAN</h2><form method=\"GET\" action=\"/wifi\"><label>SSID<input name=\"ssid\" value=\"");
  client.print(configuredSsid);
  client.print("\"></label><label>Passwort<input name=\"password\" type=\"password\" value=\"");
  client.print(configuredPassword);
  client.print("\"></label><button type=\"submit\">WLAN aktualisieren</button></form><p><small>Standard SSID: FiberWAN / sommer17</small></p></section>");

  client.print(htmlFooter());
}

String getQueryParam(const String &query, const String &key)
{
  int start = query.indexOf(key + "=");
  if (start == -1)
  {
    return "";
  }
  start += key.length() + 1;
  int end = query.indexOf('&', start);
  if (end == -1)
  {
    end = query.length();
  }
  return query.substring(start, end);
}

void handleConfigureRequest(const String &query)
{
  String newHost = getQueryParam(query, "backendHost");
  String newPort = getQueryParam(query, "backendPort");
  if (newHost.length() > 0)
  {
    backendHost = newHost;
  }
  if (newPort.length() > 0)
  {
    backendPort = newPort.toInt();
  }
  Serial.print("Backend updated to ");
  Serial.print(backendHost);
  Serial.print(":");
  Serial.println(backendPort);
  backendSocket.close();
}

void handleWifiRequest(const String &query)
{
  String newSsid = getQueryParam(query, "ssid");
  String newPassword = getQueryParam(query, "password");
  if (newSsid.length() > 0)
  {
    configuredSsid = newSsid;
  }
  if (newPassword.length() > 0)
  {
    configuredPassword = newPassword;
  }
  Serial.print("Updating WiFi to SSID '");
  Serial.print(configuredSsid);
  Serial.println("'");
  WiFi.disconnect();
  ensureWiFi();
}

void handleHttpClient(WiFiClient &client)
{
  String requestLine = client.readStringUntil('\n');
  String method;
  String path;

  int firstSpace = requestLine.indexOf(' ');
  int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
  if (firstSpace != -1 && secondSpace != -1)
  {
    method = requestLine.substring(0, firstSpace);
    path = requestLine.substring(firstSpace + 1, secondSpace);
  }

  // Skip headers
  while (client.connected())
  {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0)
    {
      break;
    }
  }

  String message;
  if (path.startsWith("/configure"))
  {
    int queryStart = path.indexOf('?');
    if (queryStart != -1)
    {
      String query = path.substring(queryStart + 1);
      handleConfigureRequest(query);
      message = "Backend gespeichert.";
    }
  }
  else if (path.startsWith("/wifi"))
  {
    int queryStart = path.indexOf('?');
    if (queryStart != -1)
    {
      String query = path.substring(queryStart + 1);
      handleWifiRequest(query);
      message = "WLAN neu verbunden.";
    }
  }

  serveStatusPage(client, message);
}

void setup()
{
  Serial.begin(115200);
  while (!Serial)
  {
    ;
  }

  if (WiFi.status() == WL_NO_MODULE)
  {
    Serial.println("Communication with WiFi module failed!");
    while (true)
      ;
  }

  resetWallboxSlots();
  ensureWiFi();

  httpServer.begin();
  wsServer.listen(8080);
  Serial.println("HTTP server started on port 80");
  Serial.println("WebSocket server for Wallboxen on port 8080");
}

void loop()
{
  ensureWiFi();

  WiFiClient client = httpServer.available();
  if (client)
  {
    handleHttpClient(client);
    delay(1);
    client.stop();
  }

  acceptWallboxConnections();

  for (size_t i = 0; i < MAX_WALLBOXES; i++)
  {
    if (wallboxActive[i])
    {
      wallboxClients[i].poll();
    }
  }

  if (backendSocket.available())
  {
    backendSocket.poll();
  }
}

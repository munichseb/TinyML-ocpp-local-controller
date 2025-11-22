#include <Arduino.h>
#include <SPI.h>
#include <WiFiNINA.h>

// Wi-Fi credentials for the Nicla Vision board
const char *ssid = "FiberWAN";
const char *password = "sommer17";

WiFiServer server(80);

void printWiFiStatus()
{
  Serial.print("Connected to SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void waitForWiFi()
{
  Serial.print("Connecting to WiFi SSID '");
  Serial.print(ssid);
  Serial.println("'...");

  int status = WL_IDLE_STATUS;
  unsigned long startAttemptTime = millis();

  while (status != WL_CONNECTED)
  {
    status = WiFi.begin(ssid, password);

    // Avoid rapid retries
    delay(5000);

    if (millis() - startAttemptTime > 60000)
    {
      Serial.println("Retrying WiFi connection...");
      startAttemptTime = millis();
    }
  }

  Serial.println("WiFi connected!");
  printWiFiStatus();
}

void serveHelloWorld(WiFiClient &client)
{
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html>");
  client.println("<html lang=\"en\">");
  client.println("<head><meta charset=\"UTF-8\"><title>Nicla Vision Gateway</title></head>");
  client.println("<body><h1>Hello World</h1></body>");
  client.println("</html>");
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

  waitForWiFi();
  server.begin();
  Serial.println("HTTP server started on port 80");
}

void loop()
{
  WiFiClient client = server.available();

  if (client)
  {
    bool currentLineIsBlank = true;

    while (client.connected())
    {
      if (client.available())
      {
        char c = client.read();
        if (c == '\n' && currentLineIsBlank)
        {
          serveHelloWorld(client);
          break;
        }
        if (c == '\n')
        {
          currentLineIsBlank = true;
        }
        else if (c != '\r')
        {
          currentLineIsBlank = false;
        }
      }
    }

    delay(1);
    client.stop();
  }
}

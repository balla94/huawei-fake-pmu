#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ETH.h>
#include <WiFi.h>
#include <ArduinoOTA.h>

#define PROMPT_BUFFER_SIZE 256

// DEFINE HERE THE KNOWN NETWORKS
const char *KNOWN_SSID[] = {"P4IT", "szarvas", "clueQuest IoT", "Lufi"};
const char *KNOWN_PASSWORD[] = {"pappka22", "pappka22", "CQIOT8147QQQ","almakorte12"};

const int KNOWN_SSID_COUNT = sizeof(KNOWN_SSID) / sizeof(KNOWN_SSID[0]); // number of known networks

EspSoftwareSerial::UART bus;

WiFiServer telnetserver(23);
WiFiClient telnetclient;

void printTelnet(const char *message)
{
  if (telnetclient && telnetclient.connected())
  {

    while (*message)
    {
      char c = *message++;
      if (c == '\n')
      {
        telnetclient.write('\r');
      }
      telnetclient.write(c);
    }
  }
}

void printTelnetf(const char *fmt, ...)
{
  if (telnetclient && telnetclient.connected())
  {
    char buffer[256]; // Adjust size as needed
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    printTelnet(buffer);
  }
}

void WiFiEvent(WiFiEvent_t event)
{
  Serial.printf("[WiFi-event] event: %d\n", event);

  switch (event)
  {
  case ARDUINO_EVENT_WIFI_READY:
    Serial.println("WiFi interface ready");
    break;
  case ARDUINO_EVENT_WIFI_SCAN_DONE:
    Serial.println("Completed scan for access points");
    break;
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println("WiFi client started");
    break;
  case ARDUINO_EVENT_WIFI_STA_STOP:
    Serial.println("WiFi clients stopped");
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println("Connected to access point");
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    Serial.println("Disconnected from WiFi access point");
    break;
  case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
    Serial.println("Authentication mode of access point has changed");
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.print("Obtained IP address: ");
    Serial.println(WiFi.localIP());
    break;
  case ARDUINO_EVENT_WIFI_STA_LOST_IP:
    Serial.println("Lost IP address and IP address is reset to 0");
    break;
  case ARDUINO_EVENT_WPS_ER_SUCCESS:
    Serial.println("WiFi Protected Setup (WPS): succeeded in enrollee mode");
    break;
  case ARDUINO_EVENT_WPS_ER_FAILED:
    Serial.println("WiFi Protected Setup (WPS): failed in enrollee mode");
    break;
  case ARDUINO_EVENT_WPS_ER_TIMEOUT:
    Serial.println("WiFi Protected Setup (WPS): timeout in enrollee mode");
    break;
  case ARDUINO_EVENT_WPS_ER_PIN:
    Serial.println("WiFi Protected Setup (WPS): pin code in enrollee mode");
    break;
  case ARDUINO_EVENT_WIFI_AP_START:
    Serial.println("WiFi access point started");
    break;
  case ARDUINO_EVENT_WIFI_AP_STOP:
    Serial.println("WiFi access point  stopped");
    break;
  case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
    Serial.println("Client connected");
    break;
  case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
    Serial.println("Client disconnected");
    break;
  case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
    Serial.println("Assigned IP address to client");
    break;
  case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
    Serial.println("Received probe request");
    break;
  case ARDUINO_EVENT_WIFI_AP_GOT_IP6:
    Serial.println("AP IPv6 is preferred");
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
    Serial.println("STA IPv6 is preferred");
    break;
  case ARDUINO_EVENT_ETH_GOT_IP6:
    Serial.println("Ethernet IPv6 is preferred");
    break;
  case ARDUINO_EVENT_ETH_START:
    Serial.println("Ethernet started");
    break;
  case ARDUINO_EVENT_ETH_STOP:
    Serial.println("Ethernet stopped");
    break;
  case ARDUINO_EVENT_ETH_CONNECTED:
    Serial.println("Ethernet connected");
    break;
  case ARDUINO_EVENT_ETH_DISCONNECTED:
    Serial.println("Ethernet disconnected");
    break;
  case ARDUINO_EVENT_ETH_GOT_IP:
    Serial.println("Obtained IP address");
    Serial.println(ETH.localIP());
    break;
  default:
    break;
  }
}

void wifiConnect()
{
  Serial.println("Configuring WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("Scanning for networks...");
  WiFi.scanNetworks(true);
  while (WiFi.scanComplete() < 0)
  {
    delay(100); // Wait for scan to complete
  }

  int nbVisibleNetworks = WiFi.scanComplete();
  if (nbVisibleNetworks == 0)
  {
    Serial.println("No networks found. Retrying...");
    delay(5000);
    return;
  }

  Serial.printf("%d network(s) found\n", nbVisibleNetworks);

  // Variables to store the strongest network info
  int strongestRSSI = -100; // Initialize with a low RSSI value
  String strongestSSID;
  uint8_t strongestBSSID[6]; // Array to store BSSID (MAC address)
  const char *password = nullptr;

  // Find the strongest BSSID for any known SSID
  for (int i = 0; i < nbVisibleNetworks; ++i)
  {
    String currentSSID = WiFi.SSID(i);
    int currentRSSI = WiFi.RSSI(i);
    uint8_t *currentBSSID = WiFi.BSSID(i);

    // Check if the current SSID is in the known list
    for (int n = 0; n < KNOWN_SSID_COUNT; n++)
    {
      if (currentSSID == KNOWN_SSID[n])
      {
        Serial.printf("Found SSID: %s, RSSI: %d, BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      currentSSID.c_str(),
                      currentRSSI,
                      currentBSSID[0], currentBSSID[1], currentBSSID[2],
                      currentBSSID[3], currentBSSID[4], currentBSSID[5]);

        // Update if this is the strongest BSSID so far
        if (currentRSSI > strongestRSSI)
        {
          strongestRSSI = currentRSSI;
          strongestSSID = currentSSID;
          memcpy(strongestBSSID, currentBSSID, 6); // Save the strongest BSSID
          password = KNOWN_PASSWORD[n];            // Get the password for this SSID
        }
      }
    }
  }

  // If no known SSIDs were found, reset or retry
  if (strongestSSID.isEmpty())
  {
    Serial.println("No known networks found. Retrying...");
    delay(5000);
    return;
  }

  // Attempt to connect to the strongest BSSID
  Serial.printf("\nConnecting to SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X, RSSI: %d\n",
                strongestSSID.c_str(),
                strongestBSSID[0], strongestBSSID[1], strongestBSSID[2],
                strongestBSSID[3], strongestBSSID[4], strongestBSSID[5],
                strongestRSSI);

  WiFi.begin(strongestSSID.c_str(), password, 0, strongestBSSID);

  int timeout = 150; // 15 seconds timeout
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(100);
    Serial.print(".");
    if (--timeout == 0)
    {
      Serial.println("\nConnection timed out");
      return;
    }
  }

  // Successfully connected
  Serial.println("\nWiFi connected!");
  Serial.println("IP Address: ");
  Serial.println(WiFi.localIP());
}

void handleTelnetCommand(char *buf) {
   if (strcmp(buf, "reboot") == 0) {
    printTelnetf("Rebooting...\n");
    delay(300);  // Give time for the message to flush
    ESP.restart();  // Restart the ESP
  } 
  else if (strcmp(buf, "wifi") == 0) {
    printTelnetf("====== WIFI INFO ======= \n");
    printTelnetf("Connected to: %s\n",WiFi.SSID().c_str());
    printTelnetf("Signal: %d\n",WiFi.RSSI());
  }else {
    printTelnetf("Unknown command: %s\n", buf);
  }
}

void setup()
{
  delay(200);
  Serial.begin(115200);
  WiFi.onEvent(WiFiEvent);
  wifiConnect();
  telnetserver.begin();

  ArduinoOTA.begin();
  bus.begin(9600, SWSERIAL_8S1, 36, 4, false, 128, 128);

}

void telnetloop()
{
  static unsigned long lastTelnetInCheck = 0;
  static unsigned long lastTelnetOutCheck = 0;
  static char promptBuffer[PROMPT_BUFFER_SIZE];
  static uint16_t promptIndex = 0;

  // Handle new client connection
  if (telnetserver.hasClient())
  {
    WiFiClient newClient = telnetserver.available();
    if (newClient)
    {
      if (telnetclient && telnetclient.connected())
      {
        telnetclient.stop(); // Disconnect existing client
      }
      telnetclient = newClient;
      telnetclient.setNoDelay(true);
      printTelnetf("Telnet connected\n");
      promptIndex = 0;  // Reset buffer
    }
  }

  // Process client communication if connected
  if (telnetclient && telnetclient.connected())
  {
    if (millis() - lastTelnetInCheck > 1)
    {
      lastTelnetInCheck = millis();
      int c = telnetclient.read();
      if (c >= 0 && (uint8_t)c != 255)
      {
        // Handle carriage return or newline
        if (c == '\r' || c == '\n')
        {
          if (promptIndex > 0)
          {
            promptBuffer[promptIndex] = '\0';  // Null-terminate
            handleTelnetCommand(promptBuffer); // Call your command handler
            promptIndex = 0;                   // Reset buffer
          }
        }
        else if (promptIndex < PROMPT_BUFFER_SIZE - 1)
        {
          promptBuffer[promptIndex++] = (char)c;
        }
      }
    }
  }
}
void loop()
{
  telnetloop();
  ArduinoOTA.handle();
}

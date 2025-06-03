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

void handlePoll() {
  bus.flush();
  printTelnetf("Starting poll sequence...\n");


  // Step 1: Send 0xC0 with MARK parity
  bus.write(0xC0, EspSoftwareSerial::PARITY_MARK);

  // Step 2: Wait up to 5ms for 0x00 with SPACE parity
  unsigned long start = micros();
  while (!bus.available() && micros() - start < 5000) delayMicroseconds(10);

  if (!bus.available()) {
    printTelnetf("Timeout waiting for 0x00 response\n");
    return;
  }

  int response = bus.read();
  int parity = bus.readParity();
  if (response != 0x00 || parity) {
    printTelnetf("Invalid response: 0x%02X, parity: %s\n", response, parity ? "MARK" : "SPACE");
    return;
  }

  // Step 3: Wait precisely 1.4ms
  delayMicroseconds(1400);

  // Step 4: Send byte stream with SPACE parity
  uint8_t request[] = {0x00, 0x04, 0xC8, 0x24, 0xFF, 0xFF, 0xEE};
  for (size_t i = 0; i < sizeof(request); ++i) {
    bus.write(request[i], EspSoftwareSerial::PARITY_SPACE);
  }
  //printTelnetf("Sent request frame with SPACE parity\n");

  // Step 5: Wait up to 5ms for 0x7F response
  start = micros();
  while (!bus.available() && micros() - start < 8000) delayMicroseconds(10);

  if (!bus.available()) {
    printTelnetf("Timeout waiting for 0x7F ack\n");
    return;
  }

  response = bus.read();
  parity = bus.readParity();
  if (response != 0x7F || parity) {
    printTelnetf("Invalid ack: 0x%02X, parity: %s\n", response, parity ? "MARK" : "SPACE");
    return;
  }

  // Step 6: Wait 1.8ms
  delayMicroseconds(1800);

  // Step 7: Send 0x20 with MARK parity
  bus.write(0x20, EspSoftwareSerial::PARITY_MARK);

  // Step 8: Wait up to 3ms for the start of 13-byte reply
  start = micros();
  while (!bus.available() && micros() - start < 8000) delayMicroseconds(10);

  if (!bus.available()) {
    printTelnetf("Timeout waiting for 13-byte response\n");
    return;
  }

// Step 9: Read up to 64 bytes, timeout if no new byte in 1ms
uint8_t buffer[64];
size_t count = 0;
unsigned long lastByteTime = micros();

while (count < sizeof(buffer)) {
  if (bus.available()) {
    buffer[count++] = bus.read();
    bus.readParity();  // always consume parity after read
    lastByteTime = micros();  // reset timeout
  } else if (micros() - lastByteTime >= 2000) {
    break;  // idle for 1ms: end of message
  }
}


  

  // Step 10: Wait 1.5ms before sending 0x7F
  delayMicroseconds(120);
  bus.write(0x7F, EspSoftwareSerial::PARITY_SPACE);

  if (count == 0) {
  printTelnetf("No data received after 0x20\n");
  return;
}

  if (count < 13) {
    printTelnetf("Incomplete response: got %d bytes, expected 13\n", count);
    return;
  }

  if (count > 13) {
    printTelnetf("Invalid response: got %d bytes, expected 13\n", count);
    return;
  }

  printTelnetf("Received 13-byte data frame: ");
  for (size_t i = 0; i < 13; ++i) {
    printTelnetf("0x%02X ", buffer[i]);
  }
  printTelnetf("\n");

 uint16_t voltage = ((uint16_t)buffer[8] << 8) | buffer[9];
  uint16_t current = ((uint16_t)buffer[10] << 8) | buffer[11];
  printTelnetf("Voltage: %.2f V\n", voltage / 100.0);
  printTelnetf("Current: %.2f A\n", current / 100.0);

  printTelnetf("Poll done\n");
}

void sendFirstCommand() {
  bus.flush();
  printTelnetf("Sending first sequence...\n");


  // Step 1: Send 0xC0 with MARK parity
  bus.write(0xC0, EspSoftwareSerial::PARITY_MARK);

  // Step 2: Wait up to 5ms for 0x00 with SPACE parity
  unsigned long start = micros();
  while (!bus.available() && micros() - start < 5000) delayMicroseconds(10);

  if (!bus.available()) {
    printTelnetf("Timeout waiting for 0x00 response\n");
    return;
  }

  int response = bus.read();
  int parity = bus.readParity();
  if (response != 0x00 || parity) {
    printTelnetf("Invalid response: 0x%02X, parity: %s\n", response, parity ? "MARK" : "SPACE");
    return;
  }

  // Step 3: Wait precisely 1.4ms
  delayMicroseconds(1400);

  // Step 4: Send byte stream with SPACE parity
  uint8_t request[] = {0x00, 0x04, 0xC8, 0x01, 0xFF, 0xFF, 0xCB};
  for (size_t i = 0; i < sizeof(request); ++i) {
    bus.write(request[i], EspSoftwareSerial::PARITY_SPACE);
  }
  //printTelnetf("Sent request frame with SPACE parity\n");

  // Step 5: Wait up to 5ms for 0x7F response
  start = micros();
  while (!bus.available() && micros() - start < 8000) delayMicroseconds(10);

  if (!bus.available()) {
    printTelnetf("Timeout waiting for 0x7F ack\n");
    return;
  }

  response = bus.read();
  parity = bus.readParity();
  if (response != 0x7F || parity) {
    printTelnetf("Invalid ack: 0x%02X, parity: %s\n", response, parity ? "MARK" : "SPACE");
    return;
  }

  // Step 6: Wait 1.8ms
  delayMicroseconds(1800);

  // Step 7: Send 0x20 with MARK parity
  bus.write(0x20, EspSoftwareSerial::PARITY_MARK);

  // Step 8: Wait up to 3ms for the start of 13-byte reply
  start = micros();
  while (!bus.available() && micros() - start < 8000) delayMicroseconds(10);

  if (!bus.available()) {
    printTelnetf("Timeout waiting for 13-byte response\n");
    return;
  }

// Step 9: Read up to 64 bytes, timeout if no new byte in 1ms
uint8_t buffer[64];
size_t count = 0;
unsigned long lastByteTime = micros();

while (count < sizeof(buffer)) {
  if (bus.available()) {
    buffer[count++] = bus.read();
    bus.readParity();  // always consume parity after read
    lastByteTime = micros();  // reset timeout
  } else if (micros() - lastByteTime >= 2000) {
    break;  // idle for 1ms: end of message
  }
}


  

  // Step 10: Wait 1.5ms before sending 0x7F
  delayMicroseconds(120);
  bus.write(0x7F, EspSoftwareSerial::PARITY_SPACE);

  if (count == 0) {
  printTelnetf("No data received after 0x20\n");
  return;
}


  if (count > 1) {
    printTelnetf("Invalid response: got %d bytes, expected 1\n", count);
    return;
  }

  printTelnetf("Received response: ");
  for (size_t i = 0; i < 13; ++i) {
    printTelnetf("0x%02X ", buffer[i]);
  }
  printTelnetf("\n");
}

void sendProbe(uint8_t command) {
  printTelnetf("Sending command: 0x%02X with MARK parity...\n", command);
  bus.flush();
  // Send the command byte with MARK parity
  bus.write(command, EspSoftwareSerial::PARITY_MARK);

  // Wait up to 100ms for the first response byte
  unsigned long start = millis();
  while (!bus.available() && (millis() - start < 100)) {
    delay(1);
  }

  if (!bus.available()) {
    printTelnetf("No response within 100ms (timeout)\n");
    return;
  }

  // Read up to 96 bytes with a 2ms idle timeout between bytes
  uint8_t buffer[96];
  size_t count = 0;
  unsigned long lastByteTime = millis();

  while (count < sizeof(buffer)) {
    if (bus.available()) {
      buffer[count++] = bus.read();
      bus.readParity();  // Call to clear/align internal parity state
      lastByteTime = millis();
    } else {
      if (millis() - lastByteTime >= 4) break; // Done if no byte for 2ms
    }
  }

  delay(1);
  bus.write(0x7F, EspSoftwareSerial::PARITY_SPACE);

  // Print response in 0x00 format
  printTelnetf("Received %d byte(s): ", count);
  for (size_t i = 0; i < count; ++i) {
    printTelnetf("0x%02X ", buffer[i]);
  }
  printTelnetf("\n");
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
  }else if (strcmp(buf, "test") == 0) {
  sendProbe(0x20); // 0x1C0 sent LSB-first as 8-bit + parity = 0xC1 with MARK
}else if (strcmp(buf, "poll") == 0) {
  sendProbe(0x20);
  delay(1);
  sendFirstCommand();
  delay(1);
  handlePoll();
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
    WiFiClient newClient = telnetserver.accept();
    if (newClient)
    {
      if (telnetclient && telnetclient.connected())
      {
        telnetclient.stop(); // Disconnect existing client
      }
      telnetclient = newClient;
      telnetclient.setNoDelay(true);
      printTelnetf("Telnet connected\n");
      printTelnetf("Fake Huawei PMU\n");
      printTelnetf("Build %s %s\n",__DATE__,__TIME__);
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

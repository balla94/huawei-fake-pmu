#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ETH.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#define PROMPT_BUFFER_SIZE 256
#define TELNET_QUEUE_SIZE 64          // Increased queue size
#define TELNET_MSG_SIZE 512           // Larger messages for batching
#define TELNET_BATCH_SIZE 1400        // Near MTU size for batching
#define TELNET_BATCH_TIMEOUT_MS 5     // Batch timeout

#define UART_COMMAND_QUEUE_SIZE 10
#define UART_RESPONSE_QUEUE_SIZE 10

// DEFINE HERE THE KNOWN NETWORKS
const char *KNOWN_SSID[] = {"P4IT", "szarvas", "clueQuest IoT", "Lufi"};
const char *KNOWN_PASSWORD[] = {"pappka22", "pappka22", "CQIOT8147QQQ","almakorte12"};

const int KNOWN_SSID_COUNT = sizeof(KNOWN_SSID) / sizeof(KNOWN_SSID[0]);

WiFiServer telnetserver(23);
WiFiClient telnetclient;

// Telnet logging queue and task handle
QueueHandle_t telnetQueue;
TaskHandle_t telnetTaskHandle;

// UART command and response queues
QueueHandle_t uartCommandQueue;
QueueHandle_t uartResponseQueue;
TaskHandle_t uartTaskHandle;

// UART command types
enum UartCommand {
  CMD_TEST,
  CMD_POLL_SEQUENCE,
  CMD_FIRST_COMMAND,
  CMD_POLL_DATA
};

// UART command structure
struct UartCommandMsg {
  UartCommand command;
  uint8_t parameter;
};

// UART response structure
struct UartResponseMsg {
  char message[256];
  bool success;
  uint8_t data[64];
  size_t dataLength;
};

// Structure for telnet messages
struct TelnetMessage {
  char message[TELNET_MSG_SIZE];
  size_t length;
  bool urgent;  // For immediate sending without batching
};

// Batch buffer for combining multiple messages
static char batchBuffer[TELNET_BATCH_SIZE];
static size_t batchLength = 0;
static unsigned long lastBatchTime = 0;

// Non-blocking telnet print function
void printTelnet(const char *message, bool urgent = false)
{
  if (!telnetQueue) return;
  
  TelnetMessage msg;
  size_t len = strlen(message);
  if (len >= TELNET_MSG_SIZE) len = TELNET_MSG_SIZE - 1;
  
  memcpy(msg.message, message, len);
  msg.message[len] = '\0';
  msg.length = len;
  msg.urgent = urgent;
  
  // Try to send to queue without blocking
  if (xQueueSend(telnetQueue, &msg, 0) != pdTRUE) {
    // Queue is full, message dropped
  }
}

// Non-blocking telnet printf function
void printTelnetf(const char *fmt, ...)
{
  if (!telnetQueue) return;
  
  TelnetMessage msg;
  va_list args;
  va_start(args, fmt);
  msg.length = vsnprintf(msg.message, TELNET_MSG_SIZE, fmt, args);
  va_end(args);
  msg.urgent = false;
  
  if (msg.length >= TELNET_MSG_SIZE) {
    msg.length = TELNET_MSG_SIZE - 1;
    msg.message[msg.length] = '\0';
  }
  
  // Try to send to queue without blocking
  if (xQueueSend(telnetQueue, &msg, 0) != pdTRUE) {
    // Queue is full, message dropped
  }
}

// Urgent printf for immediate sending
void printTelnetfUrgent(const char *fmt, ...)
{
  if (!telnetQueue) return;
  
  TelnetMessage msg;
  va_list args;
  va_start(args, fmt);
  msg.length = vsnprintf(msg.message, TELNET_MSG_SIZE, fmt, args);
  va_end(args);
  msg.urgent = true;
  
  if (msg.length >= TELNET_MSG_SIZE) {
    msg.length = TELNET_MSG_SIZE - 1;
    msg.message[msg.length] = '\0';
  }
  
  // Try to send to queue without blocking
  if (xQueueSend(telnetQueue, &msg, 0) != pdTRUE) {
    // Queue is full, message dropped
  }
}

// Optimized function to send data with proper line ending conversion
void sendToTelnetClient(const char *data, size_t length) {
  if (!telnetclient || !telnetclient.connected()) return;
  
  // Send data in chunks with line ending conversion
  const char *ptr = data;
  const char *end = data + length;
  
  while (ptr < end) {
    const char *lineEnd = (const char*)memchr(ptr, '\n', end - ptr);
    
    if (lineEnd) {
      // Send data up to \n
      if (lineEnd > ptr) {
        telnetclient.write((const uint8_t*)ptr, lineEnd - ptr);
      }
      // Send \r\n for telnet
      telnetclient.write("\r\n", 2);
      ptr = lineEnd + 1;
    } else {
      // Send remaining data
      telnetclient.write((const uint8_t*)ptr, end - ptr);
      break;
    }
  }
}

// Flush batch buffer
void flushBatch() {
  if (batchLength > 0 && telnetclient && telnetclient.connected()) {
    sendToTelnetClient(batchBuffer, batchLength);
    batchLength = 0;
  }
  lastBatchTime = millis();
}

// Add message to batch or send immediately
void processTelnetMessage(const TelnetMessage& msg) {
  if (msg.urgent || !telnetclient || !telnetclient.connected()) {
    // Send immediately for urgent messages or if not connected
    flushBatch(); // Flush any pending batch first
    if (telnetclient && telnetclient.connected()) {
      sendToTelnetClient(msg.message, msg.length);
    }
    return;
  }
  
  // Check if message fits in batch
  if (batchLength + msg.length > TELNET_BATCH_SIZE - 10) { // Leave some margin
    flushBatch();
  }
  
  // Add to batch
  memcpy(batchBuffer + batchLength, msg.message, msg.length);
  batchLength += msg.length;
  
  // Update batch time
  if (batchLength == msg.length) { // First message in batch
    lastBatchTime = millis();
  }
}

// High-performance telnet output task
void telnetOutputTask(void *parameter) {
  TelnetMessage msg;
  TickType_t timeout = pdMS_TO_TICKS(1); // 1ms timeout for responsiveness
  
  while (true) {
    // Process messages from queue
    bool gotMessage = false;
    while (xQueueReceive(telnetQueue, &msg, 0) == pdTRUE) {
      processTelnetMessage(msg);
      gotMessage = true;
    }
    
    // Check if batch should be flushed due to timeout
    if (batchLength > 0 && (millis() - lastBatchTime >= TELNET_BATCH_TIMEOUT_MS)) {
      flushBatch();
    }
    
    // Small delay if no messages processed
    if (!gotMessage) {
      vTaskDelay(timeout);
    }
  }
}

// UART Task running on core 1 - handles ALL software serial operations
void uartTask(void *parameter) {
  // Initialize software serial on this core
  EspSoftwareSerial::UART bus;
  bus.begin(9600, SWSERIAL_8S1, 36, 4, false, 256, 256);
  bus.enableIntTx(false);
  
  UartCommandMsg cmd;
  UartResponseMsg response;
  
  while (true) {
    // Wait for UART commands
    if (xQueueReceive(uartCommandQueue, &cmd, portMAX_DELAY) == pdTRUE) {
      response.success = false;
      response.dataLength = 0;
      memset(response.data, 0, sizeof(response.data));
      
      switch (cmd.command) {
        case CMD_TEST:
          {
            snprintf(response.message, sizeof(response.message), "Sending command: 0x%02X with MARK parity...\n", cmd.parameter);
            xQueueSend(uartResponseQueue, &response, 0);
            
            bus.flush();
            bus.write(cmd.parameter, EspSoftwareSerial::PARITY_MARK);

            unsigned long start = millis();
            while (!bus.available() && (millis() - start < 100)) {
              delay(1);
            }

            if (!bus.available()) {
              snprintf(response.message, sizeof(response.message), "No response within 100ms (timeout)\n");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            uint8_t buffer[96];
            size_t count = 0;
            unsigned long lastByteTime = millis();

            while (count < sizeof(buffer)) {
              if (bus.available()) {
                buffer[count++] = bus.read();
                bus.readParity();
                lastByteTime = millis();
              } else {
                if (millis() - lastByteTime >= 4) break;
              }
            }

            delay(1);
            bus.write(0x7F, EspSoftwareSerial::PARITY_SPACE);

            // Copy data to response
            if (count > 0) {
              memcpy(response.data, buffer, min(count, sizeof(response.data)));
              response.dataLength = count;
              response.success = true;
            }
            
            snprintf(response.message, sizeof(response.message), "Received %d byte(s): ", count);
            for (size_t i = 0; i < count && i < 32; ++i) {
              char hex[8];
              snprintf(hex, sizeof(hex), "0x%02X ", buffer[i]);
              strcat(response.message, hex);
            }
            strcat(response.message, "\n");
            xQueueSend(uartResponseQueue, &response, 0);
          }
          break;
          
        case CMD_FIRST_COMMAND:
          {
            snprintf(response.message, sizeof(response.message), "Sending first sequence...\n");
            xQueueSend(uartResponseQueue, &response, 0);
            
            bus.flush();
            
            // Step 1: Send 0xC0 with MARK parity
            bus.write(0xC0, EspSoftwareSerial::PARITY_MARK);

            // Step 2: Wait up to 5ms for 0x00 with SPACE parity
            unsigned long start = micros();
            while (!bus.available() && micros() - start < 5000) delayMicroseconds(10);

            if (!bus.available()) {
              snprintf(response.message, sizeof(response.message), "Timeout waiting for 0x00 response\n");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            int uartResponse = bus.read();
            int parity = bus.readParity();
            if (uartResponse != 0x00 || parity) {
              snprintf(response.message, sizeof(response.message), "Invalid response: 0x%02X, parity: %s\n", uartResponse, parity ? "MARK" : "SPACE");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            // Step 3: Wait precisely 1.4ms
            delayMicroseconds(1400);

            // Step 4: Send byte stream with SPACE parity
            uint8_t request[] = {0x00, 0x04, 0xC8, 0x01, 0xFF, 0xFF, 0xCB};
            for (size_t i = 0; i < sizeof(request); ++i) {
              bus.write(request[i], EspSoftwareSerial::PARITY_SPACE);
            }

            // Step 5: Wait up to 8ms for 0x7F response
            start = micros();
            while (!bus.available() && micros() - start < 15000) delayMicroseconds(10);

            if (!bus.available()) {
              snprintf(response.message, sizeof(response.message), "Timeout waiting for 0x7F ack\n");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            uartResponse = bus.read();
            parity = bus.readParity();
            if (uartResponse != 0x7F || parity) {
              snprintf(response.message, sizeof(response.message), "Invalid ack: 0x%02X, parity: %s\n", uartResponse, parity ? "MARK" : "SPACE");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            // Step 6: Wait 1.8ms
            delayMicroseconds(1800);

            // Step 7: Send 0x20 with MARK parity
            bus.write(0x20, EspSoftwareSerial::PARITY_MARK);

            // Step 8: Wait up to 8ms for response
            start = micros();
            while (!bus.available() && micros() - start < 15000) delayMicroseconds(10);

            if (!bus.available()) {
              snprintf(response.message, sizeof(response.message), "Timeout waiting for response\n");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            // Step 9: Read response
            uint8_t buffer[64];
            size_t count = 0;
            unsigned long lastByteTime = micros();

            while (count < sizeof(buffer)) {
              if (bus.available()) {
                buffer[count++] = bus.read();
                bus.readParity();
                lastByteTime = micros();
              } else if (micros() - lastByteTime >= 2000) {
                break;
              }
            }

            // Step 10: Wait 120μs before sending 0x7F
            delayMicroseconds(120);
            bus.write(0x7F, EspSoftwareSerial::PARITY_SPACE);

            if (count == 0) {
              snprintf(response.message, sizeof(response.message), "No data received after 0x20\n");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            if (count > 1) {
              snprintf(response.message, sizeof(response.message), "Invalid response: got %d bytes, expected 1\n", count);
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            // Copy data to response
            memcpy(response.data, buffer, count);
            response.dataLength = count;
            response.success = true;
            
            snprintf(response.message, sizeof(response.message), "Received response: ");
            for (size_t i = 0; i < count; ++i) {
              char hex[8];
              snprintf(hex, sizeof(hex), "0x%02X ", buffer[i]);
              strcat(response.message, hex);
            }
            strcat(response.message, "\n");
            xQueueSend(uartResponseQueue, &response, 0);
          }
          break;
          
        case CMD_POLL_DATA:
          {
            snprintf(response.message, sizeof(response.message), "Starting poll sequence...\n");
            xQueueSend(uartResponseQueue, &response, 0);
            
            bus.flush();

            // Step 1: Send 0xC0 with MARK parity
            bus.write(0xC0, EspSoftwareSerial::PARITY_MARK);

            // Step 2: Wait up to 5ms for 0x00 with SPACE parity
            unsigned long start = micros();
            while (!bus.available() && micros() - start < 15000) delayMicroseconds(10);

            if (!bus.available()) {
              snprintf(response.message, sizeof(response.message), "Timeout waiting for 0x00 response\n");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            int uartResponse = bus.read();
            int parity = bus.readParity();
            if (uartResponse != 0x00 || parity) {
              snprintf(response.message, sizeof(response.message), "Invalid response: 0x%02X, parity: %s\n", uartResponse, parity ? "MARK" : "SPACE");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            // Step 3: Wait precisely 1.4ms
            delayMicroseconds(1400);

            // Step 4: Send byte stream with SPACE parity
            uint8_t request[] = {0x00, 0x04, 0xC8, 0x24, 0xFF, 0xFF, 0xEE};
            for (size_t i = 0; i < sizeof(request); ++i) {
              bus.write(request[i], EspSoftwareSerial::PARITY_SPACE);
            }

            // Step 5: Wait up to 8ms for 0x7F response
            start = micros();
            while (!bus.available() && micros() - start < 15000) delayMicroseconds(10);

            if (!bus.available()) {
              snprintf(response.message, sizeof(response.message), "Timeout waiting for 0x7F ack\n");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            uartResponse = bus.read();
            parity = bus.readParity();
            if (uartResponse != 0x7F || parity) {
              snprintf(response.message, sizeof(response.message), "Invalid ack: 0x%02X, parity: %s\n", uartResponse, parity ? "MARK" : "SPACE");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            // Step 6: Wait 1.8ms
            delayMicroseconds(1800);

            // Step 7: Send 0x20 with MARK parity
            bus.write(0x20, EspSoftwareSerial::PARITY_MARK);

            // Step 8: Wait up to 8ms for the start of 13-byte reply
            start = micros();
            while (!bus.available() && micros() - start < 15000) delayMicroseconds(10);

            if (!bus.available()) {
              snprintf(response.message, sizeof(response.message), "Timeout waiting for 13-byte response\n");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            // Step 9: Read up to 64 bytes, timeout if no new byte in 2ms
            uint8_t buffer[64];
            size_t count = 0;
            unsigned long lastByteTime = micros();

            while (count < sizeof(buffer)) {
              if (bus.available()) {
                buffer[count++] = bus.read();
                bus.readParity();
                lastByteTime = micros();
              } else if (micros() - lastByteTime >= 2000) {
                break;
              }
            }

            // Step 10: Wait 120μs before sending 0x7F
            delayMicroseconds(120);
            bus.write(0x7F, EspSoftwareSerial::PARITY_SPACE);

            if (count == 0) {
              snprintf(response.message, sizeof(response.message), "No data received after 0x20\n");
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            if (count < 13) {
              snprintf(response.message, sizeof(response.message), "Incomplete response: got %d bytes, expected 13\n", count);
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            if (count > 13) {
              snprintf(response.message, sizeof(response.message), "Invalid response: got %d bytes, expected 13\n", count);
              xQueueSend(uartResponseQueue, &response, 0);
              break;
            }

            // Copy data to response
            memcpy(response.data, buffer, count);
            response.dataLength = count;
            response.success = true;

            snprintf(response.message, sizeof(response.message), "Received 13-byte data frame: ");
            for (size_t i = 0; i < 13; ++i) {
              char hex[8];
              snprintf(hex, sizeof(hex), "0x%02X ", buffer[i]);
              strcat(response.message, hex);
            }
            strcat(response.message, "\n");
            xQueueSend(uartResponseQueue, &response, 0);

            uint16_t voltage = ((uint16_t)buffer[8] << 8) | buffer[9];
            uint16_t current = ((uint16_t)buffer[10] << 8) | buffer[11];
            snprintf(response.message, sizeof(response.message), "Voltage: %.2f V, Current: %.2f A\n", voltage / 100.0, current / 100.0);
            xQueueSend(uartResponseQueue, &response, 0);

            snprintf(response.message, sizeof(response.message), "Poll done\n");
            xQueueSend(uartResponseQueue, &response, 0);
          }
          break;
      }
    }
  }
}

// Optimized WiFi and telnet setup
void optimizeNetworking() {
  // TCP performance optimizations
  WiFi.setSleep(false);  // Disable WiFi power saving for better performance
}

void setupTelnetOptimizations() {
  if (telnetclient && telnetclient.connected()) {
    // Disable Nagle's algorithm for lower latency - this works in Arduino ESP32
    telnetclient.setNoDelay(true);
    
    // Note: Advanced socket options like SO_KEEPALIVE, SO_SNDBUF etc. 
    // are not exposed in Arduino ESP32 WiFiClient, but the message batching
    // and non-blocking queue system provide the main performance benefits
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
    optimizeNetworking(); // Apply optimizations after getting IP
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
    delay(100);
  }

  int nbVisibleNetworks = WiFi.scanComplete();
  if (nbVisibleNetworks == 0)
  {
    Serial.println("No networks found. Retrying...");
    delay(5000);
    return;
  }

  Serial.printf("%d network(s) found\n", nbVisibleNetworks);

  int strongestRSSI = -100;
  String strongestSSID;
  uint8_t strongestBSSID[6];
  const char *password = nullptr;

  for (int i = 0; i < nbVisibleNetworks; ++i)
  {
    String currentSSID = WiFi.SSID(i);
    int currentRSSI = WiFi.RSSI(i);
    uint8_t *currentBSSID = WiFi.BSSID(i);

    for (int n = 0; n < KNOWN_SSID_COUNT; n++)
    {
      if (currentSSID == KNOWN_SSID[n])
      {
        Serial.printf("Found SSID: %s, RSSI: %d, BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      currentSSID.c_str(),
                      currentRSSI,
                      currentBSSID[0], currentBSSID[1], currentBSSID[2],
                      currentBSSID[3], currentBSSID[4], currentBSSID[5]);

        if (currentRSSI > strongestRSSI)
        {
          strongestRSSI = currentRSSI;
          strongestSSID = currentSSID;
          memcpy(strongestBSSID, currentBSSID, 6);
          password = KNOWN_PASSWORD[n];
        }
      }
    }
  }

  if (strongestSSID.isEmpty())
  {
    Serial.println("No known networks found. Retrying...");
    delay(5000);
    return;
  }

  Serial.printf("\nConnecting to SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X, RSSI: %d\n",
                strongestSSID.c_str(),
                strongestBSSID[0], strongestBSSID[1], strongestBSSID[2],
                strongestBSSID[3], strongestBSSID[4], strongestBSSID[5],
                strongestRSSI);

  WiFi.begin(strongestSSID.c_str(), password, 0, strongestBSSID);

  int timeout = 150;
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

  Serial.println("\nWiFi connected!");
  Serial.println("IP Address: ");
  Serial.println(WiFi.localIP());
}

void handleTelnetCommand(char *buf) {
  UartCommandMsg cmd;
  
  if (strcmp(buf, "reboot") == 0) {
    printTelnetfUrgent("Rebooting...\n");
    delay(300);
    ESP.restart();
  } 
  else if (strcmp(buf, "wifi") == 0) {
    printTelnetfUrgent("====== WIFI INFO ======= \n");
    printTelnetfUrgent("Connected to: %s\n", WiFi.SSID().c_str());
    printTelnetfUrgent("Signal: %d\n", WiFi.RSSI());
  }
  else if (strcmp(buf, "test") == 0) {
    cmd.command = CMD_TEST;
    cmd.parameter = 0x20;
    xQueueSend(uartCommandQueue, &cmd, 0);
  }
  else if (strcmp(buf, "poll") == 0) {
    // Send poll sequence
    cmd.command = CMD_TEST;
    cmd.parameter = 0x20;
    xQueueSend(uartCommandQueue, &cmd, 0);
    
    // Wait a bit then send first command
    vTaskDelay(pdMS_TO_TICKS(100));
    cmd.command = CMD_FIRST_COMMAND;
    xQueueSend(uartCommandQueue, &cmd, 0);
    
    // Wait a bit then send poll data command
    vTaskDelay(pdMS_TO_TICKS(100));
    cmd.command = CMD_POLL_DATA;
    xQueueSend(uartCommandQueue, &cmd, 0);
  }
  else if (strcmp(buf, "help") == 0) {
    printTelnetfUrgent("Commands: wifi, test, poll, reboot, help\n");
  }
  else {
    printTelnetf("Unknown command: %s\n", buf);
  }
}

// Response processor - checks for UART responses and forwards to telnet
void processUartResponses() {
  UartResponseMsg response;
  // Check for responses without blocking
  while (xQueueReceive(uartResponseQueue, &response, 0) == pdTRUE) {
    printTelnetf("%s", response.message);
  }
}

void setup()
{
  delay(200);
  Serial.begin(115200);
  
  // Create all queues
  telnetQueue = xQueueCreate(TELNET_QUEUE_SIZE, sizeof(TelnetMessage));
  uartCommandQueue = xQueueCreate(UART_COMMAND_QUEUE_SIZE, sizeof(UartCommandMsg));
  uartResponseQueue = xQueueCreate(UART_RESPONSE_QUEUE_SIZE, sizeof(UartResponseMsg));
  
  if (!telnetQueue || !uartCommandQueue || !uartResponseQueue) {
    Serial.println("Failed to create queues!");
    return;
  }
  
  // Create UART task on core 1 (isolated from WiFi/telnet)
  xTaskCreatePinnedToCore(
    uartTask,            // Task function
    "UartTask",          // Task name
    8192,                // Stack size
    NULL,                // Parameters
    5,                   // Highest priority
    &uartTaskHandle,     // Task handle
    1                    // Core 1 - isolated from WiFi
  );
  
  // Create telnet output task on core 0
  xTaskCreatePinnedToCore(
    telnetOutputTask,    // Task function
    "TelnetOutput",      // Task name
    8192,                // Stack size
    NULL,                // Parameters
    3,                   // High priority
    &telnetTaskHandle,   // Task handle
    0                    // Core 0 - with WiFi
  );
  
  WiFi.onEvent(WiFiEvent);
  wifiConnect();
  telnetserver.begin();

  ArduinoOTA.begin();
}

void telnetloop()
{
  static unsigned long lastTelnetInCheck = 0;
  static char promptBuffer[PROMPT_BUFFER_SIZE];
  static uint16_t promptIndex = 0;

  // Process UART responses first
  processUartResponses();

  // Handle new client connection
  if (telnetserver.hasClient())
  {
    WiFiClient newClient = telnetserver.accept();
    if (newClient)
    {
      if (telnetclient && telnetclient.connected())
      {
        telnetclient.stop();
      }
      telnetclient = newClient;
      
      // Apply optimizations immediately
      setupTelnetOptimizations();
      
      printTelnetfUrgent("Telnet connected (core isolated)\n");
      printTelnetfUrgent("Fake Huawei PMU\n");
      printTelnetfUrgent("Build %s %s\n", __DATE__, __TIME__);
      printTelnetfUrgent("Type 'help' for commands\n");
      promptIndex = 0;
    }
  }

  // Process client communication if connected
  if (telnetclient && telnetclient.connected())
  {
    // Check for input more frequently and filter out telnet control codes
    while (telnetclient.available()) {
      int c = telnetclient.read();
      
      // Filter out telnet control sequences and non-printable characters
      if (c == 255) {  // IAC (Interpret As Command) - skip telnet control sequences
        // Skip the next 2 bytes of telnet control sequence
        if (telnetclient.available()) telnetclient.read();
        if (telnetclient.available()) telnetclient.read();
        continue;
      }
      
      if (c >= 32 && c <= 126) {  // Only accept printable ASCII characters
        if (promptIndex < PROMPT_BUFFER_SIZE - 1) {
          promptBuffer[promptIndex++] = (char)c;
        }
      }
      else if (c == '\r' || c == '\n') {
        if (promptIndex > 0) {
          promptBuffer[promptIndex] = '\0';
          handleTelnetCommand(promptBuffer);
          promptIndex = 0;
        }
      }
      // Ignore all other control characters
    }
  }
}

void loop()
{
  telnetloop();
  ArduinoOTA.handle();
}
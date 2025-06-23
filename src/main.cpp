#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ETH.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "magic.h"
#include "LittleFS.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

EspSoftwareSerial::UART bus;

#define PROMPT_BUFFER_SIZE 256
#define TELNET_QUEUE_SIZE 64      // Increased queue size
#define TELNET_MSG_SIZE 512       // Larger messages for batching
#define TELNET_BATCH_SIZE 1400    // Near MTU size for batching
#define TELNET_BATCH_TIMEOUT_MS 5 // Batch timeout

#define UART_COMMAND_QUEUE_SIZE 10
#define UART_RESPONSE_QUEUE_SIZE 10

// DEFINE HERE THE KNOWN NETWORKS
const char *KNOWN_SSID[] = {"P4IT", "szarvas", "clueQuest IoT", "Lufi"};
const char *KNOWN_PASSWORD[] = {"pappka22", "pappka22", "CQIOT8147QQQ", "almakorte12"};

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

// Configuration structures
struct MqttConfig
{
  bool enabled;
  String serverIP;
  String baseTopic;
  String username;
  String password;
  int port;
};

struct PmuConfig
{
  int expectedClients;
  float outputVoltage;
  float outputCurrent;
};

// Global config variables
MqttConfig mqttConfig;
PmuConfig pmuConfig;

// MQTT status variables
bool mqtt_connected = false;
unsigned long last_mqtt_reconnect = 0;

// UART command types
enum UartCommand
{
  CMD_TEST,
  CMD_POLL_SEQUENCE,
  CMD_FIRST_COMMAND,
  CMD_POLL_DATA
};

// UART command structure
struct UartCommandMsg
{
  UartCommand command;
  uint8_t parameter;
};

// UART response structure
struct UartResponseMsg
{
  char message[256];
  bool success;
  uint8_t data[64];
  size_t dataLength;
};

// Structure for telnet messages
struct TelnetMessage
{
  char message[TELNET_MSG_SIZE];
  size_t length;
  bool urgent; // For immediate sending without batching
};

// Batch buffer for combining multiple messages
static char batchBuffer[TELNET_BATCH_SIZE];
static size_t batchLength = 0;
static unsigned long lastBatchTime = 0;

// Forward declarations
void printTelnetf(const char *fmt, ...);
void printTelnetfUrgent(const char *fmt, ...);
bool getEthernetStatus();
void updateMQTTStatus(bool connected);
void publishMQTTStatus();
void saveMqttConfig();
void savePmuConfig();
void initMQTT();
void mqttCallback(char *topic, byte *payload, unsigned int length);
int8_t pollSlave(uint8_t slaveID);

// Validate IP address format
bool isValidIPAddress(const String &ip)
{
  int parts[4];
  int partCount = 0;
  int startIndex = 0;

  for (int i = 0; i <= ip.length(); i++)
  {
    if (i == ip.length() || ip.charAt(i) == '.')
    {
      if (partCount >= 4)
        return false;

      String part = ip.substring(startIndex, i);
      if (part.length() == 0 || part.length() > 3)
        return false;

      int value = part.toInt();
      if (value < 0 || value > 255)
        return false;

      // Check if conversion was valid (not just 0 from invalid string)
      if (value == 0 && part != "0")
        return false;

      parts[partCount++] = value;
      startIndex = i + 1;
    }
  }

  return partCount == 4;
}

// Save MQTT configuration
void saveMqttConfig()
{
  JsonDocument doc;
  doc["enabled"] = mqttConfig.enabled;
  doc["serverIP"] = mqttConfig.serverIP;
  doc["baseTopic"] = mqttConfig.baseTopic;
  doc["username"] = mqttConfig.username;
  doc["password"] = mqttConfig.password;
  doc["port"] = mqttConfig.port;

  File file = LittleFS.open("/config/mqtt.cfg", "w");
  if (file)
  {
    serializeJson(doc, file);
    file.close();
    Serial.println("MQTT config saved");
  }
  else
  {
    Serial.println("Failed to save MQTT config");
  }
}

// Save PMU configuration
void savePmuConfig()
{
  JsonDocument doc;
  doc["expectedClients"] = pmuConfig.expectedClients;
  doc["outputVoltage"] = pmuConfig.outputVoltage;
  doc["outputCurrent"] = pmuConfig.outputCurrent;

  File file = LittleFS.open("/config/pmu.cfg", "w");
  if (file)
  {
    serializeJson(doc, file);
    file.close();
    Serial.println("PMU config saved");
  }
  else
  {
    Serial.println("Failed to save PMU config");
  }
}

// Load MQTT configuration
void loadMqttConfig()
{
  // Set defaults first
  mqttConfig.enabled = false;
  mqttConfig.serverIP = "0.0.0.0";
  mqttConfig.baseTopic = "pmu";
  mqttConfig.username = "admin";
  mqttConfig.password = "";
  mqttConfig.port = 1883;

  if (!LittleFS.exists("/config"))
  {
    LittleFS.mkdir("/config");
  }

  if (LittleFS.exists("/config/mqtt.cfg"))
  {
    File file = LittleFS.open("/config/mqtt.cfg", "r");
    if (file)
    {
      String content = file.readString();
      file.close();

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, content);

      if (!error)
      {
        mqttConfig.enabled = doc["enabled"] | false;
        String serverIP = doc["serverIP"] | "0.0.0.0";

        // Validate IP address
        if (isValidIPAddress(serverIP))
        {
          mqttConfig.serverIP = serverIP;
        }
        else
        {
          mqttConfig.serverIP = "0.0.0.0";
          mqttConfig.enabled = false; // Disable if invalid IP
          Serial.println("Invalid MQTT server IP, MQTT disabled");
        }

        mqttConfig.baseTopic = doc["baseTopic"] | "pmu";
        if (mqttConfig.baseTopic.isEmpty())
        {
          mqttConfig.baseTopic = "pmu";
        }

        mqttConfig.username = doc["username"] | "admin";
        mqttConfig.password = doc["password"] | "";
        mqttConfig.port = doc["port"] | 1883;

        Serial.printf("MQTT config loaded: %s, IP: %s, Topic: %s\n",
                      mqttConfig.enabled ? "enabled" : "disabled",
                      mqttConfig.serverIP.c_str(),
                      mqttConfig.baseTopic.c_str());
      }
      else
      {
        Serial.println("Failed to parse MQTT config, using defaults");
        saveMqttConfig(); // Save defaults
      }
    }
  }
  else
  {
    Serial.println("MQTT config not found, creating default");
    saveMqttConfig();
  }
}

// Load PMU configuration
void loadPmuConfig()
{
  // Set defaults first
  pmuConfig.expectedClients = 10;
  pmuConfig.outputVoltage = 48.0;
  pmuConfig.outputCurrent = 33.0;

  if (!LittleFS.exists("/config"))
  {
    LittleFS.mkdir("/config");
  }

  if (LittleFS.exists("/config/pmu.cfg"))
  {
    File file = LittleFS.open("/config/pmu.cfg", "r");
    if (file)
    {
      String content = file.readString();
      file.close();

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, content);

      if (!error)
      {
        int clients = doc["expectedClients"] | 10;
        pmuConfig.expectedClients = constrain(clients, 1, 10);

        pmuConfig.outputVoltage = doc["outputVoltage"] | 48.0;
        pmuConfig.outputCurrent = doc["outputCurrent"] | 33.0;

        Serial.printf("PMU config loaded: %d clients, %.1fV, %.1fA\n",
                      pmuConfig.expectedClients,
                      pmuConfig.outputVoltage,
                      pmuConfig.outputCurrent);
      }
      else
      {
        Serial.println("Failed to parse PMU config, using defaults");
        savePmuConfig(); // Save defaults
      }
    }
  }
  else
  {
    Serial.println("PMU config not found, creating default");
    savePmuConfig();
  }
}

// Dummy functions for compatibility
bool getEthernetStatus()
{
  return WiFi.status() == WL_CONNECTED;
}

void updateMQTTStatus(bool connected)
{
  // Update status indicator
  mqtt_connected = connected;
}

void publishMQTTStatus()
{
  if (mqttClient.connected())
  {
    String statusTopic = mqttConfig.baseTopic + "/status";
    mqttClient.publish(statusTopic.c_str(), mqtt_connected ? "online" : "offline");
  }
}

// MQTT callback function
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  // Convert payload to string
  String message = "";
  for (unsigned int i = 0; i < length; i++)
  {
    message += (char)payload[i];
  }

  String topicStr = String(topic);
  // Process MQTT messages here if needed
}

// Initialize MQTT
void initMQTT()
{
  if (mqttConfig.enabled && mqttConfig.serverIP != "0.0.0.0")
  {
    mqttClient.setServer(mqttConfig.serverIP.c_str(), mqttConfig.port);
    mqttClient.setCallback(mqttCallback);
    Serial.printf("MQTT client configured for server: %s:%d\n",
                  mqttConfig.serverIP.c_str(), mqttConfig.port);
  }
  else
  {
    Serial.println("MQTT disabled or invalid configuration");
  }
}

// Fixed WebSocket message handler
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len)
{
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
  {
    data[len] = 0;
    String message = (char *)data;
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_DATA:
    handleWebSocketMessage(arg, data, len);
    break;
  default:
    break;
  }
}

void initWebServer()
{
  // Create WebSocket
  ws.onEvent(onEvent);
  server.addHandler(&ws);

  // Handle favicon.ico requests to prevent 500 errors
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    // Send a simple 1x1 transparent PNG
    const uint8_t favicon[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
      0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
      0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
      0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
    };
    AsyncWebServerResponse *response = request->beginResponse_P(200, "image/png", favicon, sizeof(favicon));
    request->send(response); });

  // MQTT Configuration API
  server.on("/api/mqtt/config", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    doc["enabled"] = mqttConfig.enabled;
    doc["serverIP"] = mqttConfig.serverIP;
    doc["port"] = mqttConfig.port;
    doc["baseTopic"] = mqttConfig.baseTopic;
    doc["username"] = mqttConfig.username;
    // Don't send password in GET request for security
    doc["hasPassword"] = !mqttConfig.password.isEmpty();
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  server.on("/api/mqtt/config", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    // Handle form data
    bool configChanged = false;
    
    if (request->hasParam("enabled", true)) {
      bool newEnabled = request->getParam("enabled", true)->value() == "true";
      if (newEnabled != mqttConfig.enabled) {
        mqttConfig.enabled = newEnabled;
        configChanged = true;
      }
    }
    
    if (request->hasParam("serverIP", true)) {
      String newIP = request->getParam("serverIP", true)->value();
      if (isValidIPAddress(newIP) && newIP != mqttConfig.serverIP) {
        mqttConfig.serverIP = newIP;
        configChanged = true;
      } else if (!isValidIPAddress(newIP)) {
        request->send(400, "application/json", "{\"error\":\"Invalid IP address\"}");
        return;
      }
    }
    
    if (request->hasParam("port", true)) {
      int newPort = request->getParam("port", true)->value().toInt();
      if (newPort > 0 && newPort <= 65535 && newPort != mqttConfig.port) {
        mqttConfig.port = newPort;
        configChanged = true;
      }
    }
    
    if (request->hasParam("baseTopic", true)) {
      String newTopic = request->getParam("baseTopic", true)->value();
      if (newTopic.isEmpty()) newTopic = "pmu";
      if (newTopic != mqttConfig.baseTopic) {
        mqttConfig.baseTopic = newTopic;
        configChanged = true;
      }
    }
    
    if (request->hasParam("username", true)) {
      String newUsername = request->getParam("username", true)->value();
      if (newUsername != mqttConfig.username) {
        mqttConfig.username = newUsername;
        configChanged = true;
      }
    }
    
    if (request->hasParam("password", true)) {
      String newPassword = request->getParam("password", true)->value();
      if (newPassword != mqttConfig.password) {
        mqttConfig.password = newPassword;
        configChanged = true;
      }
    }
    
    if (configChanged) {
      saveMqttConfig();
      
      // Disconnect and reconnect MQTT if needed
      if (mqttClient.connected()) {
        mqttClient.disconnect();
      }
      
      initMQTT(); // Reinitialize with new config
    }
    
    request->send(200, "application/json", "{\"status\":\"success\"}"); });

  // PMU Configuration API
  server.on("/api/pmu/config", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    doc["expectedClients"] = pmuConfig.expectedClients;
    doc["outputVoltage"] = pmuConfig.outputVoltage;
    doc["outputCurrent"] = pmuConfig.outputCurrent;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  server.on("/api/pmu/config", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    bool configChanged = false;
    
    if (request->hasParam("expectedClients", true)) {
      int newClients = request->getParam("expectedClients", true)->value().toInt();
      newClients = constrain(newClients, 1, 10);
      if (newClients != pmuConfig.expectedClients) {
        pmuConfig.expectedClients = newClients;
        configChanged = true;
      }
    }
    
    if (request->hasParam("outputVoltage", true)) {
      float newVoltage = request->getParam("outputVoltage", true)->value().toFloat();
      if (newVoltage != pmuConfig.outputVoltage) {
        pmuConfig.outputVoltage = newVoltage;
        configChanged = true;
      }
    }
    
    if (request->hasParam("outputCurrent", true)) {
      float newCurrent = request->getParam("outputCurrent", true)->value().toFloat();
      if (newCurrent != pmuConfig.outputCurrent) {
        pmuConfig.outputCurrent = newCurrent;
        configChanged = true;
      }
    }
    
    if (configChanged) {
      savePmuConfig();
    }
    
    request->send(200, "application/json", "{\"status\":\"success\"}"); });

  // Serve static files from LittleFS root directory (AFTER API routes)
  server.serveStatic("/", LittleFS, "/")
      .setDefaultFile("index.html")
      .setFilter([](AsyncWebServerRequest *request)
                 {
      // Only serve static files for non-API routes
      return !request->url().startsWith("/api/"); });

  // Handle root path explicitly
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (LittleFS.exists("/index.html")) {
      request->send(LittleFS, "/index.html", "text/html");
    } else {
      // Simple fallback page
      String html = "<!DOCTYPE html><html><head><title>Fake PMU</title></head>";
      html += "<body><h1>Huawei Fake PMU</h1>";
      html += "<p>Failed to load page. LittleFS storage is corrupt or missing</p>";
      html += "</body></html>";
      request->send(200, "text/html", html);
    } });

  server.begin();
  Serial.println("Web server started");
}

// Handle MQTT reconnection
void handleMQTTReconnect()
{
  if (!mqttConfig.enabled || mqttConfig.serverIP == "0.0.0.0")
  {
    return; // Don't try to connect if disabled or invalid IP
  }

  if (!getEthernetStatus())
  {
    return; // Don't try to connect if ethernet is down
  }

  if (millis() - last_mqtt_reconnect < 5000)
  {
    return; // Don't try to reconnect too frequently
  }

  last_mqtt_reconnect = millis();

  // Use configured credentials for connection
  bool connected = false;
  if (mqttConfig.username.length() > 0)
  {
    connected = mqttClient.connect("poe-panel-controller",
                                   mqttConfig.username.c_str(),
                                   mqttConfig.password.c_str());
  }
  else
  {
    connected = mqttClient.connect("poe-panel-controller");
  }

  if (connected)
  {
    Serial.println("MQTT connected");
    updateMQTTStatus(true);
    mqtt_connected = true;

    // Subscribe to topics using configured base topic
    String moveTopic = mqttConfig.baseTopic + "/move";
    String ledTopic = mqttConfig.baseTopic + "/led";

    mqttClient.subscribe(moveTopic.c_str());
    mqttClient.subscribe(ledTopic.c_str());

    Serial.printf("MQTT subscribed to: %s, %s\n", moveTopic.c_str(), ledTopic.c_str());

    // Publish initial status immediately
    publishMQTTStatus();
  }
  else
  {
    Serial.printf("MQTT connection failed, rc=%d\n", mqttClient.state());
    updateMQTTStatus(false);
    mqtt_connected = false;
  }
}

// File system functions - SIMPLIFIED
void initLittleFS()
{
  if (!LittleFS.begin(true))
  {
    return;
  }
}

// Non-blocking telnet print function
void printTelnet(const char *message, bool urgent = false)
{
  if (!telnetQueue)
    return;

  TelnetMessage msg;
  size_t len = strlen(message);
  if (len >= TELNET_MSG_SIZE)
    len = TELNET_MSG_SIZE - 1;

  memcpy(msg.message, message, len);
  msg.message[len] = '\0';
  msg.length = len;
  msg.urgent = urgent;

  // Try to send to queue without blocking
  if (xQueueSend(telnetQueue, &msg, 0) != pdTRUE)
  {
    // Queue is full, message dropped
  }
}

// Non-blocking telnet printf function
void printTelnetf(const char *fmt, ...)
{
  if (!telnetQueue)
    return;

  TelnetMessage msg;
  va_list args;
  va_start(args, fmt);
  msg.length = vsnprintf(msg.message, TELNET_MSG_SIZE, fmt, args);
  va_end(args);
  msg.urgent = false;

  if (msg.length >= TELNET_MSG_SIZE)
  {
    msg.length = TELNET_MSG_SIZE - 1;
    msg.message[msg.length] = '\0';
  }

  // Try to send to queue without blocking
  if (xQueueSend(telnetQueue, &msg, 0) != pdTRUE)
  {
    // Queue is full, message dropped
  }
}

// Urgent printf for immediate sending
void printTelnetfUrgent(const char *fmt, ...)
{
  if (!telnetQueue)
    return;

  TelnetMessage msg;
  va_list args;
  va_start(args, fmt);
  msg.length = vsnprintf(msg.message, TELNET_MSG_SIZE, fmt, args);
  va_end(args);
  msg.urgent = true;

  if (msg.length >= TELNET_MSG_SIZE)
  {
    msg.length = TELNET_MSG_SIZE - 1;
    msg.message[msg.length] = '\0';
  }

  // Try to send to queue without blocking
  if (xQueueSend(telnetQueue, &msg, 0) != pdTRUE)
  {
    // Queue is full, message dropped
  }
}

// Optimized function to send data with proper line ending conversion
void sendToTelnetClient(const char *data, size_t length)
{
  if (!telnetclient || !telnetclient.connected())
    return;

  // Send data in chunks with line ending conversion
  const char *ptr = data;
  const char *end = data + length;

  while (ptr < end)
  {
    const char *lineEnd = (const char *)memchr(ptr, '\n', end - ptr);

    if (lineEnd)
    {
      // Send data up to \n
      if (lineEnd > ptr)
      {
        telnetclient.write((const uint8_t *)ptr, lineEnd - ptr);
      }
      // Send \r\n for telnet
      telnetclient.write("\r\n", 2);
      ptr = lineEnd + 1;
    }
    else
    {
      // Send remaining data
      telnetclient.write((const uint8_t *)ptr, end - ptr);
      break;
    }
  }
}

// Flush batch buffer
void flushBatch()
{
  if (batchLength > 0 && telnetclient && telnetclient.connected())
  {
    sendToTelnetClient(batchBuffer, batchLength);
    batchLength = 0;
  }
  lastBatchTime = millis();
}

// Add message to batch or send immediately
void processTelnetMessage(const TelnetMessage &msg)
{
  if (msg.urgent || !telnetclient || !telnetclient.connected())
  {
    // Send immediately for urgent messages or if not connected
    flushBatch(); // Flush any pending batch first
    if (telnetclient && telnetclient.connected())
    {
      sendToTelnetClient(msg.message, msg.length);
    }
    return;
  }

  // Check if message fits in batch
  if (batchLength + msg.length > TELNET_BATCH_SIZE - 10)
  { // Leave some margin
    flushBatch();
  }

  // Add to batch
  memcpy(batchBuffer + batchLength, msg.message, msg.length);
  batchLength += msg.length;

  // Update batch time
  if (batchLength == msg.length)
  { // First message in batch
    lastBatchTime = millis();
  }
}

// High-performance telnet output task
void telnetOutputTask(void *parameter)
{
  TelnetMessage msg;
  TickType_t timeout = pdMS_TO_TICKS(1); // 1ms timeout for responsiveness

  while (true)
  {
    // Process messages from queue
    bool gotMessage = false;
    while (xQueueReceive(telnetQueue, &msg, 0) == pdTRUE)
    {
      processTelnetMessage(msg);
      gotMessage = true;
    }

    // Check if batch should be flushed due to timeout
    if (batchLength > 0 && (millis() - lastBatchTime >= TELNET_BATCH_TIMEOUT_MS))
    {
      flushBatch();
    }

    // Small delay if no messages processed
    if (!gotMessage)
    {
      vTaskDelay(timeout);
    }
  }
}

void parseOutputValues(uint8_t slaveID, uint8_t *buffer, size_t count)
{
  if (count != 13)
  {
    printTelnetf("Invalid packet size");
  }
  uint16_t voltage = ((uint16_t)buffer[8] << 8) | buffer[9];
  uint16_t current = ((uint16_t)buffer[10] << 8) | buffer[11];

  float vout = voltage / 100.0;
  float cout = current / 100.0;

  printTelnetf("Slave %d output voltage: %2.2fV current: %2.2fA\n", slaveID, vout, cout);
}

uint32_t getMagic(uint8_t *buffer, size_t count)
{
  if (count < 7) // Need 7 bytes now (0,1,2,3,4,5,6)
    return 0;

  return ((uint32_t)buffer[3] << 24) | // Changed from buffer[2]
         ((uint32_t)buffer[4] << 16) | // Changed from buffer[3]
         ((uint32_t)buffer[5] << 8) |  // Changed from buffer[4]
         ((uint32_t)buffer[6]);        // Changed from buffer[5]
}

uint8_t calculateCRC(const uint8_t *buffer, int length)
{
  uint16_t sum = 0;
  for (int i = 0; i < length; i++)
  {
    sum += buffer[i];
  }
  return sum & 0xFF; // Take lower 8 bits (modulo 256)
}

int8_t sendSimpleCommand(uint8_t slaveID, uint32_t command)
{
  printTelnetf("Sending simple command to slave %d\n", slaveID);
  bus.flush();
  while (bus.available())
    bus.read();

  bus.write(0xC0 + slaveID, EspSoftwareSerial::PARITY_MARK);

  unsigned long start = millis();
  while (!bus.available() && (millis() - start < 100))
  {
    delay(1);
  }

  if (!bus.available())
  {
    printTelnetf("Timeout.\n");
    return -1;
  }

  uint8_t buffer[96];
  size_t count = 0;
  unsigned long lastByteTime = millis();

  while (count < sizeof(buffer))
  {
    if (bus.available())
    {
      buffer[count++] = bus.read();
      lastByteTime = millis();
    }
    else
    {
      if (millis() - lastByteTime >= 20)
        break;
    }
  }

  if (count != 1 || buffer[0] != slaveID)
  {
    printTelnetf("Unexpected response. Got %d bytes, expected 1. First byte: 0x%02X, expected: 0x%02X\n",
                 count, count > 0 ? buffer[0] : 0, slaveID);
    return -2;
  }

  // Send 4-byte command with SPACE parity
  printTelnetf("Sending command: 0x%08X\n", command);

  // Create frame: 0x00 0x04 cmd_byte3 cmd_byte2 cmd_byte1 cmd_byte0 crc
  uint8_t frame[7];
  frame[0] = 0x00; // Header/slave ID
  frame[1] = 0x04; // Payload length (4 bytes)

  // Split uint32_t into 4 bytes (big-endian)
  frame[2] = (command >> 24) & 0xFF; // MSB
  frame[3] = (command >> 16) & 0xFF;
  frame[4] = (command >> 8) & 0xFF;
  frame[5] = command & 0xFF; // LSB

  // Calculate CRC on first 6 bytes
  frame[6] = calculateCRC(frame, 6);

  // Send all bytes with SPACE parity
  for (int i = 0; i < 7; i++)
  {
    bus.write(frame[i], EspSoftwareSerial::PARITY_SPACE);
  }

  printTelnetf("Frame sent: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X (SPACE parity)\n",
               frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6]);
  delay(1);

  count = 0;
  lastByteTime = millis();

  while (count < sizeof(buffer))
  {
    if (bus.available())
    {
      buffer[count++] = bus.read();
      lastByteTime = millis();
    }
    else
    {
      if (millis() - lastByteTime >= 16)
        break;
    }
  }

  if (count != 1 || buffer[0] != 0x7F)
  {
    printTelnetf("Unexpected response. Got %d bytes, expected 1. First byte: 0x%02X, expected: 0x7F ACK\n",
                 count, count > 0 ? buffer[0] : 0);
    return -2;
  }
  else
    printTelnetf("Got ACK\n");

  return 0; // Success
}

int8_t sendCommandWithPayload(uint8_t slaveID, uint32_t command, uint32_t payload)
{
  printTelnetf("Sending command with payload to slave %d\n", slaveID);
  bus.flush();
  while (bus.available())
    bus.read();

  bus.write(0xC0 + slaveID, EspSoftwareSerial::PARITY_MARK);

  unsigned long start = millis();
  while (!bus.available() && (millis() - start < 100))
  {
    delay(1);
  }

  if (!bus.available())
  {
    printTelnetf("Timeout.\n");
    return -1;
  }

  uint8_t buffer[96];
  size_t count = 0;
  unsigned long lastByteTime = millis();

  while (count < sizeof(buffer))
  {
    if (bus.available())
    {
      buffer[count++] = bus.read();
      lastByteTime = millis();
    }
    else
    {
      if (millis() - lastByteTime >= 20)
        break;
    }
  }

  if (count != 1 || buffer[0] != slaveID)
  {
    printTelnetf("Unexpected response. Got %d bytes, expected 1. First byte: 0x%02X, expected: 0x%02X\n",
                 count, count > 0 ? buffer[0] : 0, slaveID);
    return -2;
  }

  // Send 4-byte command with SPACE parity
  printTelnetf("Sending command: 0x%08X\n", command);

  // Create frame: 0x00 0x04 cmd_byte3 cmd_byte2 cmd_byte1 cmd_byte0 crc
  uint8_t frame[11];
  frame[0] = 0x00; // Header/slave ID
  frame[1] = 0x08; // Payload length (4 bytes)

  // Split uint32_t into 4 bytes (big-endian)
  frame[2] = (command >> 24) & 0xFF; // MSB
  frame[3] = (command >> 16) & 0xFF;
  frame[4] = (command >> 8) & 0xFF;
  frame[5] = command & 0xFF; // LSB

  // Split uint32_t into 4 bytes (big-endian)
  frame[6] = (payload >> 24) & 0xFF; // MSB
  frame[7] = (payload >> 16) & 0xFF;
  frame[8] = (payload >> 8) & 0xFF;
  frame[9] = payload & 0xFF; // LSB

  // Calculate CRC on first 6 bytes
  frame[10] = calculateCRC(frame, 10);

  // Send all bytes with SPACE parity
  for (int i = 0; i < 11; i++)
  {
    bus.write(frame[i], EspSoftwareSerial::PARITY_SPACE);
  }

  printTelnetf("Frame sent: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X \n",
               frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7], frame[8], frame[9], frame[10]);
  delay(1);

  count = 0;
  lastByteTime = millis();

  while (count < sizeof(buffer))
  {
    if (bus.available())
    {
      buffer[count++] = bus.read();
      lastByteTime = millis();
    }
    else
    {
      if (millis() - lastByteTime >= 16)
        break;
    }
  }

  if (count != 1 || buffer[0] != 0x7F)
  {
    printTelnetf("Unexpected response. Got %d bytes, expected 1. First byte: 0x%02X, expected: 0x7F ACK\n",
                 count, count > 0 ? buffer[0] : 0);
    return -2;
  }
  else
    printTelnetf("Got ACK\n");

  return 0; // Success
}

void setVoltage(uint8_t slaveID, float voltage, float current)
{
  // Convert to integers (multiply by 100)
  uint16_t voltage_int = (uint16_t)(voltage * 100);
  uint16_t current_int = (uint16_t)(current * 100);

  // Combine into 32-bit payload: voltage in upper 16 bits, current in lower 16 bits
  uint32_t payload = ((uint32_t)voltage_int << 16) | current_int;

  printTelnetf("Setting voltage: %.2fV (0x%04X), current: %.2fA (0x%04X)\n",
               voltage, voltage_int, current, current_int);
  printTelnetf("Combined payload: 0x%08X\n", payload);

  sendCommandWithPayload(slaveID, MAGIC_SET_OUTPUT, payload);
  pollSlave(slaveID);
}

void processReceivedData(uint8_t slaveID, uint8_t *buffer, size_t count)
{
  if (calculateCRC(buffer, count - 1) != buffer[count - 1])
  {
    printTelnetf("CRC fail!\n");
    return;
  }

  if (buffer[2] != (count - 4))
  { // count - (header + length byte + CRC)
    printTelnetf("Length invalid! Expected: %d, Got: %d\n", (count - 4), buffer[2]);
    return;
  }

  if (count < 7)
  {
    printTelnetf("Frame too short for magic words! Got %d bytes, need at least 7\n", count);
    return;
  }

  uint32_t magic = getMagic(buffer, count);

  if (magic == 0)
  {
    printTelnetf("Magic extraction failed!\n");
    return;
  }

  if (magic == MAGIC_INITIALIZE)
  {
    printTelnetf("PMU is uninitialized!\n");
    sendSimpleCommand(slaveID, MAGIC_INITIALIZE);
  }
  if (magic == MAGIC_OUTPUT_VOLTAGE)
  {
    parseOutputValues(slaveID, buffer, count);
  }
  return;
}

int8_t pollSlave(uint8_t slaveID)
{

  printTelnetf("Polling slave %d\n", slaveID);

  bus.flush();
  while (bus.available())
    bus.read();

  bus.write(0x20 + slaveID, EspSoftwareSerial::PARITY_MARK);

  unsigned long start = millis();
  while (!bus.available() && (millis() - start < 100))
  {
    delay(1);
  }

  if (!bus.available())
  {
    printTelnetf("Timeout.\n");
    return -1;
  }

  uint8_t buffer[96];
  size_t count = 0;
  unsigned long lastByteTime = millis();

  while (count < sizeof(buffer))
  {
    if (bus.available())
    {
      buffer[count++] = bus.read();
      lastByteTime = millis();
    }
    else
    {
      if (millis() - lastByteTime >= 4)
        break;
    }
  }

  bus.write(0x7F, EspSoftwareSerial::PARITY_SPACE);

  // Copy data to response
  if (count > 0)
  {
    printTelnetf("Received %d byte(s)\n", count);
  }

  if (count == 1 && buffer[0] == 0x73)
  {
    printTelnetf("Slave has nothing to say\n");
    return 0;
  }

  char hexString[count * 5];
  hexString[0] = '\0'; // Initialize as empty string

  for (size_t i = 0; i < count; i++)
  {
    char hexByte[6]; // "0x73 " + null terminator
    if (i == 0)
    {
      sprintf(hexByte, "0x%02X", buffer[i]);
    }
    else
    {
      sprintf(hexByte, " 0x%02X", buffer[i]);
    }
    strcat(hexString, hexByte);
  }

  printTelnetf("Data: %s\n", hexString);
  processReceivedData(slaveID, buffer, count);

  return count;
}

// UART Task running on core 1 - handles ALL software serial operations
void uartTask(void *parameter)
{
  // Initialize software serial on this core

  bus.begin(9600, SWSERIAL_8S1, 36, 4, false, 256, 256);
  bus.enableIntTx(false);

  UartCommandMsg cmd;
  UartResponseMsg response;

  unsigned long lastOutputVoltagePolled = 0;

  unsigned long lastVoltageSetAsked = 0;
  bool lastV = false;

  while (true)
  {
    for (int i = 0; i < 1; i++)
    {
      if (pollSlave(i) == 0)
      {
        if (millis() - lastOutputVoltagePolled > 1000)
        {
          lastOutputVoltagePolled = millis();

          // Send current command in round-robin sequence
          sendSimpleCommand(i, pollCommands[commandIndex]);
          pollSlave(i);

          // Move to next command for next iteration
          commandIndex = (commandIndex + 1) % numCommands;
        }
      }
      delay(300);
    }
    if (millis() - lastVoltageSetAsked > 5000)
    {
      lastVoltageSetAsked = millis();
      setVoltage(0, 50.00, 2.00);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Optimized WiFi and telnet setup
void optimizeNetworking()
{
  // TCP performance optimizations
  WiFi.setSleep(false); // Disable WiFi power saving for better performance
}

void setupTelnetOptimizations()
{
  if (telnetclient && telnetclient.connected())
  {
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

void handleTelnetCommand(char *buf)
{
  UartCommandMsg cmd;

  if (strcmp(buf, "reboot") == 0)
  {
    printTelnetfUrgent("Rebooting...\n");
    delay(300);
    ESP.restart();
  }
  else if (strcmp(buf, "wifi") == 0)
  {
    printTelnetfUrgent("====== WIFI INFO ======= \n");
    printTelnetfUrgent("Connected to: %s\n", WiFi.SSID().c_str());
    printTelnetfUrgent("Signal: %d\n", WiFi.RSSI());
  }
  else if (strcmp(buf, "config") == 0)
  {
    printTelnetfUrgent("====== CONFIGURATION ======= \n");
    printTelnetfUrgent("MQTT: %s\n", mqttConfig.enabled ? "enabled" : "disabled");
    printTelnetfUrgent("MQTT Server: %s:%d\n", mqttConfig.serverIP.c_str(), mqttConfig.port);
    printTelnetfUrgent("MQTT Topic: %s\n", mqttConfig.baseTopic.c_str());
    printTelnetfUrgent("MQTT User: %s\n", mqttConfig.username.c_str());
    printTelnetfUrgent("PMU Clients: %d\n", pmuConfig.expectedClients);
    printTelnetfUrgent("PMU Output: %.1fV %.1fA\n", pmuConfig.outputVoltage, pmuConfig.outputCurrent);
  }
  else if (strcmp(buf, "mqtt_enable") == 0)
  {
    mqttConfig.enabled = true;
    saveMqttConfig();
    initMQTT();
    printTelnetfUrgent("MQTT enabled\n");
  }
  else if (strcmp(buf, "mqtt_disable") == 0)
  {
    mqttConfig.enabled = false;
    if (mqttClient.connected())
    {
      mqttClient.disconnect();
    }
    saveMqttConfig();
    printTelnetfUrgent("MQTT disabled\n");
  }
  else if (strcmp(buf, "test") == 0)
  {
    cmd.command = CMD_TEST;
    cmd.parameter = 0x20;
    xQueueSend(uartCommandQueue, &cmd, 0);
  }
  else if (strcmp(buf, "poll") == 0)
  {
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
  else if (strcmp(buf, "help") == 0)
  {
    printTelnetfUrgent("Commands: wifi, config, mqtt_enable, mqtt_disable, test, poll, reboot, help\n");
  }
  else
  {
    printTelnetf("Unknown command: %s\n", buf);
  }
}

// Response processor - checks for UART responses and forwards to telnet
void processUartResponses()
{
  UartResponseMsg response;
  // Check for responses without blocking
  while (xQueueReceive(uartResponseQueue, &response, 0) == pdTRUE)
  {
    printTelnetf("%s", response.message);
  }
}

void setup()
{
  delay(200);
  Serial.begin(115200);
  WiFi.onEvent(WiFiEvent);
  wifiConnect();

  // Create all queues
  telnetQueue = xQueueCreate(TELNET_QUEUE_SIZE, sizeof(TelnetMessage));
  uartCommandQueue = xQueueCreate(UART_COMMAND_QUEUE_SIZE, sizeof(UartCommandMsg));
  uartResponseQueue = xQueueCreate(UART_RESPONSE_QUEUE_SIZE, sizeof(UartResponseMsg));

  if (!telnetQueue || !uartCommandQueue || !uartResponseQueue)
  {
    Serial.println("Failed to create queues!");
    return;
  }

  // Create UART task on core 1 (isolated from WiFi/telnet)
  xTaskCreatePinnedToCore(
      uartTask,        // Task function
      "UartTask",      // Task name
      8192,            // Stack size
      NULL,            // Parameters
      5,               // Highest priority
      &uartTaskHandle, // Task handle
      1                // Core 1 - isolated from WiFi
  );

  // Create telnet output task on core 0
  xTaskCreatePinnedToCore(
      telnetOutputTask,  // Task function
      "TelnetOutput",    // Task name
      8192,              // Stack size
      NULL,              // Parameters
      3,                 // High priority
      &telnetTaskHandle, // Task handle
      0                  // Core 0 - with WiFi
  );

  // Initialize file system
  initLittleFS();

  // Load configurations
  loadMqttConfig();
  loadPmuConfig();

  // Initialize MQTT with loaded config
  initMQTT();

  // Initialize web server
  initWebServer();

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
    while (telnetclient.available())
    {
      int c = telnetclient.read();

      // Filter out telnet control sequences and non-printable characters
      if (c == 255)
      { // IAC (Interpret As Command) - skip telnet control sequences
        // Skip the next 2 bytes of telnet control sequence
        if (telnetclient.available())
          telnetclient.read();
        if (telnetclient.available())
          telnetclient.read();
        continue;
      }

      if (c >= 32 && c <= 126)
      { // Only accept printable ASCII characters
        if (promptIndex < PROMPT_BUFFER_SIZE - 1)
        {
          promptBuffer[promptIndex++] = (char)c;
        }
      }
      else if (c == '\r' || c == '\n')
      {
        if (promptIndex > 0)
        {
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
  handleMQTTReconnect();
  // Process MQTT messages
  if (mqttClient.connected())
  {
    mqttClient.loop();
  }
}
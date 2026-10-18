/*
 * ESP32-Codex-Voice-Terminal — Stage 5 LAN heartbeat client
 * Target: ESP32-S3 N16R8 / Arduino board "ESP32S3 Dev Module"
 *
 * Connects to the local Wi-Fi network and POSTs a JSON heartbeat to the
 * Windows Python service every 5 seconds. Only WiFi/HTTPClient from the
 * Arduino-ESP32 core are used; no external libraries.
 *
 * No OLED/audio/buttons/RGB LED are used in this test.
 *
 * Wi-Fi credentials are read from ../stage2_wifi_test/wifi_config.h.
 * The service URL is read from the local lan_config.h (gitignored).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

#if __has_include("../stage2_wifi_test/wifi_config.h")
#include "../stage2_wifi_test/wifi_config.h"
#define WIFI_CONFIG_PRESENT 1
#else
#define WIFI_CONFIG_PRESENT 0
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

#if __has_include("lan_config.h")
#include "lan_config.h"
#define LAN_CONFIG_PRESENT 1
#else
#define LAN_CONFIG_PRESENT 0
#define SERVICE_URL ""
#endif

constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long CONNECT_TIMEOUT_MS = 15000UL;
constexpr unsigned long CONNECT_RETRY_DELAY_MS = 5000UL;
constexpr uint8_t MAX_CONNECT_ATTEMPTS = 3;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 5000UL;
constexpr unsigned long RECONNECT_INTERVAL_MS = 10000UL;

bool fatalConfigError = false;
bool wifiReady = false;
unsigned long lastHeartbeatMs = 0;
unsigned long lastConnectAttemptMs = 0;

bool connectWiFi() {
  if (WIFI_SSID[0] == '\0') {
    Serial.println("WiFi: SSID is empty; fill in wifi_config.h.");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  for (uint8_t attempt = 1; attempt <= MAX_CONNECT_ATTEMPTS; ++attempt) {
    Serial.printf("WiFi: attempt %u/%u\n", attempt, MAX_CONNECT_ATTEMPTS);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < CONNECT_TIMEOUT_MS) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi: connected; IP=%s RSSI=%d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return true;
    }
    Serial.printf("WiFi: attempt %u failed; status=%d\n", attempt, WiFi.status());
    WiFi.disconnect(false, false);
    if (attempt < MAX_CONNECT_ATTEMPTS) {
      delay(CONNECT_RETRY_DELAY_MS);
    }
  }
  return false;
}

void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HTTP: skipped; WiFi is not connected.");
    return;
  }

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String deviceId = "esp32-s3-" + mac;
  deviceId.toLowerCase();

  String body = "{";
  body += "\"protocol_version\":1,";
  body += "\"device_id\":\"" + deviceId + "\",";
  body += "\"message_type\":\"heartbeat\",";
  body += "\"uptime_s\":" + String(millis() / 1000UL) + ",";
  body += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  body += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  body += "\"firmware\":\"stage5_lan_client\"";
  body += "}";

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, SERVICE_URL)) {
    Serial.println("HTTP: begin failed.");
    return;
  }
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  if (code > 0) {
    const String response = http.getString();
    Serial.printf("HTTP %d: %s\n", code, response.c_str());
  } else {
    Serial.printf("HTTP error: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  unsigned long waitStart = millis();
  while (!Serial && (millis() - waitStart < 2000)) {
    delay(10);
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32-Codex-Voice-Terminal");
  Serial.println("Stage 5 LAN heartbeat client");
  Serial.printf("Chip: %s\n", ESP.getChipModel());

#if !WIFI_CONFIG_PRESENT
  Serial.println("WiFi: wifi_config.h not found; expected ../stage2_wifi_test/wifi_config.h.");
  Serial.println("Status: ERROR");
  fatalConfigError = true;
  return;
#endif

#if !LAN_CONFIG_PRESENT
  Serial.println("LAN: lan_config.h not found; copy lan_config.example.h and set SERVICE_URL.");
  Serial.println("Status: ERROR");
  fatalConfigError = true;
  return;
#endif

  Serial.printf("Service URL: %s\n", SERVICE_URL);
  Serial.println("Not using OLED/audio/buttons/RGB LED in this test.");

  wifiReady = connectWiFi();
  if (!wifiReady) {
    Serial.println("WiFi: initial connection failed; will retry in loop.");
  } else {
    sendHeartbeat();
    lastHeartbeatMs = millis();
  }
  lastConnectAttemptMs = millis();

  Serial.println("Status: RUNNING");
  Serial.println("========================================");
}

void loop() {
  if (fatalConfigError) {
    delay(1000);
    return;
  }

  const unsigned long now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatMs = now;
      sendHeartbeat();
    }
  } else {
    wifiReady = false;
    if (now - lastConnectAttemptMs >= RECONNECT_INTERVAL_MS) {
      lastConnectAttemptMs = now;
      Serial.println("WiFi: disconnected; reconnecting.");
      if (connectWiFi()) {
        sendHeartbeat();
        lastHeartbeatMs = millis();
      }
    }
  }
  delay(20);
}

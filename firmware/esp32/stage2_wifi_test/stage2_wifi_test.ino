/*
 * ESP32-Codex-Voice-Terminal — Stage 2 Wi-Fi test with OLED status
 * Target: ESP32-S3 N16R8 / Arduino board "ESP32S3 Dev Module"
 *
 * Wi-Fi: uses WiFi.h from Arduino-ESP32 Core and the local wifi_config.h.
 * OLED: SSD1306 128x32 on I2C (SDA GPIO41, SCL GPIO42, address 0x3C).
 *       Uses only Wire and raw SSD1306 commands; no external libraries.
 *
 * Serial logs remain available. If the OLED is not found, the Wi-Fi test
 * continues in serial-only mode.
 *
 * Failure-path build option:
 *   --build-property "compiler.cpp.extra_flags=-DWIFI_TEST_FORCE_FAILURE"
 * uses a nonexistent SSID without touching local wifi_config.h.
 */

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#define WIFI_CONFIG_PRESENT 1
#else
#define WIFI_CONFIG_PRESENT 0
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

#ifdef WIFI_TEST_FORCE_FAILURE
  #undef WIFI_SSID
  #undef WIFI_PASSWORD
  #define WIFI_SSID "ZCode-NoSuchAP-ForFailureTest"
  #define WIFI_PASSWORD "invalid-password-for-test"
#endif

#include <Wire.h>
#include <WiFi.h>
#include <limits.h>
#include <stdarg.h>

constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long CONNECT_TIMEOUT_MS = 15000UL;
constexpr unsigned long RETRY_DELAY_MS = 5000UL;
constexpr unsigned long STATUS_INTERVAL_MS = 1000UL;
constexpr uint8_t MAX_CONNECT_ATTEMPTS = 3;
// With USB CDC On Boot disabled, Serial and Serial0 both use UART0;
// keep the mirror off to avoid duplicate log lines.
constexpr bool MIRROR_LOG_TO_UART0 = false;

constexpr int OLED_SDA_PIN = 41;
constexpr int OLED_SCL_PIN = 42;
constexpr uint32_t I2C_FREQUENCY = 400000;
constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 32;
constexpr int OLED_PAGES = OLED_HEIGHT / 8;
constexpr uint8_t SSD1306_ADDRESSES[] = {0x3C, 0x3D};

uint8_t oledAddress = 0;
uint8_t framebuffer[OLED_WIDTH * OLED_PAGES];

enum class ConnectionPhase : uint8_t {
  WAITING,
  CONNECTING,
  RETRY_DELAY,
  CONNECTED,
  EXHAUSTED,
  NO_CREDENTIALS
};

ConnectionPhase phase = ConnectionPhase::WAITING;
uint8_t connectAttempts = 0;
unsigned long phaseStartedAt = 0;
unsigned long lastStatusAt = 0;
unsigned long lastReportSeconds = ULONG_MAX;

struct Glyph {
  char ch;
  uint8_t columns[5];
};

// 5x7 column-major font for the status strings used by this test.
const Glyph FONT[] = {
  {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
  {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
  {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
  {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
  {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
  {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
  {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
  {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
  {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
  {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
  {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
  {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
  {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
  {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
  {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
  {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
  {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
  {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
  {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
  {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
  {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
  {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
  {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
  {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
  {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
  {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
  {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
  {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
  {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
  {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
  {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
  {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
  {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
  {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
};

bool i2cProbe(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool oledWriteCommands(const uint8_t *commands, size_t count) {
  Wire.beginTransmission(oledAddress);
  Wire.write(0x00);
  for (size_t i = 0; i < count; ++i) {
    Wire.write(commands[i]);
  }
  return Wire.endTransmission() == 0;
}

bool ssd1306Init() {
  const uint8_t initSequence[] = {
    0xAE,        // display off
    0xD5, 0x80,  // clock divide ratio
    0xA8, 0x1F,  // multiplex ratio: 32 rows
    0xD3, 0x00,  // display offset
    0x40,        // start line 0
    0x8D, 0x14,  // charge pump on
    0x20, 0x02,  // page addressing mode
    0xA1,        // segment remap
    0xC8,        // COM scan direction remapped
    0xDA, 0x02,  // COM pins for 128x32
    0x81, 0x8F,  // contrast
    0xD9, 0xF1,  // pre-charge period
    0xDB, 0x40,  // VCOMH deselect level
    0xA4,        // display follows RAM content
    0xA6,        // normal display
    0xAF,        // display on
  };
  return oledWriteCommands(initSequence, sizeof(initSequence));
}

bool oledDisplay() {
  for (int page = 0; page < OLED_PAGES; ++page) {
    Wire.beginTransmission(oledAddress);
    Wire.write(0x00);
    Wire.write(0xB0 | page);
    Wire.write(0x00);
    Wire.write(0x10);
    if (Wire.endTransmission() != 0) {
      return false;
    }

    for (int offset = 0; offset < OLED_WIDTH; offset += 32) {
      const int chunk = min(32, OLED_WIDTH - offset);
      Wire.beginTransmission(oledAddress);
      Wire.write(0x40);
      Wire.write(&framebuffer[page * OLED_WIDTH + offset], chunk);
      if (Wire.endTransmission() != 0) {
        return false;
      }
    }
  }
  return true;
}

void oledClear() {
  memset(framebuffer, 0, sizeof(framebuffer));
}

void oledSetPixel(int x, int y, bool on) {
  if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
    return;
  }
  const int index = (y / 8) * OLED_WIDTH + x;
  const uint8_t mask = 1 << (y % 8);
  if (on) {
    framebuffer[index] |= mask;
  } else {
    framebuffer[index] &= ~mask;
  }
}

void oledDrawChar(char ch, int x, int y, int scale) {
  const Glyph *glyph = nullptr;
  for (const Glyph &candidate : FONT) {
    if (candidate.ch == ch) {
      glyph = &candidate;
      break;
    }
  }
  if (glyph == nullptr) {
    return;
  }

  for (int col = 0; col < 5; ++col) {
    const uint8_t bits = glyph->columns[col];
    for (int row = 0; row < 7; ++row) {
      if ((bits & (1 << row)) == 0) {
        continue;
      }
      for (int dy = 0; dy < scale; ++dy) {
        for (int dx = 0; dx < scale; ++dx) {
          oledSetPixel(x + col * scale + dx, y + row * scale + dy, true);
        }
      }
    }
  }
}

void oledDrawText(const char *text, int x, int y, int scale) {
  for (int i = 0; text[i] != '\0'; ++i) {
    oledDrawChar(text[i], x + i * 6 * scale, y, scale);
  }
}

bool oledInit() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN, I2C_FREQUENCY);
  delay(50);

  for (uint8_t address : SSD1306_ADDRESSES) {
    if (i2cProbe(address)) {
      oledAddress = address;
      break;
    }
  }
  if (oledAddress == 0) {
    return false;
  }
  if (!ssd1306Init()) {
    oledAddress = 0;
    return false;
  }
  oledClear();
  return oledDisplay();
}

void logMessage(const char *message) {
  Serial.println(message);
  if (MIRROR_LOG_TO_UART0) {
    Serial0.println(message);
  }
}

void logPrintf(const char *format, ...) {
  char message[256];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  Serial.print(message);
  if (MIRROR_LOG_TO_UART0) {
    Serial0.print(message);
  }
}

const char *wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAILABLE";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

void updateOledStatus() {
  if (oledAddress == 0) {
    return;
  }

  oledClear();
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    oledDrawText("WIFI OK", 4, 2, 2);
    char line[24];
    const String ip = WiFi.localIP().toString();
    snprintf(line, sizeof(line), "IP %s", ip.c_str());
    oledDrawText(line, 4, 18, 1);
    snprintf(line, sizeof(line), "RSSI %d", WiFi.RSSI());
    oledDrawText(line, 4, 25, 1);
  } else if (phase == ConnectionPhase::EXHAUSTED || phase == ConnectionPhase::NO_CREDENTIALS) {
    oledDrawText("WIFI FAIL", 4, 2, 2);
    if (phase == ConnectionPhase::NO_CREDENTIALS) {
      oledDrawText("NO CFG", 4, 18, 1);
    } else {
      char line[16];
      snprintf(line, sizeof(line), "TRY %u/%u", (unsigned)connectAttempts, (unsigned)MAX_CONNECT_ATTEMPTS);
      oledDrawText(line, 4, 18, 1);
    }
  } else {
    oledDrawText("WIFI WAIT", 4, 2, 2);
    char line[16];
    snprintf(line, sizeof(line), "TRY %u/%u", (unsigned)connectAttempts, (unsigned)MAX_CONNECT_ATTEMPTS);
    oledDrawText(line, 4, 18, 1);
  }
  oledDisplay();
}

void beginConnectionAttempt() {
  ++connectAttempts;
  logPrintf("WiFi: starting attempt %u/%u\n", connectAttempts, MAX_CONNECT_ATTEMPTS);
  phase = ConnectionPhase::CONNECTING;
  phaseStartedAt = millis();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void printStatus(unsigned long uptimeSeconds) {
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    logPrintf("Status: RUNNING | Uptime: %lu s | WiFi: %s | Attempt: %u/%u | IP: %s | RSSI: %d dBm\n",
              uptimeSeconds, wifiStatusName(status), connectAttempts, MAX_CONNECT_ATTEMPTS,
              WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    logPrintf("Status: RUNNING | Uptime: %lu s | WiFi: %s | Attempt: %u/%u\n",
              uptimeSeconds, wifiStatusName(status), connectAttempts, MAX_CONNECT_ATTEMPTS);
  }
  updateOledStatus();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
#if MIRROR_LOG_TO_UART0
  Serial0.begin(SERIAL_BAUD);
#endif
  delay(300);

  logMessage("");
  logMessage("========================================");
  logMessage("ESP32-Codex-Voice-Terminal");
  logMessage("Stage 2 Wi-Fi test with OLED status");
#ifdef WIFI_TEST_FORCE_FAILURE
  logMessage("WiFi: FAILURE TEST MODE (nonexistent SSID).");
#endif
  logPrintf("Chip: %s\n", ESP.getChipModel());
  logPrintf("Serial baud: %lu\n", SERIAL_BAUD);
  logPrintf("OLED: SDA GPIO%d, SCL GPIO%d\n", OLED_SDA_PIN, OLED_SCL_PIN);

  if (oledInit()) {
    logPrintf("OLED: SSD1306 ready at 0x%02X.\n", oledAddress);
  } else {
    logMessage("OLED: not found; continuing in serial-only mode.");
  }

#if !WIFI_CONFIG_PRESENT
  logMessage("WiFi: wifi_config.h is missing; copy wifi_config.example.h and fill in local credentials.");
  phase = ConnectionPhase::NO_CREDENTIALS;
#else
  if (WIFI_SSID[0] == '\0') {
    logMessage("WiFi: SSID is empty; fill in wifi_config.h. No connection attempt made.");
    phase = ConnectionPhase::NO_CREDENTIALS;
  } else {
    logMessage("WiFi: waiting 1 s for the power rail to stabilize.");
    delay(1000);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    updateOledStatus();
    beginConnectionAttempt();
  }
#endif

  updateOledStatus();
  logMessage("Status: RUNNING");
  logMessage("========================================");
}

void loop() {
  const unsigned long now = millis();

  if (phase == ConnectionPhase::CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      phase = ConnectionPhase::CONNECTED;
      logPrintf("WiFi: connected; state=%s; IP=%s; RSSI=%d dBm\n",
                wifiStatusName(WiFi.status()), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else if (now - phaseStartedAt >= CONNECT_TIMEOUT_MS) {
      logPrintf("WiFi: attempt %u timed out; state=%s\n", connectAttempts,
                wifiStatusName(WiFi.status()));
      WiFi.disconnect(false, false);
      if (connectAttempts < MAX_CONNECT_ATTEMPTS) {
        phase = ConnectionPhase::RETRY_DELAY;
        phaseStartedAt = now;
        logPrintf("WiFi: retrying in %lu ms\n", RETRY_DELAY_MS);
      } else {
        phase = ConnectionPhase::EXHAUSTED;
        logMessage("WiFi: retry limit reached; no more attempts until reset.");
      }
    }
  } else if (phase == ConnectionPhase::RETRY_DELAY && now - phaseStartedAt >= RETRY_DELAY_MS) {
    beginConnectionAttempt();
  } else if (phase == ConnectionPhase::CONNECTED && WiFi.status() != WL_CONNECTED) {
    logPrintf("WiFi: connection lost; state=%s. Initial retry limit has been reached; reset to retry.\n",
              wifiStatusName(WiFi.status()));
    phase = ConnectionPhase::EXHAUSTED;
  }

  if (now - lastStatusAt >= STATUS_INTERVAL_MS) {
    lastStatusAt = now;
    const unsigned long uptimeSeconds = now / 1000UL;
    if (uptimeSeconds != lastReportSeconds) {
      lastReportSeconds = uptimeSeconds;
      printStatus(uptimeSeconds);
    }
  }
  delay(10);
}

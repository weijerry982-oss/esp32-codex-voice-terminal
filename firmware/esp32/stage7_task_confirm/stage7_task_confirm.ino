/*
 * ESP32-Codex-Voice-Terminal — Stage 7 task confirmation
 * Target: ESP32-S3 N16R8 / Arduino board "ESP32S3 Dev Module"
 *
 * Flow:
 *   GPIO0 (BOOT) hold/speak/release -> POST WAV to /api/v1/tasks/request
 *   OLED alternates the Chinese task summary and TASK <id>;
 *   confirm only for the task currently displayed.
 *   GPIO39 = CONFIRM (only for the displayed task ID)
 *   GPIO40 = CANCEL
 *   Poll /api/v1/tasks/<id>; on done, GET /audio and play via MAX98357A.
 *
 * The device never sends an unconditional approval: it only sends the task ID
 * currently stored and displayed. The service also checks ID, current task and
 * 60 s expiry. Codex itself runs with its native sandbox/approval settings.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include "status_bitmaps.h"

#if __has_include("../stage2_wifi_test/wifi_config.h")
#include "../stage2_wifi_test/wifi_config.h"
#define WIFI_CONFIG_PRESENT 1
#else
#define WIFI_CONFIG_PRESENT 0
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

#if __has_include("voice_config.h")
#include "voice_config.h"
#define VOICE_CONFIG_PRESENT 1
#else
#define VOICE_CONFIG_PRESENT 0
#define SERVICE_BASE_URL ""
#define VOICE_KEY_PIN 0
#define CONFIRM_PIN 39
#define CANCEL_PIN 40
#endif

constexpr int MIC_BCLK_PIN = 5;
constexpr int MIC_WS_PIN = 4;
constexpr int MIC_DIN_PIN = 6;
constexpr int SPK_BCLK_PIN = 15;
constexpr int SPK_WS_PIN = 16;
constexpr int SPK_DOUT_PIN = 7;
constexpr int RGB_LED_PIN = 48;
constexpr uint8_t LED_BRIGHTNESS = 64;

constexpr int OLED_SDA_PIN = 41;
constexpr int OLED_SCL_PIN = 42;
constexpr uint32_t I2C_FREQUENCY = 400000;
constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 32;
constexpr int OLED_PAGES = OLED_HEIGHT / 8;
constexpr uint8_t SSD1306_ADDRESSES[] = {0x3C, 0x3D};

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t MAX_RECORD_SECONDS = 8;
constexpr size_t MAX_SAMPLES = SAMPLE_RATE * MAX_RECORD_SECONDS;
constexpr size_t MIN_SAMPLES = SAMPLE_RATE / 5;
constexpr size_t READ_CHUNK_SAMPLES = 256;
constexpr size_t MAX_REPLY_BYTES = 7 * 1024 * 1024;
constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long CONNECT_TIMEOUT_MS = 15000UL;
constexpr unsigned long CONNECT_RETRY_DELAY_MS = 5000UL;
constexpr uint8_t MAX_CONNECT_ATTEMPTS = 3;
constexpr unsigned long RECONNECT_INTERVAL_MS = 10000UL;
constexpr unsigned long TASK_TTL_MS = 55000UL;
constexpr unsigned long POLL_INTERVAL_MS = 3000UL;
constexpr unsigned long ANSWER_SCREEN_HOLD_MS = 10000UL;
constexpr unsigned long PROMPT_PAGE_INTERVAL_MS = 5000UL;

enum class TaskState : uint8_t {
  IDLE,
  WAIT,
  RUNNING,
  INTERACTIVE,
  DONE,
  ERROR,
  EXPIRED,
  BUSY
};

struct Button {
  uint8_t pin;
  bool stableState;
  bool lastReading;
  unsigned long lastChangeMs;
};

struct Glyph {
  char ch;
  uint8_t columns[5];
};

const Glyph FONT[] = {
  {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
  {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
  {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
  {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
  {'=', {0x14, 0x14, 0x14, 0x14, 0x14}},
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
  {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
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
  {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
  {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
  {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
  {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
  {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
  {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
  {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
  {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
  {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
  {'_', {0x40, 0x40, 0x40, 0x40, 0x40}},
  {'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
  {'a', {0x20, 0x54, 0x54, 0x54, 0x78}},
  {'b', {0x7F, 0x48, 0x44, 0x44, 0x38}},
  {'c', {0x38, 0x44, 0x44, 0x44, 0x20}},
  {'d', {0x38, 0x44, 0x44, 0x48, 0x7F}},
  {'e', {0x38, 0x54, 0x54, 0x54, 0x18}},
  {'f', {0x08, 0x7E, 0x09, 0x01, 0x02}},
  {'g', {0x0C, 0x52, 0x52, 0x52, 0x3E}},
  {'h', {0x7F, 0x08, 0x04, 0x04, 0x78}},
  {'i', {0x00, 0x44, 0x7D, 0x40, 0x00}},
  {'j', {0x20, 0x40, 0x44, 0x3D, 0x00}},
  {'k', {0x7F, 0x10, 0x28, 0x44, 0x00}},
  {'l', {0x00, 0x41, 0x7F, 0x40, 0x00}},
  {'m', {0x7C, 0x04, 0x18, 0x04, 0x78}},
  {'n', {0x7C, 0x08, 0x04, 0x04, 0x78}},
  {'o', {0x38, 0x44, 0x44, 0x44, 0x38}},
  {'p', {0x7C, 0x14, 0x14, 0x14, 0x08}},
  {'q', {0x08, 0x14, 0x14, 0x18, 0x7C}},
  {'r', {0x7C, 0x08, 0x04, 0x04, 0x08}},
  {'s', {0x48, 0x54, 0x54, 0x54, 0x20}},
  {'t', {0x04, 0x3F, 0x44, 0x40, 0x20}},
  {'u', {0x3C, 0x40, 0x40, 0x20, 0x7C}},
  {'v', {0x1C, 0x20, 0x40, 0x20, 0x1C}},
  {'w', {0x3C, 0x40, 0x30, 0x40, 0x3C}},
  {'x', {0x44, 0x28, 0x10, 0x28, 0x44}},
  {'y', {0x0C, 0x50, 0x50, 0x50, 0x3C}},
  {'z', {0x44, 0x64, 0x54, 0x4C, 0x44}},
};

I2SClass i2sMic;
I2SClass i2sSpk;

int32_t rawBuffer[READ_CHUNK_SAMPLES];
int16_t *recordBuffer = nullptr;
uint8_t *replyBuffer = nullptr;
size_t replyBufferCapacity = 0;
uint8_t framebuffer[OLED_WIDTH * OLED_PAGES];
uint8_t oledAddress = 0;

Button confirmButton{CONFIRM_PIN, HIGH, HIGH, 0};
Button cancelButton{CANCEL_PIN, HIGH, HIGH, 0};

TaskState taskState = TaskState::IDLE;
String pendingTaskId;
unsigned long taskCreatedMs = 0;
unsigned long lastPollMs = 0;
bool fatalConfigError = false;
unsigned long lastConnectAttemptMs = 0;
String currentSsid;
uint8_t screenBackup[OLED_WIDTH * OLED_PAGES];
bool screenBackupValid = false;
unsigned long answerScreenUntilMs = 0;
uint8_t promptScreen[OLED_WIDTH * OLED_PAGES];
bool promptScreenValid = false;
bool showingPromptPage = false;
unsigned long lastPendingPageMs = 0;

// ---------- OLED ----------

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
    0xAE, 0xD5, 0x80, 0xA8, 0x1F, 0xD3, 0x00, 0x40,
    0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x02,
    0x81, 0x8F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
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

void drawLines(const String &line1, const String &line2, const String &line3) {
  if (oledAddress == 0) {
    return;
  }
  oledClear();
  oledDrawText(line1.c_str(), 2, 2, 1);
  oledDrawText(line2.c_str(), 2, 12, 1);
  oledDrawText(line3.c_str(), 2, 22, 1);
  oledDisplay();
}

void showStatusBitmap(const uint8_t *bitmap) {
  if (oledAddress == 0) {
    return;
  }
  memcpy(framebuffer, bitmap, sizeof(framebuffer));
  oledDisplay();
}

String fittedSsidLine(const String &ssid) {
  const String value = ssid.length() > 0 ? ssid : String(WIFI_SSID);
  String line = "SSID:";
  for (size_t i = 0; i < value.length() && line.length() < 21; ++i) {
    const char ch = value[i];
    line += static_cast<uint8_t>(ch) < 0x80 ? ch : '?';
  }
  if (line.length() > 21) {
    line = line.substring(0, 21);
  }
  return line;
}

void showWifiStatus(const char *title, const String &ssid, const String &detail) {
  drawLines(title, fittedSsidLine(ssid), detail);
}

void showIdle() {
  screenBackupValid = false;
  answerScreenUntilMs = 0;
  drawLines("VOICE TASK", "BOOT=TALK", fittedSsidLine(currentSsid));
}

void showPending() {
  screenBackupValid = false;
  answerScreenUntilMs = 0;
  drawLines("TASK " + pendingTaskId, "CHECK CHATGPT", "39=SEND 40=CLEAR");
}

void showPromptScreen() {
  if (!promptScreenValid) {
    showPending();
    return;
  }
  memcpy(framebuffer, promptScreen, sizeof(framebuffer));
  oledDisplay();
}

void showStatus(const char *status) {
  screenBackupValid = false;
  answerScreenUntilMs = 0;
  drawLines("TASK " + pendingTaskId, status, "");
}

void restoreScreen() {
  if (taskState == TaskState::WAIT) {
    lastPendingPageMs = millis();
    if (promptScreenValid) {
      showPromptScreen();
      showingPromptPage = true;
    } else {
      showPending();
      showingPromptPage = false;
    }
    return;
  }
  if (taskState == TaskState::RUNNING || taskState == TaskState::INTERACTIVE) {
    showStatus(taskState == TaskState::RUNNING ? "RUNNING" : "CODEX WIN");
    return;
  }
  if (screenBackupValid) {
    memcpy(framebuffer, screenBackup, sizeof(framebuffer));
    oledDisplay();
    answerScreenUntilMs = millis() + ANSWER_SCREEN_HOLD_MS;
    return;
  }
  showIdle();
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

// ---------- Wi-Fi / audio ----------

String deviceId() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String id = "esp32-s3-" + mac;
  id.toLowerCase();
  return id;
}

bool connectWiFi() {
  if (WIFI_SSID[0] == '\0') {
    Serial.println("WiFi: SSID is empty; fill in wifi_config.h.");
    showWifiStatus("WIFI ERROR", "", "NO SSID");
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  showWifiStatus("WIFI CONNECT", currentSsid, "PLEASE WAIT");
  for (uint8_t attempt = 1; attempt <= MAX_CONNECT_ATTEMPTS; ++attempt) {
    Serial.printf("WiFi: attempt %u/%u\n", attempt, MAX_CONNECT_ATTEMPTS);
    const String detail = String("ATTEMPT ") + String(attempt) + "/" + String(MAX_CONNECT_ATTEMPTS);
    showWifiStatus("WIFI CONNECT", currentSsid, detail);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < CONNECT_TIMEOUT_MS) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
      currentSsid = WiFi.SSID();
      if (currentSsid.length() == 0) {
        currentSsid = WIFI_SSID;
      }
      Serial.printf("WiFi: connected; SSID=%s IP=%s RSSI=%d dBm\n",
                    currentSsid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      showWifiStatus("WIFI OK", currentSsid, WiFi.localIP().toString());
      delay(2500);
      return true;
    }
    Serial.printf("WiFi: attempt %u failed; status=%d\n", attempt, WiFi.status());
    WiFi.disconnect(false, false);
    if (attempt < MAX_CONNECT_ATTEMPTS) {
      delay(CONNECT_RETRY_DELAY_MS);
    }
  }
  showWifiStatus("WIFI LOST", currentSsid, "RETRY 10S");
  return false;
}

void writeLe16(uint8_t *data, uint16_t value) {
  data[0] = value & 0xFF;
  data[1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t *data, uint32_t value) {
  data[0] = value & 0xFF;
  data[1] = (value >> 8) & 0xFF;
  data[2] = (value >> 16) & 0xFF;
  data[3] = (value >> 24) & 0xFF;
}

uint16_t readLe16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

size_t buildWav(const int16_t *samples, size_t count, uint8_t *out) {
  const uint32_t dataBytes = count * 2;
  memcpy(out, "RIFF", 4);
  writeLe32(out + 4, 36 + dataBytes);
  memcpy(out + 8, "WAVE", 4);
  memcpy(out + 12, "fmt ", 4);
  writeLe32(out + 16, 16);
  writeLe16(out + 20, 1);
  writeLe16(out + 22, 1);
  writeLe32(out + 24, SAMPLE_RATE);
  writeLe32(out + 28, SAMPLE_RATE * 2);
  writeLe16(out + 32, 2);
  writeLe16(out + 34, 16);
  memcpy(out + 36, "data", 4);
  writeLe32(out + 40, dataBytes);
  memcpy(out + 44, samples, dataBytes);
  return 44 + dataBytes;
}

size_t recordUtterance() {
  size_t total = 0;
  Serial.println("Recording... release the key to create the task.");
  while (digitalRead(VOICE_KEY_PIN) == LOW && total < MAX_SAMPLES) {
    const size_t bytes = i2sMic.readBytes(reinterpret_cast<char *>(rawBuffer), sizeof(rawBuffer));
    const size_t count = bytes / sizeof(int32_t);
    for (size_t i = 0; i < count && total < MAX_SAMPLES; ++i) {
      recordBuffer[total++] = static_cast<int16_t>(rawBuffer[i] >> 16);
    }
  }
  Serial.printf("Recorded %u samples (%.2f s)\n", static_cast<unsigned>(total),
                total / static_cast<float>(SAMPLE_RATE));
  return total;
}

bool playWav(const uint8_t *data, size_t length) {
  if (length < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
    Serial.println("Playback: invalid WAV header.");
    return false;
  }
  int sampleRate = 0;
  int channels = 0;
  int bits = 0;
  size_t dataOffset = 0;
  size_t dataSize = 0;
  size_t pos = 12;
  while (pos + 8 <= length) {
    const uint32_t chunkSize = readLe32(data + pos + 4);
    if (memcmp(data + pos, "fmt ", 4) == 0 && chunkSize >= 16 && pos + 8 + 16 <= length) {
      channels = readLe16(data + pos + 10);
      sampleRate = readLe32(data + pos + 12);
      bits = readLe16(data + pos + 22);
    } else if (memcmp(data + pos, "data", 4) == 0) {
      dataOffset = pos + 8;
      dataSize = chunkSize;
      if (dataSize > length - dataOffset) {
        dataSize = length - dataOffset;
      }
      break;
    }
    pos += 8 + chunkSize + (chunkSize & 1);
  }
  if (sampleRate <= 0 || bits != 16 || channels < 1 || channels > 2 ||
      dataOffset == 0 || dataSize < 2) {
    Serial.println("Playback: unsupported WAV.");
    return false;
  }
  dataSize &= ~1UL;
  i2sSpk.configureTX(static_cast<uint32_t>(sampleRate), I2S_DATA_BIT_WIDTH_16BIT,
                     I2S_SLOT_MODE_STEREO);
  const int16_t *pcm = reinterpret_cast<const int16_t *>(data + dataOffset);
  const size_t frames = dataSize / (2 * channels);
  int16_t stereo[512];
  size_t frame = 0;
  while (frame < frames) {
    size_t chunkFrames = frames - frame;
    if (chunkFrames > 256) {
      chunkFrames = 256;
    }
    for (size_t i = 0; i < chunkFrames; ++i) {
      if (channels == 1) {
        const int16_t sample = pcm[frame + i];
        stereo[i * 2] = sample;
        stereo[i * 2 + 1] = sample;
      } else {
        stereo[i * 2] = pcm[(frame + i) * 2];
        stereo[i * 2 + 1] = pcm[(frame + i) * 2 + 1];
      }
    }
    i2sSpk.write(stereo, chunkFrames * 4);
    frame += chunkFrames;
  }
  Serial.printf("Playback done: %u frames at %d Hz.\n", static_cast<unsigned>(frames), sampleRate);
  return true;
}

// ---------- HTTP / task state ----------

String extractJsonString(const String &json, const char *key) {
  const String pattern = "\"" + String(key) + "\"";
  const int keyPos = json.indexOf(pattern);
  if (keyPos < 0) {
    return "";
  }
  const int colon = json.indexOf(':', keyPos + pattern.length());
  if (colon < 0) {
    return "";
  }
  const int firstQuote = json.indexOf('"', colon + 1);
  if (firstQuote < 0) {
    return "";
  }
  const int secondQuote = json.indexOf('"', firstQuote + 1);
  if (secondQuote < 0) {
    return "";
  }
  return json.substring(firstQuote + 1, secondQuote);
}

int requestTask(uint8_t *wav, size_t wavSize, String &taskId, String &body) {
  WiFiClient client;
  HTTPClient http;
  const String url = String(SERVICE_BASE_URL) + "/api/v1/tasks/request?mode=read_only";
  if (!http.begin(client, url)) {
    return -1;
  }
  http.setTimeout(20000);
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Device-Id", deviceId());
  const int code = http.POST(wav, wavSize);
  body = http.getString();
  http.end();
  taskId = extractJsonString(body, "task_id");
  return code;
}

int taskAction(const String &taskId, const char *action, String &body) {
  WiFiClient client;
  HTTPClient http;
  const String url = String(SERVICE_BASE_URL) + "/api/v1/tasks/" + taskId + "/" + action;
  if (!http.begin(client, url)) {
    return -1;
  }
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(String(""));
  body = http.getString();
  http.end();
  return code;
}

int fetchTaskStatus(const String &taskId, String &status, String &body) {
  WiFiClient client;
  HTTPClient http;
  const String url = String(SERVICE_BASE_URL) + "/api/v1/tasks/" + taskId;
  if (!http.begin(client, url)) {
    return -1;
  }
  http.setTimeout(15000);
  const int code = http.GET();
  body = http.getString();
  http.end();
  status = extractJsonString(body, "status");
  return code;
}

bool fetchAndPlayTaskAudio(const String &taskId) {
  WiFiClient client;
  HTTPClient http;
  const String url = String(SERVICE_BASE_URL) + "/api/v1/tasks/" + taskId + "/audio";
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(30000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength <= 44 || replyBufferCapacity == 0 ||
      contentLength > static_cast<int>(replyBufferCapacity)) {
    Serial.printf("Task audio rejected: content_length=%d max=%u\n",
                  contentLength, static_cast<unsigned>(replyBufferCapacity));
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  size_t total = 0;
  const unsigned long start = millis();
  while (total < static_cast<size_t>(contentLength) && millis() - start < 30000) {
    const size_t available = stream->available();
    if (available > 0) {
      const size_t remaining = static_cast<size_t>(contentLength) - total;
      const size_t chunk = available < remaining ? available : remaining;
      total += stream->readBytes(replyBuffer + total, chunk);
    } else {
      delay(1);
    }
  }
  http.end();
  if (total != static_cast<size_t>(contentLength)) {
    Serial.printf("Task audio download incomplete: %u/%d\n",
                  static_cast<unsigned>(total), contentLength);
    return false;
  }
  return playWav(replyBuffer, total);
}

bool fetchTaskScreen(const String &taskId) {
  WiFiClient client;
  HTTPClient http;
  const String url = String(SERVICE_BASE_URL) + "/api/v1/tasks/" + taskId + "/screen";
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(15000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength != static_cast<int>(sizeof(framebuffer))) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  size_t total = 0;
  const unsigned long start = millis();
  while (total < sizeof(framebuffer) && millis() - start < 5000) {
    const size_t available = stream->available();
    if (available > 0) {
      const size_t remaining = sizeof(framebuffer) - total;
      const size_t chunk = available < remaining ? available : remaining;
      total += stream->readBytes(framebuffer + total, chunk);
    } else {
      delay(1);
    }
  }
  http.end();
  if (total != sizeof(framebuffer)) {
    return false;
  }
  if (!oledDisplay()) {
    return false;
  }
  memcpy(screenBackup, framebuffer, sizeof(framebuffer));
  screenBackupValid = true;
  return true;
}

bool fetchTaskPrompt(const String &taskId) {
  WiFiClient client;
  HTTPClient http;
  const String url = String(SERVICE_BASE_URL) + "/api/v1/tasks/" + taskId + "/prompt";
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(15000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength != static_cast<int>(sizeof(framebuffer))) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  size_t total = 0;
  const unsigned long start = millis();
  while (total < sizeof(framebuffer) && millis() - start < 5000) {
    const size_t available = stream->available();
    if (available > 0) {
      const size_t remaining = sizeof(framebuffer) - total;
      const size_t chunk = available < remaining ? available : remaining;
      total += stream->readBytes(framebuffer + total, chunk);
    } else {
      delay(1);
    }
  }
  http.end();
  if (total != sizeof(framebuffer)) {
    return false;
  }
  if (!oledDisplay()) {
    return false;
  }
  memcpy(promptScreen, framebuffer, sizeof(promptScreen));
  promptScreenValid = true;
  return true;
}

bool updateButton(Button &button, unsigned long now) {
  const bool reading = digitalRead(button.pin);
  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.lastChangeMs = now;
  }
  if (now - button.lastChangeMs < 30 || reading == button.stableState) {
    return false;
  }
  button.stableState = reading;
  return reading == LOW;
}

void clearTask() {
  pendingTaskId = "";
  taskState = TaskState::IDLE;
  showIdle();
}

void setLed(uint8_t red, uint8_t green, uint8_t blue) {
  rgbLedWrite(RGB_LED_PIN, red, green, blue);
}

void blinkLed(uint8_t red, uint8_t green, uint8_t blue, uint8_t times,
              unsigned long intervalMs) {
  for (uint8_t i = 0; i < times; ++i) {
    setLed(red, green, blue);
    delay(intervalMs);
    setLed(0, 0, 0);
    delay(intervalMs);
  }
}

// ---------- setup / loop ----------

void setup() {
  Serial.begin(SERIAL_BAUD);
  unsigned long waitStart = millis();
  while (!Serial && (millis() - waitStart < 2000)) {
    delay(10);
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32-Codex-Voice-Terminal");
  Serial.println("Stage 7 task confirmation");
  Serial.printf("Chip: %s\n", ESP.getChipModel());

#if !WIFI_CONFIG_PRESENT
  Serial.println("WiFi: wifi_config.h not found; expected ../stage2_wifi_test/wifi_config.h.");
  Serial.println("Status: ERROR");
  fatalConfigError = true;
  return;
#endif
#if !VOICE_CONFIG_PRESENT
  Serial.println("Task: voice_config.h not found; copy voice_config.example.h.");
  Serial.println("Status: ERROR");
  fatalConfigError = true;
  return;
#endif

  Serial.printf("Service: %s\n", SERVICE_BASE_URL);
  Serial.printf("Voice key: GPIO%d; confirm: GPIO%d; cancel: GPIO%d\n",
                VOICE_KEY_PIN, CONFIRM_PIN, CANCEL_PIN);
  Serial.println("OLED alternates the task summary and task ID; confirm only after checking it.");

  currentSsid = WIFI_SSID;
  if (!oledInit()) {
    Serial.println("OLED: initialization failed; task confirmation requires the display.");
    Serial.println("Status: ERROR");
    fatalConfigError = true;
    return;
  }

  if (!connectWiFi()) {
    Serial.println("WiFi: initial connection failed; will retry in loop.");
  }
  lastConnectAttemptMs = millis();

  if (!i2sMic.setPort(I2S_NUM_0)) {
    Serial.println("I2S mic: setPort failed.");
    fatalConfigError = true;
    return;
  }
  i2sMic.setPins(MIC_BCLK_PIN, MIC_WS_PIN, -1, MIC_DIN_PIN);
  if (!i2sMic.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                    I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("I2S mic: initialization failed.");
    fatalConfigError = true;
    return;
  }

  if (!i2sSpk.setPort(I2S_NUM_1)) {
    Serial.println("I2S speaker: setPort failed.");
    fatalConfigError = true;
    return;
  }
  i2sSpk.setPins(SPK_BCLK_PIN, SPK_WS_PIN, SPK_DOUT_PIN, -1);
  if (!i2sSpk.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_STEREO)) {
    Serial.println("I2S speaker: initialization failed.");
    fatalConfigError = true;
    return;
  }

  recordBuffer = static_cast<int16_t *>(ps_malloc(MAX_SAMPLES * sizeof(int16_t)));
  static const size_t replyCandidates[] = {
    MAX_REPLY_BYTES,
    6 * 1024 * 1024,
    5 * 1024 * 1024,
    4 * 1024 * 1024,
    2 * 1024 * 1024,
  };
  for (const size_t candidate : replyCandidates) {
    replyBuffer = static_cast<uint8_t *>(ps_malloc(candidate));
    if (replyBuffer != nullptr) {
      replyBufferCapacity = candidate;
      break;
    }
  }
  if (recordBuffer == nullptr || replyBuffer == nullptr) {
    Serial.println("PSRAM: allocation failed.");
    fatalConfigError = true;
    return;
  }
  Serial.printf("PSRAM: record=%u reply=%u\n",
                static_cast<unsigned>(MAX_SAMPLES * sizeof(int16_t)),
                static_cast<unsigned>(replyBufferCapacity));

  pinMode(VOICE_KEY_PIN, INPUT_PULLUP);
  pinMode(CONFIRM_PIN, INPUT_PULLUP);
  pinMode(CANCEL_PIN, INPUT_PULLUP);
  setLed(0, 0, 0);
  if (WiFi.status() == WL_CONNECTED) {
    showIdle();
  }
  Serial.println("Hold GPIO0, speak a task, release. Then confirm on GPIO39 or cancel on GPIO40.");
  Serial.println("Status: RUNNING");
  Serial.println("========================================");
}

void loop() {
  if (fatalConfigError) {
    delay(1000);
    return;
  }

  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED && now - lastConnectAttemptMs >= RECONNECT_INTERVAL_MS) {
    lastConnectAttemptMs = now;
    Serial.printf("WiFi: disconnected; reconnecting. SSID=%s\n", currentSsid.c_str());
    showWifiStatus("WIFI LOST", currentSsid, "RECONNECTING");
    if (connectWiFi()) {
      restoreScreen();
    }
  }

  if (WiFi.status() == WL_CONNECTED && taskState == TaskState::IDLE &&
      answerScreenUntilMs != 0 &&
      static_cast<long>(millis() - answerScreenUntilMs) >= 0) {
    showIdle();
  }

  if (taskState == TaskState::WAIT && now - taskCreatedMs >= TASK_TTL_MS) {
    Serial.println("Task expired before confirmation; clearing ChatGPT draft.");
    String body;
    const int code = taskAction(pendingTaskId, "cancel", body);
    Serial.printf("Expiry cancel HTTP %d: %s\n", code, body.c_str());
    taskState = TaskState::EXPIRED;
    showStatus("EXPIRED");
    delay(3000);
    clearTask();
    return;
  }

  if (taskState == TaskState::WAIT && promptScreenValid &&
      now - lastPendingPageMs >= PROMPT_PAGE_INTERVAL_MS) {
    lastPendingPageMs = now;
    if (showingPromptPage) {
      showPending();
      showingPromptPage = false;
    } else {
      showPromptScreen();
      showingPromptPage = true;
    }
  }

  if (taskState == TaskState::WAIT) {
    if (updateButton(confirmButton, now)) {
      if (!promptScreenValid) {
        drawLines("TASK " + pendingTaskId, "NO SUMMARY", "40=CANCEL");
        return;
      }
      if (!showingPromptPage) {
        showPromptScreen();
        showingPromptPage = true;
        lastPendingPageMs = now;
        return;
      }
      showStatusBitmap(kStatusSending);
      blinkLed(0, LED_BRIGHTNESS, 0, 2, 500);
      String body;
      const int code = taskAction(pendingTaskId, "confirm", body);
      Serial.printf("Confirm HTTP %d: %s\n", code, body.c_str());
      if (code == HTTP_CODE_OK) {
        taskState = TaskState::RUNNING;
        lastPollMs = now;
        showStatus("RUNNING");
      } else {
        taskState = TaskState::ERROR;
        showStatus("ERROR");
        delay(3000);
        clearTask();
      }
      return;
    }
    if (updateButton(cancelButton, now)) {
      showStatusBitmap(kStatusCanceling);
      blinkLed(LED_BRIGHTNESS, LED_BRIGHTNESS / 2, 0, 2, 500);
      String body;
      const int code = taskAction(pendingTaskId, "cancel", body);
      Serial.printf("Cancel HTTP %d: %s\n", code, body.c_str());
      if (code == HTTP_CODE_OK) {
        clearTask();
      } else {
        taskState = TaskState::ERROR;
        showStatus("ERROR");
        delay(3000);
        clearTask();
      }
      return;
    }
  }

  if ((taskState == TaskState::RUNNING || taskState == TaskState::INTERACTIVE) &&
      now - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = now;
    String status;
    String body;
    const int code = fetchTaskStatus(pendingTaskId, status, body);
    Serial.printf("Poll HTTP %d: status=%s\n", code, status.c_str());
    if (code != HTTP_CODE_OK) {
      Serial.printf("Poll body: %s\n", body.c_str());
    } else if (status == "done") {
      taskState = TaskState::DONE;
      if (fetchTaskScreen(pendingTaskId)) {
        Serial.println("Answer screen displayed.");
      } else {
        showStatus("DONE");
      }
      answerScreenUntilMs = millis() + ANSWER_SCREEN_HOLD_MS;
      Serial.printf("Task result: %s\n", body.c_str());
      if (fetchAndPlayTaskAudio(pendingTaskId)) {
        Serial.println("Task result audio played.");
      } else {
        Serial.println("Task result audio unavailable.");
      }
      pendingTaskId = "";
      taskState = TaskState::IDLE;
      // Keep the answer screen for ANSWER_SCREEN_HOLD_MS, then return to idle.
      return;
    } else if (status == "error") {
      taskState = TaskState::ERROR;
      showStatus("ERROR");
      Serial.printf("Task error: %s\n", body.c_str());
      delay(3000);
      clearTask();
      return;
    } else if (status == "interactive") {
      taskState = TaskState::INTERACTIVE;
      showStatus("CODEX WIN");
    } else if (status == "expired" || status == "cancelled") {
      clearTask();
      return;
    } else {
      showStatus("RUNNING");
    }
  }

  if (digitalRead(VOICE_KEY_PIN) == LOW) {
    delay(30);
    if (digitalRead(VOICE_KEY_PIN) != LOW) {
      delay(10);
      return;
    }
    if (taskState != TaskState::IDLE) {
      showStatus("BUSY");
      while (digitalRead(VOICE_KEY_PIN) == LOW) {
        delay(10);
      }
      return;
    }

    showStatusBitmap(kStatusRecording);
    bool taskCreated = false;
    setLed(0, LED_BRIGHTNESS, 0);
    const size_t samples = recordUtterance();
    setLed(0, 0, 0);
    if (samples >= MIN_SAMPLES) {
      showStatusBitmap(kStatusUploading);
      const size_t wavSize = 44 + samples * 2;
      uint8_t *wavBuffer = static_cast<uint8_t *>(ps_malloc(wavSize));
      if (wavBuffer == nullptr) {
        Serial.println("PSRAM: WAV allocation failed.");
      } else {
        buildWav(recordBuffer, samples, wavBuffer);
        if (WiFi.status() == WL_CONNECTED) {
          String taskId;
          String body;
          const int code = requestTask(wavBuffer, wavSize, taskId, body);
          Serial.printf("Task request HTTP %d: %s\n", code, body.c_str());
          if (code == HTTP_CODE_OK && taskId.length() == 6) {
            pendingTaskId = taskId;
            taskState = TaskState::WAIT;
            taskCreated = true;
            taskCreatedMs = millis();
            promptScreenValid = false;
            showingPromptPage = false;
            screenBackupValid = false;
            if (fetchTaskPrompt(taskId)) {
              showingPromptPage = true;
              Serial.println("Task summary screen displayed.");
            } else {
              showPending();
              Serial.println("Task summary unavailable; showing task ID.");
            }
            lastPendingPageMs = millis();
            Serial.printf("Pending task %s; confirm on GPIO%d or cancel on GPIO%d.\n",
                          pendingTaskId.c_str(), CONFIRM_PIN, CANCEL_PIN);
          } else if (code == HTTP_CODE_CONFLICT) {
            taskState = TaskState::BUSY;
            showStatus("BUSY");
            delay(2000);
            taskState = TaskState::IDLE;
            showIdle();
          } else {
            taskState = TaskState::ERROR;
            showStatus("ERROR");
            delay(2000);
            clearTask();
          }
        } else {
          Serial.println("Task: WiFi disconnected; not sent.");
        }
        free(wavBuffer);
      }
    } else {
      Serial.println("Voice: utterance too short.");
    }

    if (!taskCreated && taskState == TaskState::IDLE) {
      restoreScreen();
    }

    while (digitalRead(VOICE_KEY_PIN) == LOW) {
      delay(10);
    }
  }

  delay(10);
}

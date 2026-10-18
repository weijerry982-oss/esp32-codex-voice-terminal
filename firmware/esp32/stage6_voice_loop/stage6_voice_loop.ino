/*
 * ESP32-Codex-Voice-Terminal — Stage 6 voice loop
 * Target: ESP32-S3 N16R8 / Arduino board "ESP32S3 Dev Module"
 *
 * Push-to-talk on GPIO0 (BOOT button, matching the xiaozhi
 * bread-compact-wifi configuration). While held, record 16 kHz/16-bit mono
 * from the INMP441/ICS43434 microphone; on release, POST a WAV to the Windows
 * service; receive a TTS WAV and play it through the MAX98357A.
 *
 * Wi-Fi credentials: ../stage2_wifi_test/wifi_config.h
 * Service URL / voice key: local voice_config.h (gitignored).
 * No external libraries: WiFi, HTTPClient, ESP_I2S from the Arduino-ESP32 core.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>

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
#define VOICE_SERVICE_URL ""
#define VOICE_KEY_PIN 0
#endif

constexpr int MIC_BCLK_PIN = 5;
constexpr int MIC_WS_PIN = 4;
constexpr int MIC_DIN_PIN = 6;
constexpr int SPK_BCLK_PIN = 15;
constexpr int SPK_WS_PIN = 16;
constexpr int SPK_DOUT_PIN = 7;

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t MAX_RECORD_SECONDS = 8;
constexpr size_t MAX_SAMPLES = SAMPLE_RATE * MAX_RECORD_SECONDS;
constexpr size_t MIN_SAMPLES = SAMPLE_RATE / 5;  // 200 ms
constexpr size_t READ_CHUNK_SAMPLES = 256;
constexpr size_t MAX_REPLY_BYTES = 512 * 1024;
constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long CONNECT_TIMEOUT_MS = 15000UL;
constexpr unsigned long CONNECT_RETRY_DELAY_MS = 5000UL;
constexpr uint8_t MAX_CONNECT_ATTEMPTS = 3;
constexpr unsigned long RECONNECT_INTERVAL_MS = 10000UL;

I2SClass i2sMic;
I2SClass i2sSpk;

int32_t rawBuffer[READ_CHUNK_SAMPLES];
int16_t *recordBuffer = nullptr;
uint8_t *replyBuffer = nullptr;
bool fatalConfigError = false;
unsigned long lastConnectAttemptMs = 0;

uint16_t readLe16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
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

size_t buildWav(const int16_t *samples, size_t count, uint8_t *out) {
  const uint32_t dataBytes = count * 2;
  memcpy(out, "RIFF", 4);
  writeLe32(out + 4, 36 + dataBytes);
  memcpy(out + 8, "WAVE", 4);
  memcpy(out + 12, "fmt ", 4);
  writeLe32(out + 16, 16);
  writeLe16(out + 20, 1);              // PCM
  writeLe16(out + 22, 1);              // mono
  writeLe32(out + 24, SAMPLE_RATE);
  writeLe32(out + 28, SAMPLE_RATE * 2);
  writeLe16(out + 32, 2);              // block align
  writeLe16(out + 34, 16);             // bits per sample
  memcpy(out + 36, "data", 4);
  writeLe32(out + 40, dataBytes);
  memcpy(out + 44, samples, dataBytes);
  return 44 + dataBytes;
}

size_t recordUtterance() {
  size_t total = 0;
  Serial.println("Recording... release the key to send.");
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
    Serial.printf("Playback: unsupported WAV (rate=%d channels=%d bits=%d data=%u).\n",
                  sampleRate, channels, bits, static_cast<unsigned>(dataSize));
    return false;
  }
  dataSize &= ~1UL;

  i2sSpk.configureTX(static_cast<uint32_t>(sampleRate), I2S_DATA_BIT_WIDTH_16BIT,
                     I2S_SLOT_MODE_STEREO);
  const int16_t *pcm = reinterpret_cast<const int16_t *>(data + dataOffset);
  const size_t frameBytes = 2 * channels;
  const size_t frames = dataSize / frameBytes;
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

bool sendAndPlay(uint8_t *wav, size_t wavSize) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, VOICE_SERVICE_URL)) {
    Serial.println("HTTP: begin failed.");
    return false;
  }
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Device-Id", deviceId());

  const int code = http.POST(wav, wavSize);
  Serial.printf("HTTP audio POST: %d\n", code);
  const String transcript = http.header("X-Transcript");
  if (transcript.length() > 0) {
    Serial.printf("Transcript(urlencoded): %s\n", transcript.c_str());
  }

  if (code != HTTP_CODE_OK) {
    const String body = http.getString();
    Serial.printf("HTTP error body: %s\n", body.c_str());
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  Serial.printf("Reply WAV bytes: %d\n", contentLength);
  if (contentLength <= 44 || contentLength > static_cast<int>(MAX_REPLY_BYTES)) {
    Serial.println("HTTP: reply size invalid.");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t total = 0;
  const unsigned long start = millis();
  while (total < static_cast<size_t>(contentLength) && millis() - start < 10000) {
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
    Serial.printf("HTTP: short WAV read (%u/%d).\n", static_cast<unsigned>(total), contentLength);
    return false;
  }
  return playWav(replyBuffer, total);
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
  Serial.println("Stage 6 push-to-talk voice loop");
  Serial.printf("Chip: %s\n", ESP.getChipModel());

#if !WIFI_CONFIG_PRESENT
  Serial.println("WiFi: wifi_config.h not found; expected ../stage2_wifi_test/wifi_config.h.");
  Serial.println("Status: ERROR");
  fatalConfigError = true;
  return;
#endif
#if !VOICE_CONFIG_PRESENT
  Serial.println("Voice: voice_config.h not found; copy voice_config.example.h.");
  Serial.println("Status: ERROR");
  fatalConfigError = true;
  return;
#endif

  Serial.printf("Voice service: %s\n", VOICE_SERVICE_URL);
  Serial.printf("Voice key: GPIO%d (BOOT), active LOW\n", VOICE_KEY_PIN);
  Serial.println("Mic: WS=4 SCK=5 SD=6; Speaker: DIN=7 BCLK=15 LRC=16");

  if (!connectWiFi()) {
    Serial.println("WiFi: initial connection failed; will retry in loop.");
  }
  lastConnectAttemptMs = millis();

  if (!i2sMic.setPort(I2S_NUM_0)) {
    Serial.println("I2S mic: setPort(I2S_NUM_0) failed.");
    Serial.println("Status: ERROR");
    fatalConfigError = true;
    return;
  }
  i2sMic.setPins(MIC_BCLK_PIN, MIC_WS_PIN, -1, MIC_DIN_PIN);
  if (!i2sMic.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                    I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("I2S mic: initialization failed.");
    Serial.println("Status: ERROR");
    fatalConfigError = true;
    return;
  }

  if (!i2sSpk.setPort(I2S_NUM_1)) {
    Serial.println("I2S speaker: setPort(I2S_NUM_1) failed.");
    Serial.println("Status: ERROR");
    fatalConfigError = true;
    return;
  }
  i2sSpk.setPins(SPK_BCLK_PIN, SPK_WS_PIN, SPK_DOUT_PIN, -1);
  if (!i2sSpk.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_STEREO)) {
    Serial.println("I2S speaker: initialization failed.");
    Serial.println("Status: ERROR");
    fatalConfigError = true;
    return;
  }

  recordBuffer = static_cast<int16_t *>(ps_malloc(MAX_SAMPLES * sizeof(int16_t)));
  replyBuffer = static_cast<uint8_t *>(ps_malloc(MAX_REPLY_BYTES));
  if (recordBuffer == nullptr || replyBuffer == nullptr) {
    Serial.println("PSRAM: allocation failed.");
    Serial.println("Status: ERROR");
    fatalConfigError = true;
    return;
  }

  pinMode(VOICE_KEY_PIN, INPUT_PULLUP);
  Serial.println("Press and hold GPIO0, speak, then release to send.");
  Serial.println("Status: RUNNING");
  Serial.println("========================================");
}

void loop() {
  if (fatalConfigError) {
    delay(1000);
    return;
  }

  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastConnectAttemptMs >= RECONNECT_INTERVAL_MS) {
      lastConnectAttemptMs = now;
      Serial.println("WiFi: disconnected; reconnecting.");
      connectWiFi();
    }
  }

  if (digitalRead(VOICE_KEY_PIN) == LOW) {
    delay(30);
    if (digitalRead(VOICE_KEY_PIN) != LOW) {
      delay(10);
      return;
    }

    const size_t samples = recordUtterance();
    if (samples >= MIN_SAMPLES) {
      const size_t wavSize = 44 + samples * 2;
      uint8_t *wavBuffer = static_cast<uint8_t *>(ps_malloc(wavSize));
      if (wavBuffer == nullptr) {
        Serial.println("PSRAM: WAV allocation failed.");
      } else {
        buildWav(recordBuffer, samples, wavBuffer);
        if (WiFi.status() == WL_CONNECTED) {
          sendAndPlay(wavBuffer, wavSize);
        } else {
          Serial.println("Voice: WiFi disconnected; utterance not sent.");
        }
        free(wavBuffer);
      }
    } else {
      Serial.println("Voice: utterance too short; ignored.");
    }

    while (digitalRead(VOICE_KEY_PIN) == LOW) {
      delay(10);
    }
  }
  delay(10);
}

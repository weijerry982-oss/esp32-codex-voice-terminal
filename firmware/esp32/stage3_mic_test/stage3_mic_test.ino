/*
 * ESP32-Codex-Voice-Terminal — Stage 3 I2S microphone recording test
 * Target: ESP32-S3 N16R8 / Arduino board "ESP32S3 Dev Module"
 *
 * User-confirmed wiring:
 *   Mic WS   -> GPIO4
 *   Mic SCK  -> GPIO5
 *   Mic SD   -> GPIO6
 *
 * Uses only the ESP_I2S library bundled with the Arduino-ESP32 core.
 * Configuration: 16 kHz, 32-bit slots, mono, left slot (INMP441/ICS43434
 * with L/R tied to GND). Serial prints one statistics window per second.
 *
 * No Wi-Fi, OLED, buttons, or RGB LED are used in this test.
 */

#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>

constexpr int I2S_BCLK_PIN = 5;
constexpr int I2S_WS_PIN = 4;
constexpr int I2S_DIN_PIN = 6;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t SAMPLES_PER_READ = 256;
constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long WINDOW_MS = 1000;

I2SClass i2s;
int32_t samples[SAMPLES_PER_READ];

unsigned long windowStartMs = 0;
unsigned long windowSamples = 0;
int32_t windowMin = INT32_MAX;
int32_t windowMax = INT32_MIN;
int64_t windowSum = 0;
int64_t windowSumSquares = 0;
unsigned long windowZeroCrossings = 0;
int32_t previousSample = 0;
unsigned long emptyReads = 0;

void resetWindow(unsigned long now) {
  windowStartMs = now;
  windowSamples = 0;
  windowMin = INT32_MAX;
  windowMax = INT32_MIN;
  windowSum = 0;
  windowSumSquares = 0;
  windowZeroCrossings = 0;
  previousSample = 0;
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
  Serial.println("Stage 3 I2S microphone recording test");
  Serial.printf("Chip: %s\n", ESP.getChipModel());
  Serial.printf("Mic pins: WS=GPIO%d, SCK=GPIO%d, SD=GPIO%d\n",
                I2S_WS_PIN, I2S_BCLK_PIN, I2S_DIN_PIN);
  Serial.printf("I2S: %lu Hz, 32-bit slots, mono, left slot\n", (unsigned long)SAMPLE_RATE);
  Serial.println("Not using Wi-Fi/OLED/buttons/RGB LED in this test.");

  i2s.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DIN_PIN);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("I2S: initialization failed; check WS/SCK/SD wiring and power.");
    Serial.println("Status: ERROR");
    return;
  }

  resetWindow(millis());
  Serial.println("I2S: initialized; speak or clap to verify signal changes.");
  Serial.println("Status: RUNNING");
  Serial.println("========================================");
}

void loop() {
  const size_t bytesRead = i2s.readBytes(reinterpret_cast<char *>(samples), sizeof(samples));
  const unsigned long now = millis();

  if (bytesRead == 0) {
    ++emptyReads;
    if (emptyReads == 100) {
      Serial.println("I2S: no data received; check mic power, clock, and data pins.");
    }
    if (emptyReads > 100) {
      emptyReads = 0;
    }
    delay(5);
  } else {
    emptyReads = 0;
    const size_t count = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < count; ++i) {
      // INMP441/ICS43434 output 24-bit data left-justified in a 32-bit slot.
      // Shift down to a 24-bit scale for stable integer statistics.
      const int32_t sample = samples[i] >> 8;
      if (sample < windowMin) {
        windowMin = sample;
      }
      if (sample > windowMax) {
        windowMax = sample;
      }
      windowSum += sample;
      windowSumSquares += static_cast<int64_t>(sample) * sample;
      if ((previousSample < 0 && sample >= 0) || (previousSample >= 0 && sample < 0)) {
        ++windowZeroCrossings;
      }
      previousSample = sample;
      ++windowSamples;
    }
  }

  if (now - windowStartMs >= WINDOW_MS) {
    if (windowSamples > 0) {
      const double rms = sqrt(static_cast<double>(windowSumSquares) / windowSamples);
      const double mean = static_cast<double>(windowSum) / windowSamples;
      Serial.printf("Mic window: samples=%lu | RMS=%.0f | p-p=%ld | min=%ld | max=%ld | mean=%.0f | ZCR=%lu\n",
                    windowSamples, rms, (long)(windowMax - windowMin),
                    (long)windowMin, (long)windowMax, mean, windowZeroCrossings);
    } else {
      Serial.println("Mic window: no samples received.");
    }
    resetWindow(now);
  }
}

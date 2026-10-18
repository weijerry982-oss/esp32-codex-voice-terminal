/*
 * ESP32-Codex-Voice-Terminal — Stage 4 MAX98357A speaker playback test
 * Target: ESP32-S3 N16R8 / Arduino board "ESP32S3 Dev Module"
 *
 * User-confirmed wiring:
 *   MAX98357A DIN  -> GPIO7
 *   MAX98357A BCLK -> GPIO15
 *   MAX98357A LRC  -> GPIO16
 *   MAX98357A VIN/GND -> 3V3/GND, SD tied to VIN, GAIN tied to GND
 *
 * Uses only the ESP_I2S library bundled with the Arduino-ESP32 core.
 * Plays a 1-second tone sequence (440 / 660 / 880 Hz, then silence),
 * repeating, so the speaker and I2S output path can be verified.
 *
 * No Wi-Fi, OLED, microphone, buttons, or RGB LED are used in this test.
 */

#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>

constexpr int I2S_BCLK_PIN = 15;
constexpr int I2S_WS_PIN = 16;
constexpr int I2S_DOUT_PIN = 7;
constexpr uint32_t SAMPLE_RATE = 24000;
constexpr unsigned long SERIAL_BAUD = 115200;
constexpr size_t FRAMES_PER_BUFFER = 256;
constexpr int16_t AMPLITUDE = 5000;

constexpr double TWO_PI_VALUE = 2.0 * PI;

I2SClass i2s;
int16_t sampleBuffer[FRAMES_PER_BUFFER * 2];  // interleaved left/right
double phase = 0.0;
int currentToneIndex = -1;

const int TONE_FREQUENCIES[] = {440, 660, 880, 0};
const char *TONE_NAMES[] = {"440 Hz", "660 Hz", "880 Hz", "silence"};

void setup() {
  Serial.begin(SERIAL_BAUD);
  unsigned long waitStart = millis();
  while (!Serial && (millis() - waitStart < 2000)) {
    delay(10);
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32-Codex-Voice-Terminal");
  Serial.println("Stage 4 MAX98357A speaker playback test");
  Serial.printf("Chip: %s\n", ESP.getChipModel());
  Serial.printf("Amp pins: DIN=GPIO%d, BCLK=GPIO%d, LRC=GPIO%d\n",
                I2S_DOUT_PIN, I2S_BCLK_PIN, I2S_WS_PIN);
  Serial.printf("I2S: %lu Hz, 16-bit, stereo, 440/660/880 Hz sequence\n",
                (unsigned long)SAMPLE_RATE);
  Serial.println("Not using Wi-Fi/OLED/mic/buttons/RGB LED in this test.");

  i2s.setPins(I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN, -1);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO)) {
    Serial.println("I2S: initialization failed; check DIN/BCLK/LRC wiring and amp power.");
    Serial.println("Status: ERROR");
    return;
  }

  Serial.println("I2S: initialized; playing tone sequence.");
  Serial.println("Status: RUNNING");
  Serial.println("========================================");
}

void loop() {
  const int toneIndex = (millis() / 1000UL) % 4;
  if (toneIndex != currentToneIndex) {
    currentToneIndex = toneIndex;
    phase = 0.0;
    Serial.printf("Tone: %s\n", TONE_NAMES[toneIndex]);
  }

  const int frequency = TONE_FREQUENCIES[toneIndex];
  for (size_t frame = 0; frame < FRAMES_PER_BUFFER; ++frame) {
    int16_t value = 0;
    if (frequency > 0) {
      value = static_cast<int16_t>(AMPLITUDE * sin(phase));
      phase += TWO_PI_VALUE * frequency / SAMPLE_RATE;
      if (phase >= TWO_PI_VALUE) {
        phase -= TWO_PI_VALUE;
      }
    }
    sampleBuffer[frame * 2] = value;
    sampleBuffer[frame * 2 + 1] = value;
  }

  const size_t written = i2s.write(sampleBuffer, sizeof(sampleBuffer));
  if (written == 0) {
    delay(1);
  }
}

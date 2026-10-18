/*
 * ESP32-Codex-Voice-Terminal — Stage 2 OLED minimal display test
 * Target: ESP32-S3 N16R8 (Arduino board selection: ESP32S3 Dev Module)
 *
 * Scope: I2C scan + SSD1306 128x32 minimal display test only.
 * No Wi-Fi, audio, buttons, or status LED.
 *
 * User-confirmed wiring:
 *   OLED SDA -> GPIO41
 *   OLED SCL -> GPIO42
 *   OLED VCC -> 3V3, OLED GND -> GND
 *
 * The OLED module schematic/datasheet identifies the controller as SSD1306,
 * I2C interface, 128x32, with 4.7K pull-ups on SDA/SCL.
 *
 * No external libraries are used: this sketch talks to the SSD1306 through
 * the Wire library provided by the Arduino-ESP32 core.
 */

#include <Wire.h>

constexpr int OLED_SDA_PIN = 41;
constexpr int OLED_SCL_PIN = 42;
constexpr uint32_t I2C_FREQUENCY = 400000;
constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 32;
constexpr int OLED_PAGES = OLED_HEIGHT / 8;
constexpr unsigned long SERIAL_BAUD = 115200;

// SSD1306 slave address is 0x3C or 0x3D depending on SA0. Probe both.
constexpr uint8_t SSD1306_ADDRESSES[] = {0x3C, 0x3D};

uint8_t oledAddress = 0;
uint8_t framebuffer[OLED_WIDTH * OLED_PAGES];
bool blinkOn = false;
unsigned long lastBlinkMs = 0;
unsigned long lastReportSeconds = ULONG_MAX;

struct Glyph {
  char ch;
  uint8_t columns[5];
};

// Minimal 5x7 column-major font for the characters used by this test.
const Glyph FONT[] = {
  {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
  {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
  {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
  {'3', {0x42, 0x61, 0x51, 0x49, 0x46}},
  {'6', {0x3E, 0x49, 0x49, 0x49, 0x32}},
  {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
  {'D', {0x7F, 0x41, 0x41, 0x41, 0x3E}},
  {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
  {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
  {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
  {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
  {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
  {'x', {0x44, 0x28, 0x10, 0x28, 0x44}},
};

bool i2cProbe(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void scanI2cBus() {
  Serial.println("I2C scan (0x08-0x77):");
  int found = 0;
  for (uint8_t address = 0x08; address <= 0x77; ++address) {
    if (i2cProbe(address)) {
      Serial.printf("  found 0x%02X\n", address);
      ++found;
    }
  }
  if (found == 0) {
    Serial.println("  no I2C device found; check SDA=GPIO41, SCL=GPIO42, power, and pull-ups.");
  }
}

bool ssd1306WriteCommands(const uint8_t *commands, size_t count) {
  Wire.beginTransmission(oledAddress);
  Wire.write(0x00);  // command stream
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
  return ssd1306WriteCommands(initSequence, sizeof(initSequence));
}

bool ssd1306Display() {
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
      Wire.write(0x40);  // data stream
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

void oledFill(bool on) {
  memset(framebuffer, on ? 0xFF : 0x00, sizeof(framebuffer));
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

void oledDrawRect(int x, int y, int width, int height) {
  for (int i = 0; i < width; ++i) {
    oledSetPixel(x + i, y, true);
    oledSetPixel(x + i, y + height - 1, true);
  }
  for (int i = 0; i < height; ++i) {
    oledSetPixel(x, y + i, true);
    oledSetPixel(x + width - 1, y + i, true);
  }
}

void oledFillRect(int x, int y, int width, int height) {
  for (int dy = 0; dy < height; ++dy) {
    for (int dx = 0; dx < width; ++dx) {
      oledSetPixel(x + dx, y + dy, true);
    }
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

void drawTestScreen() {
  oledClear();
  oledDrawRect(0, 0, OLED_WIDTH, OLED_HEIGHT);
  oledDrawText("OLED OK", 6, 3, 2);
  oledDrawText("SSD1306", 6, 19, 1);

  char addressText[8];
  snprintf(addressText, sizeof(addressText), "0x%02X", oledAddress);
  oledDrawText(addressText, 56, 19, 1);

  if (blinkOn) {
    oledFillRect(118, 3, 6, 6);
  }
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
  Serial.println("Stage 2 OLED minimal display test");
  Serial.printf("Chip: %s\n", ESP.getChipModel());
  Serial.printf("SDA: GPIO%d, SCL: GPIO%d, I2C: %lu Hz\n",
                OLED_SDA_PIN, OLED_SCL_PIN, (unsigned long)I2C_FREQUENCY);
  Serial.println("Controller: SSD1306 (module schematic/datasheet)");

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN, I2C_FREQUENCY);
  delay(50);

  scanI2cBus();

  for (uint8_t address : SSD1306_ADDRESSES) {
    if (i2cProbe(address)) {
      oledAddress = address;
      break;
    }
  }
  if (oledAddress == 0) {
    Serial.println("OLED: no SSD1306 responded at 0x3C or 0x3D.");
    Serial.println("OLED: check VCC/GND, SDA/SCL wiring, and pull-ups.");
    Serial.println("Status: ERROR");
    return;
  }
  Serial.printf("OLED: using I2C address 0x%02X\n", oledAddress);

  if (!ssd1306Init()) {
    Serial.println("OLED: initialization command transfer failed.");
    Serial.println("Status: ERROR");
    return;
  }
  Serial.println("OLED: initialization sequence sent.");
  delay(100);

  oledFill(true);
  if (!ssd1306Display()) {
    Serial.println("OLED: full-on pattern transfer failed.");
    Serial.println("Status: ERROR");
    return;
  }
  Serial.println("OLED: full-on pattern displayed.");
  delay(500);

  oledClear();
  ssd1306Display();
  Serial.println("OLED: cleared.");
  delay(200);

  drawTestScreen();
  if (!ssd1306Display()) {
    Serial.println("OLED: test screen transfer failed.");
    Serial.println("Status: ERROR");
    return;
  }
  Serial.println("OLED: test screen displayed.");
  Serial.println("Status: RUNNING");
  Serial.println("========================================");
}

void loop() {
  if (oledAddress == 0) {
    delay(1000);
    return;
  }

  const unsigned long now = millis();
  if (now - lastBlinkMs >= 1000) {
    lastBlinkMs = now;
    blinkOn = !blinkOn;
    drawTestScreen();
    if (!ssd1306Display()) {
      Serial.println("OLED: display update failed.");
    }
  }

  const unsigned long uptimeSeconds = now / 1000;
  if (uptimeSeconds != lastReportSeconds) {
    lastReportSeconds = uptimeSeconds;
    Serial.printf("Status: RUNNING | Uptime: %lu s | OLED addr: 0x%02X\n",
                  uptimeSeconds, oledAddress);
  }
  delay(20);
}

/*
 * ESP32-Codex-Voice-Terminal — Stage 1 minimal serial/OLED test
 * Target: ESP32-S3 N16R8 (Arduino board selection: ESP32S3 Dev Module)
 *
 * OLED support is deliberately disabled until the physical panel/controller
 * and I2C address are confirmed. Serial-only mode uses Arduino/ESP32 core APIs.
 * See the comments below before enabling OLED support.
 */

#include <limits.h>

#define ENABLE_OLED 0  // Change to 1 only after confirming the OLED module.

#if ENABLE_OLED
  // Dependencies (install manually only if/when OLED testing is approved):
  // - Adafruit GFX Library
  // - Adafruit SSD1306
  // Wire is included with the Arduino-ESP32 core.
  // The panel controller must be SSD1306-compatible; this is not yet verified.
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>

  // TODO: Set this to the address measured/confirmed for the actual OLED.
  // Do not assume 0x3C or 0x3D without checking the module.
  #ifndef OLED_I2C_ADDRESS
    #error "Confirm OLED I2C address, then define OLED_I2C_ADDRESS (for example in this file)."
  #endif

  constexpr int OLED_WIDTH = 128;
  constexpr int OLED_HEIGHT = 32;
  constexpr int OLED_SDA_PIN = 41;  // User-confirmed wiring diagram; hardware test pending.
  constexpr int OLED_SCL_PIN = 42;  // Diagram labels this I2C clock as SCK; verify module marking.
  Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
#endif

constexpr unsigned long SERIAL_BAUD = 115200;
unsigned long lastReportSeconds = ULONG_MAX;

void drawOled(unsigned long uptimeSeconds) {
#if ENABLE_OLED
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("ESP32-Codex-Voice");
  display.println("Terminal");
  display.println("Status: RUNNING");
  display.print("Uptime: ");
  display.print(uptimeSeconds);
  display.println(" s");
  display.display();
#else
  (void)uptimeSeconds;
#endif
}

void setup() {
  Serial.begin(SERIAL_BAUD);

  // Wait up to ~2 s for the USB CDC port to become ready, so the host does
  // not miss the startup banner after a reset re-enumerates the port.
  unsigned long waitStart = millis();
  while (!Serial && (millis() - waitStart < 2000)) {
    delay(10);
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32-Codex-Voice-Terminal");
  Serial.println("Stage 1 minimal hardware test");
  Serial.printf("Chip: %s\n", ESP.getChipModel());
  Serial.printf("Cores: %d, Revision: %d\n", ESP.getChipCores(), ESP.getChipRevision());
  Serial.printf("Flash size: %lu MB\n", (unsigned long)(ESP.getFlashChipSize() / (1024UL * 1024UL)));
  Serial.printf("PSRAM size: %lu MB\n", (unsigned long)(ESP.getPsramSize() / (1024UL * 1024UL)));
  Serial.printf("SDK: %s\n", ESP.getSdkVersion());
  Serial.println("Status: STARTING");
  Serial.printf("Serial baud: %lu\n", SERIAL_BAUD);

#if ENABLE_OLED
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    Serial.println("OLED: initialization failed; check controller, address, wiring, and power.");
  } else {
    Serial.println("OLED: initialized (controller/address must be confirmed on the real module).");
  }
#else
  Serial.println("OLED: disabled; running serial-only test.");
#endif

  Serial.println("Status: RUNNING");
  Serial.println("========================================");
}

void loop() {
  const unsigned long uptimeSeconds = millis() / 1000UL;
  if (uptimeSeconds != lastReportSeconds) {
    lastReportSeconds = uptimeSeconds;
    Serial.printf("Status: RUNNING | Uptime: %lu s\n", uptimeSeconds);
    drawOled(uptimeSeconds);
  }
  delay(20);
}




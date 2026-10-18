/*
 * ESP32-Codex-Voice-Terminal — Stage 2 button + RGB LED test
 * Target: ESP32-S3 N16R8 / Arduino board "ESP32S3 Dev Module"
 *
 * User-confirmed wiring:
 *   RGB LED data -> GPIO48 (xiaozhi bread-compact-wifi BUILTIN_LED_GPIO;
 *                   WS2812-compatible; core rgbLedWrite, GRB order)
 *   Button A     -> GPIO39 -> GND (INPUT_PULLUP, active LOW)
 *   Button B     -> GPIO40 -> GND (INPUT_PULLUP, active LOW)
 *
 * This test does not use Wi-Fi or the OLED. The OLED keeps its last content.
 * Press GPIO39 for red, GPIO40 for blue, both for green; release for off.
 * Button events are printed over Serial (115200 baud).
 */

constexpr int RGB_LED_PIN = 48;
constexpr int BUTTON_A_PIN = 39;
constexpr int BUTTON_B_PIN = 40;
constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long DEBOUNCE_MS = 30;
constexpr unsigned long HEARTBEAT_MS = 5000;
constexpr uint8_t LED_BRIGHTNESS = 64;

struct Button {
  uint8_t pin;
  bool stableState;   // HIGH = released, LOW = pressed
  bool lastReading;
  unsigned long lastChangeMs;
};

Button buttonA{BUTTON_A_PIN, HIGH, HIGH, 0};
Button buttonB{BUTTON_B_PIN, HIGH, HIGH, 0};

char lastEvent[40] = "none";
unsigned long lastEventSeconds = 0;
unsigned long lastHeartbeatMs = 0;

void setLed(bool aPressed, bool bPressed) {
  if (aPressed && bPressed) {
    rgbLedWrite(RGB_LED_PIN, 0, LED_BRIGHTNESS, 0);  // green
  } else if (aPressed) {
    rgbLedWrite(RGB_LED_PIN, LED_BRIGHTNESS, 0, 0);  // red
  } else if (bPressed) {
    rgbLedWrite(RGB_LED_PIN, 0, 0, LED_BRIGHTNESS);  // blue
  } else {
    rgbLedWrite(RGB_LED_PIN, 0, 0, 0);               // off
  }
}

const char *ledName(bool aPressed, bool bPressed) {
  if (aPressed && bPressed) {
    return "GREEN";
  }
  if (aPressed) {
    return "RED";
  }
  if (bPressed) {
    return "BLUE";
  }
  return "OFF";
}

void updateButton(Button &button, const char *label, unsigned long now) {
  const bool reading = digitalRead(button.pin);
  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.lastChangeMs = now;
  }

  if ((now - button.lastChangeMs) < DEBOUNCE_MS) {
    return;
  }
  if (reading == button.stableState) {
    return;
  }

  button.stableState = reading;
  const bool pressed = (reading == LOW);
  snprintf(lastEvent, sizeof(lastEvent), "%s %s", label, pressed ? "PRESSED" : "RELEASED");
  lastEventSeconds = now / 1000UL;
  Serial.printf("BTN: %s %s | uptime %lu s\n", label, pressed ? "PRESSED" : "RELEASED",
                now / 1000UL);

  setLed(buttonA.stableState == LOW, buttonB.stableState == LOW);
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
  Serial.println("Stage 2 button + RGB LED test");
  Serial.printf("Chip: %s\n", ESP.getChipModel());
  Serial.printf("RGB LED: GPIO%d (WS2812-compatible)\n", RGB_LED_PIN);
  Serial.printf("Button A: GPIO%d -> GND, INPUT_PULLUP, active LOW\n", BUTTON_A_PIN);
  Serial.printf("Button B: GPIO%d -> GND, INPUT_PULLUP, active LOW\n", BUTTON_B_PIN);

  pinMode(BUTTON_A_PIN, INPUT_PULLUP);
  pinMode(BUTTON_B_PIN, INPUT_PULLUP);
  delay(50);

  Serial.println("LED self-test: red, green, blue, white, off");
  rgbLedWrite(RGB_LED_PIN, LED_BRIGHTNESS, 0, 0);
  delay(300);
  rgbLedWrite(RGB_LED_PIN, 0, LED_BRIGHTNESS, 0);
  delay(300);
  rgbLedWrite(RGB_LED_PIN, 0, 0, LED_BRIGHTNESS);
  delay(300);
  rgbLedWrite(RGB_LED_PIN, LED_BRIGHTNESS, LED_BRIGHTNESS, LED_BRIGHTNESS);
  delay(300);
  rgbLedWrite(RGB_LED_PIN, 0, 0, 0);

  lastHeartbeatMs = millis();
  Serial.println("Status: RUNNING");
  Serial.println("========================================");
}

void loop() {
  const unsigned long now = millis();

  updateButton(buttonA, "GPIO39", now);
  updateButton(buttonB, "GPIO40", now);

  if (now - lastHeartbeatMs >= HEARTBEAT_MS) {
    lastHeartbeatMs = now;
    const bool aPressed = (buttonA.stableState == LOW);
    const bool bPressed = (buttonB.stableState == LOW);
    Serial.printf("Status: RUNNING | Uptime: %lu s | GPIO39: %s | GPIO40: %s | LED: %s | Last: %s @ %lu s\n",
                  now / 1000UL, aPressed ? "DOWN" : "UP", bPressed ? "DOWN" : "UP",
                  ledName(aPressed, bPressed), lastEvent, lastEventSeconds);
  }

  delay(10);
}

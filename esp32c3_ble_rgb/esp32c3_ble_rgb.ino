/*
  ESP32-C3 BLE RGB LED controller
  ---------------------------------
  Drives a common-anode analog RGB strip through 3 low-side N-MOSFETs
  (your IRLZ44Ns). Control is over Bluetooth LE only - no WiFi, no mic, no IR.

  Replicates the core HappyLighting experience:
    - solid color
    - master brightness
    - a couple of effects (rainbow cycle, breathe)

  REQUIRES: arduino-esp32 core v3.0 or newer (uses the v3 LEDC API:
            ledcAttach() / ledcWrite(pin, duty)).
  BOARD:    any ESP32-C3 (e.g. "ESP32C3 Dev Module" / C3 Super Mini).

  If it fails to flash with a "sketch too big" error, pick a partition
  scheme with more app space:  Tools -> Partition Scheme -> "Minimal SPIFFS
  (1.9MB APP)".

  If it fails to COMPILE on a BLE callback line, your core version uses the
  2-argument callback form. Fix: add  , esp_ble_gatts_cb_param_t *param
  to the onWrite / onConnect / onDisconnect parameter lists below.
*/

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// ---------- Pins ----------
// Safe ESP32-C3 GPIOs: avoid strapping pins 2/8/9, USB pins 18/19, UART 20/21.
const int PIN_R = 3;
const int PIN_G = 4;
const int PIN_B = 5;

// ---------- PWM ----------
const uint32_t PWM_FREQ = 2000;   // Hz. Raise for less flicker on camera,
                                  // lower for less switching loss. 2 kHz is a
                                  // good middle ground with the slow 3.3V gate drive.
const uint8_t  PWM_RES  = 8;      // 8-bit -> duty 0..255, matches RGB byte values.

// With a low-side N-MOSFET on a common-anode strip, duty maps DIRECTLY to
// brightness (high duty = MOSFET on = color on). No inversion needed.

// ---------- BLE UUIDs ----------
#define SERVICE_UUID "9a1e5b00-0001-4a2b-8c00-1a2b3c4d5e6f"
#define COLOR_UUID   "9a1e5b00-0002-4a2b-8c00-1a2b3c4d5e6f"  // write 3 bytes: R G B
#define BRIGHT_UUID  "9a1e5b00-0003-4a2b-8c00-1a2b3c4d5e6f"  // write 1 byte:  0..255 brightness
#define MODE_UUID    "9a1e5b00-0004-4a2b-8c00-1a2b3c4d5e6f"  // write 1-2 bytes: mode [, speed]
//   mode: 0 = solid, 1 = rainbow, 2 = breathe ;  speed: 1 (slow) .. 255 (fast)

// ---------- State ----------
volatile uint8_t curR = 255, curG = 255, curB = 255;  // base color
volatile uint8_t bright = 255;                         // master brightness 0..255
volatile uint8_t mode   = 0;                           // 0 solid, 1 rainbow, 2 breathe
volatile uint8_t speed  = 128;                         // effect speed 1..255

// ---------- Helpers ----------
// Integer HSV (each 0..255) -> RGB. Used for the rainbow effect.
void hsv2rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t region = h / 43;
  uint8_t rem    = (h - region * 43) * 6;
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
  switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
}

inline uint8_t scale(uint8_t c, uint8_t br) { return (uint16_t)c * br / 255; }

void writeRGB(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(PIN_R, r);
  ledcWrite(PIN_G, g);
  ledcWrite(PIN_B, b);
}

// ---------- BLE callbacks ----------
class ColorCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    uint8_t *d = c->getData();
    size_t   n = c->getLength();
    if (n >= 3) { curR = d[0]; curG = d[1]; curB = d[2]; mode = 0; } // color write -> solid
  }
};

class BrightCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    uint8_t *d = c->getData();
    if (c->getLength() >= 1) bright = d[0];
  }
};

class ModeCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    uint8_t *d = c->getData();
    size_t   n = c->getLength();
    if (n >= 1) mode = d[0];
    if (n >= 2) speed = d[1] ? d[1] : 1;   // never 0
  }
};

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override { /* connected */ }
  void onDisconnect(BLEServer *s) override {
    BLEDevice::startAdvertising();         // re-advertise so it's reconnectable
  }
};

// ---------- Setup ----------
void setup() {
  ledcAttach(PIN_R, PWM_FREQ, PWM_RES);
  ledcAttach(PIN_G, PWM_FREQ, PWM_RES);
  ledcAttach(PIN_B, PWM_FREQ, PWM_RES);
  writeRGB(0, 0, 0);

  BLEDevice::init("ESP32C3-RGB");
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  BLEService *svc = server->createService(SERVICE_UUID);

  BLECharacteristic *colorChar = svc->createCharacteristic(
      COLOR_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ);
  colorChar->setCallbacks(new ColorCB());
  uint8_t initColor[3] = {255, 255, 255};
  colorChar->setValue(initColor, 3);

  BLECharacteristic *brightChar = svc->createCharacteristic(
      BRIGHT_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ);
  brightChar->setCallbacks(new BrightCB());
  uint8_t initBright = bright;            // local (non-volatile) copy for setValue
  brightChar->setValue(&initBright, 1);

  BLECharacteristic *modeChar = svc->createCharacteristic(
      MODE_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ);
  modeChar->setCallbacks(new ModeCB());
  uint8_t initMode = mode;               // local (non-volatile) copy for setValue
  modeChar->setValue(&initMode, 1);

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// ---------- Loop ----------
void loop() {
  static uint32_t last  = 0;
  static uint8_t  hue   = 0;
  static int16_t  breath = 0;
  static int8_t   bdir  = 1;

  uint32_t now = millis();
  uint32_t interval = map(speed, 1, 255, 60, 2);   // ms per animation step
  if (now - last < interval) return;
  last = now;

  if (mode == 0) {                                 // solid color
    writeRGB(scale(curR, bright), scale(curG, bright), scale(curB, bright));

  } else if (mode == 1) {                          // rainbow cycle
    uint8_t r, g, b;
    hsv2rgb(hue++, 255, 255, r, g, b);
    writeRGB(scale(r, bright), scale(g, bright), scale(b, bright));

  } else if (mode == 2) {                          // breathe current color
    breath += bdir * 4;
    if (breath >= 255) { breath = 255; bdir = -1; }
    if (breath <= 0)   { breath = 0;   bdir =  1; }
    uint8_t b8 = (uint16_t)breath * bright / 255;
    writeRGB(scale(curR, b8), scale(curG, b8), scale(curB, b8));
  }
}

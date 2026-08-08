/*
  ------------------------------------------------------------
  M5 Atom Matrix Companion v4
  Single Button Satellite
  Author: Adrian Davis
  URL: https://github.com/themusicnerd/M5-AtomMatrix-Companion-v4-Satellite
  Board: M5Atom (ESP32)
  License: MIT

  Features:
    - Companion v4 Satellite API support
    - Single-button surface
    - 5x5 Matrix status icons (WiFi, OK, error, etc.)
    - External RGB LED PWM output (G33 RED / G22 GREEN / G19 BLUE + G23 GND)
    - WiFiManager config portal (hold 5s)
    - OTA firmware updates
    - Full MAC-based deviceID (M5ATOM_<fullmac>)
    - Auto reconnect, ping, and key-release failsafe
  Thanks To:
    Joespeh Adams, Brad De La Rue, m9-999 and all the wonderful people behind Companion!
  ------------------------------------------------------------
*/

#include <M5Atom.h>

#define FIRMWARE_VERSION "1.3.15"
#ifdef ATOMIC_POE_BUILD
#include <SPI.h>
#include <M5_Ethernet.h>
#include <esp_mac.h>
#include "PoeWebServer.h"
#else
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#endif
#include <Preferences.h>
#include <Update.h>
#include <vector>
#include <esp32-hal-ledc.h>   // core 3.x LEDC helpers

Preferences preferences;
#ifdef ATOMIC_POE_BUILD
EthernetClient client;
PoeWebServer restServer(9999);
#else
WiFiManager wifiManager;
WiFiClient client;
WebServer restServer(9999);
#endif

// -------------------------------------------------------------------
// Companion Server
// -------------------------------------------------------------------
char companion_host[40] = "Companion IP";
char companion_port[6]  = "16622";

// Static IP (0.0.0.0 = DHCP)
IPAddress stationIP   = IPAddress(0, 0, 0, 0);
IPAddress stationGW   = IPAddress(0, 0, 0, 0);
IPAddress stationMask = IPAddress(255, 255, 255, 0);

// Device ID – full MAC will be appended
String deviceID = "";

// WiFi hostname for mDNS
String wifiHostname = "";

// AP password for config portal (empty = open)
const char* AP_password = "";

// Timing
unsigned long lastPingTime    = 0;
unsigned long lastConnectTry  = 0;
const unsigned long connectRetryMs = 5000;
const unsigned long pingIntervalMs  = 2000;

// Brightness (0–100)
int brightness = 100;
bool ledEnabled = true;
int ledBrightnessPercent = 100;
String configuredDeviceName = "";
String serialProvisionBuffer = "";

// LED_DisPlay::setBrightness() accepts 0-100 (not 0-255) and applies its own
// FastLED scaling internally. The matrix controller ceiling is 100/100; the
// separately configurable RGB multiplier provides additional output control.
const uint8_t MATRIX_MAX_BRIGHTNESS_PERCENT = 100;
const uint8_t MATRIX_MAX_RGB_SCALE_PERCENT = 100;
int matrixOutputPercent = 100;
int matrixRedPercent = 100;
int matrixGreenPercent = 100;
int matrixBluePercent = 100;

uint8_t matrixFastLedBrightness() {
  const int m5Brightness = map(brightness, 0, 100, 0, MATRIX_MAX_BRIGHTNESS_PERCENT);
  return 40 * m5Brightness / 100;
}

void applyMatrixBrightness() {
  brightness = constrain(brightness, 0, 100);
  M5.dis.setBrightness(map(brightness, 0, 100, 0, MATRIX_MAX_BRIGHTNESS_PERCENT));
  FastLED.setBrightness(matrixFastLedBrightness());
}

// 0=0°, 1=90° clockwise, 2=180°, 3=270° clockwise.  This applies to the
// logical 5x5 canvas, including text, status icons and the status pixel.
int matrixRotation = 0;

// -------------------------------------------------------------------
// External RGB LED (Jaycar RGB LED)  - ATOM MATRIX PINS
// -------------------------------------------------------------------
#define LEDC_CHANNEL_RED   0
#define LEDC_CHANNEL_GREEN 1
#define LEDC_CHANNEL_BLUE  2

const int LED_PIN_RED   = 33;  // G33
const int LED_PIN_GREEN = 22;  // G22
const int LED_PIN_BLUE  = 19;  // G19
const int LED_PIN_GND   = 23;  // G23 (ground for LED)

const int pwmFreq       = 5000; // 5 kHz
const int pwmResolution = 8;

bool attachPwm(uint8_t pin, uint8_t channel) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  return ledcAttach(pin, pwmFreq, pwmResolution);
#else
  ledcSetup(channel, pwmFreq, pwmResolution);
  ledcAttachPin(pin, channel);
  return true;
#endif
}

void writePwm(uint8_t pin, uint8_t channel, uint8_t value) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(pin, value);
#else
  ledcWrite(channel, value);
#endif
}

uint8_t lastColorR = 0;
uint8_t lastColorG = 0;
uint8_t lastColorB = 0;

// The 5x5 panel is also the tally indicator, so reserve its top-left pixel as
// a small connection-status light.  The remaining 24 pixels retain the colour
// sent by Companion.
enum MatrixStatus { STATUS_BOOT, STATUS_WIFI, STATUS_CONFIG, STATUS_CONNECTED, STATUS_ERROR };
MatrixStatus matrixStatus = STATUS_BOOT;
bool tallyActive = false;
const unsigned long connectedStatusDisplayMs = 30000;
unsigned long matrixConnectedSince = 0;
bool matrixConnectedIndicatorHidden = false;

// Companion can supply both COLOR and TEXT with a key state.  The Atom's
// 5x5 panel is too small for a conventional display font, so text uses a
// purpose-built 3x5 pixel font.  Short labels are centred and longer labels
// scroll from right to left.  With no text, the existing colour-only tally
// display remains in use.
String matrixText = "";
int matrixTextScroll = 0;
unsigned long lastTextScrollTime = 0;
const unsigned long textScrollIntervalMs = 180;
int matrixBackgroundColor = 0x000000;

// -------------------------------------------------------------------
// Matrix number / icon system (ported from TallyArbiter project)
// -------------------------------------------------------------------
int rotatedNumber[25];   // kept for future rotation use

// Default color values
int RGB_COLOR_WHITE        = 0xffffff;
int RGB_COLOR_DIMWHITE     = 0x555555;
int RGB_COLOR_WARMWHITE    = 0xFFEBC8;
int RGB_COLOR_DIMWARMWHITE = 0x877D5F;
int RGB_COLOR_BLACK        = 0x000000;
int RGB_COLOR_RED          = 0xff0000;
int RGB_COLOR_ORANGE       = 0xa5ff00;
int RGB_COLOR_YELLOW       = 0xffff00;
int RGB_COLOR_DIMYELLOW    = 0x555500;
int RGB_COLOR_GREEN        = 0x008800; // toned down
int RGB_COLOR_BLUE         = 0x0000ff;
int RGB_COLOR_PURPLE       = 0x008080;

int numbercolor = RGB_COLOR_WARMWHITE;

int flashcolor[]  = {RGB_COLOR_WHITE, RGB_COLOR_WHITE};
int offcolor[]    = {RGB_COLOR_BLACK, numbercolor};
int badcolor[]    = {RGB_COLOR_BLACK, RGB_COLOR_RED};
int readycolor[]  = {RGB_COLOR_BLACK, RGB_COLOR_GREEN};
int alloffcolor[] = {RGB_COLOR_BLACK, RGB_COLOR_BLACK};
int wificolor[]   = {RGB_COLOR_BLACK, RGB_COLOR_BLUE};
int infocolor[]   = {RGB_COLOR_BLACK, RGB_COLOR_ORANGE};

// Number glyphs (only 0 is used at the moment as a “dot”)
int number[17][25] = {
  {
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 1,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0
  }, // Number 0 - (single dot)
  { 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1,
    1, 1, 1, 1, 1,
    0, 1, 0, 0, 1,
    0, 0, 0, 0, 0
  }, // Number 1
  { 0, 0, 0, 0, 0,
    1, 1, 1, 0, 1,
    1, 0, 1, 0, 1,
    1, 0, 1, 1, 1,
    0, 0, 0, 0, 0
  }, // Number 2
  { 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1,
    1, 0, 1, 0, 1,
    1, 0, 1, 0, 1,
    0, 0, 0, 0, 0
  }, // Number 3
  { 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1,
    0, 0, 1, 0, 0,
    1, 1, 1, 0, 0,
    0, 0, 0, 0, 0
  }, // Number 4
  { 0, 0, 0, 0, 0,
    1, 0, 1, 1, 1,
    1, 0, 1, 0, 1,
    1, 1, 1, 0, 1,
    0, 0, 0, 0, 0
  }, // Number 5
  { 0, 0, 0, 0, 0,
    1, 0, 1, 1, 1,
    1, 0, 1, 0, 1,
    1, 1, 1, 1, 1,
    0, 0, 0, 0, 0
  }, // Number 6
  { 0, 0, 0, 0, 0,
    1, 1, 0, 0, 0,
    1, 0, 1, 0, 0,
    1, 0, 0, 1, 1,
    0, 0, 0, 0, 0
  }, // Number 7
  { 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1,
    1, 0, 1, 0, 1,
    1, 1, 1, 1, 1,
    0, 0, 0, 0, 0
  }, // Number 8
  { 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1,
    1, 0, 1, 0, 1,
    1, 1, 1, 0, 1,
    0, 0, 0, 0, 0
  }, // Number 9
  { 1, 1, 1, 1, 1,
    1, 0, 0, 0, 1,
    1, 1, 1, 1, 1,
    0, 0, 0, 0, 0,
    1, 1, 1, 1, 1
  }, // Number 10
  { 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1,
    0, 0, 0, 0, 0,
    1, 1, 1, 1, 1,
    0, 0, 0, 0, 0
  }, // Number 11
  { 1, 1, 1, 0, 1,
    1, 0, 1, 0, 1,
    1, 0, 1, 1, 1,
    0, 0, 0, 0, 0,
    1, 1, 1, 1, 1
  }, // Number 12
  { 1, 1, 1, 1, 1,
    1, 0, 1, 0, 1,
    1, 0, 1, 0, 1,
    0, 0, 0, 0, 0,
    1, 1, 1, 1, 1
  }, // Number 13
  { 1, 1, 1, 1, 1,
    0, 0, 1, 0, 0,
    1, 1, 1, 0, 0,
    0, 0, 0, 0, 0,
    1, 1, 1, 1, 1
  }, // Number 14
  { 1, 0, 1, 1, 1,
    1, 0, 1, 0, 1,
    1, 1, 1, 0, 1,
    0, 0, 0, 0, 0,
    1, 1, 1, 1, 1
  }, // Number 15
  { 1, 0, 1, 1, 1,
    1, 0, 1, 0, 1,
    1, 1, 1, 1, 1,
    0, 0, 0, 0, 0,
    1, 1, 1, 1, 1
  }, // Number 16
};

// Icons for WiFi / setup / good / error, etc.
int icons[13][25] = {
  { 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1,
    1, 1, 1, 1, 1,
    1, 1, 1, 1, 1,
    1, 1, 1, 1, 1
  }, // full blank (used as fill)
  { 0, 0, 1, 1, 1,
    0, 1, 0, 0, 0,
    1, 0, 0, 1, 1,
    1, 0, 1, 0, 0,
    1, 0, 1, 0, 1
  }, // wifi 3 rings
  { 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 1, 1,
    0, 0, 1, 0, 0,
    0, 0, 1, 0, 1
  }, // wifi 2 rings
  { 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 1
  }, // wifi 1 ring
  { 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 1, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0
  }, // reassign 1
  { 0, 0, 0, 0, 0,
    0, 1, 1, 1, 0,
    0, 1, 0, 1, 0,
    0, 1, 1, 1, 0,
    0, 0, 0, 0, 0
  }, // reassign 2
  { 1, 1, 1, 1, 1,
    1, 0, 0, 0, 1,
    1, 0, 0, 0, 1,
    1, 0, 0, 0, 1,
    1, 1, 1, 1, 1
  }, // reassign 3
  { 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 1, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0
  }, // setup 1
  { 0, 0, 1, 0, 0,
    0, 0, 0, 0, 0,
    1, 0, 0, 0, 1,
    0, 0, 0, 0, 0,
    0, 0, 1, 0, 0
  }, // setup 3 (slight tweak)
  { 1, 0, 0, 0, 1,
    0, 1, 0, 1, 0,
    0, 0, 1, 0, 0,
    0, 1, 0, 1, 0,
    1, 0, 0, 0, 1
  }, // error
  { 0, 1, 0, 0, 0,
    0, 0, 1, 0, 0,
    0, 0, 0, 1, 0,
    0, 0, 0, 0, 1,
    0, 0, 0, 1, 0
  }, // good (tick)
  { 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0
  }, // no icon
};

// -------------------------------------------------------------------
// WiFiManager Parameters
// -------------------------------------------------------------------
#ifndef ATOMIC_POE_BUILD
WiFiManagerParameter* custom_companionIP;
WiFiManagerParameter* custom_companionPort;
WiFiManagerParameter* custom_rotation;
#endif

// Logger
void logger(const String& s, const String& type = "info") {
  Serial.println(s);
}

// -------------------------------------------------------------------
// Matrix drawing helpers (Tally-Arbiter style)
// -------------------------------------------------------------------
int scaleMatrixColor(int rgb) {
  // Apply the configured percentage equally to every colour channel. The
  // separate M5 display brightness remains capped at 100/100. Per-channel
  // percentages allow white balance adjustment without changing hue globally.
  const uint8_t r = ((rgb >> 16) & 0xFF) * matrixOutputPercent / 100 * matrixRedPercent / 100;
  const uint8_t g = ((rgb >> 8)  & 0xFF) * matrixOutputPercent / 100 * matrixGreenPercent / 100;
  const uint8_t b = (rgb & 0xFF) * matrixOutputPercent / 100 * matrixBluePercent / 100;
  return (r << 16) | (g << 8) | b;
}

void matrixDrawPixel(uint8_t pixel, int rgb) {
  uint8_t x = pixel % 5;
  uint8_t y = pixel / 5;
  uint8_t rotatedX = x;
  uint8_t rotatedY = y;
  switch (matrixRotation) {
    case 1: rotatedX = 4 - y; rotatedY = x; break;
    case 2: rotatedX = 4 - x; rotatedY = 4 - y; break;
    case 3: rotatedX = y;     rotatedY = 4 - x; break;
  }
  M5.dis.drawpix(rotatedY * 5 + rotatedX, scaleMatrixColor(rgb));
}

void matrixFill(int rgb) {
  M5.dis.fillpix(scaleMatrixColor(rgb));
}

void drawNumberArray(int arr[25], int colors[2]) {
  for (int i = 0; i < 25; i++) {
    int colorIndex = arr[i];  // 0 or 1
    int rgb        = colors[colorIndex];
    matrixDrawPixel(i, rgb);
  }
}

void drawMultiple(int arr[25], int colors[2], int times, int delaysMs) {
  for (int t = 0; t < times; t++) {
    drawNumberArray(arr, colors);
    delay(delaysMs);
  }
}

// Clear Matrix with black
void matrixOff() {
  matrixFill(0x000000);
}

// Each glyph is five rows of three bits, most-significant bit on the left.
// Unsupported characters intentionally render as a blank space.
uint8_t matrixGlyphRow(char c, uint8_t row) {
  c = toupper((unsigned char)c);
  static const uint8_t digits[10][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,7}
  };
  static const uint8_t letters[26][5] = {
    {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},
    {7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},
    {5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},{5,7,7,7,5},{2,5,5,5,2},
    {6,5,6,4,4},{2,5,5,7,3},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},
    {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},{7,1,2,4,7}
  };
  if (c >= '0' && c <= '9') return digits[c - '0'][row];
  if (c >= 'A' && c <= 'Z') return letters[c - 'A'][row];
  switch (c) {
    case '-': return row == 2 ? 7 : 0;
    case '_': return row == 4 ? 7 : 0;
    case '.': return row == 4 ? 2 : 0;
    case ':': return (row == 1 || row == 3) ? 2 : 0;
    case '!': return (row == 0 || row == 1 || row == 2 || row == 4) ? 2 : 0;
    case '/': return row == 0 ? 1 : row == 1 ? 1 : row == 2 ? 2 : row == 3 ? 4 : 4;
    default: return 0;
  }
}

int matrixTextColor() {
  // Contrast is based on the source tally colour rather than its brightness-
  // scaled output, so dim white still receives a dark glyph.
  const int r = lastColorR;
  const int g = lastColorG;
  const int b = lastColorB;
  // Use a high-contrast foreground so text remains readable on tally colours.
  return (r * 299 + g * 587 + b * 114 > 150000) ? RGB_COLOR_BLACK : RGB_COLOR_WARMWHITE;
}

bool isCompactTeenLabel() {
  return matrixText.length() == 2 && matrixText[0] == '1' &&
         matrixText[1] >= '0' && matrixText[1] <= '9';
}

void renderMatrixText() {
  matrixFill(matrixBackgroundColor);
  if (!matrixText.length()) return;

  const int foreground = matrixTextColor();
  if (isCompactTeenLabel()) {
    if (matrixText[1] == '1') {
      // Give 11 a balanced layout: vertical strokes in human-numbered
      // columns 2 and 4 (zero-based matrix columns 1 and 3).
      for (uint8_t y = 0; y < 5; y++) {
        matrixDrawPixel(y * 5 + 1, foreground);
        matrixDrawPixel(y * 5 + 3, foreground);
      }
      return;
    }
    // Fit 10-19 without scrolling: a one-column leading 1, one blank spacer,
    // then the normal three-column glyph for the second digit.
    for (uint8_t y = 0; y < 5; y++) {
      matrixDrawPixel(y * 5, foreground);
      const uint8_t bits = matrixGlyphRow(matrixText[1], y);
      for (uint8_t x = 0; x < 3; x++) {
        if (bits & (4 >> x)) matrixDrawPixel(y * 5 + x + 2, foreground);
      }
    }
    return;
  }

  const int textWidth = matrixText.length() * 4 - 1;
  const int startX = textWidth <= 5 ? (5 - textWidth) / 2 : -matrixTextScroll;
  for (uint16_t character = 0; character < matrixText.length(); character++) {
    const int glyphX = startX + character * 4;
    for (uint8_t y = 0; y < 5; y++) {
      const uint8_t bits = matrixGlyphRow(matrixText[character], y);
      for (uint8_t x = 0; x < 3; x++) {
        const int panelX = glyphX + x;
        if ((bits & (4 >> x)) && panelX >= 0 && panelX < 5) matrixDrawPixel(y * 5 + panelX, foreground);
      }
    }
  }
}

void updateMatrixTextScroll() {
  if (!matrixText.length() || isCompactTeenLabel() || matrixText.length() * 4 - 1 <= 5) return;
  const unsigned long now = millis();
  if (now - lastTextScrollTime < textScrollIntervalMs) return;
  lastTextScrollTime = now;
  const int scrollWidth = matrixText.length() * 4 + 5; // blank gap before repeating
  matrixTextScroll = (matrixTextScroll + 1) % scrollWidth;
  renderMatrixText();
}

int matrixStatusColor() {
  switch (matrixStatus) {
    case STATUS_CONFIG:    return RGB_COLOR_ORANGE;
    case STATUS_CONNECTED: return RGB_COLOR_GREEN;
    case STATUS_ERROR:     return RGB_COLOR_RED;
    case STATUS_WIFI:      return RGB_COLOR_BLUE;
    default:               return RGB_COLOR_DIMWHITE;
  }
}

void renderMatrixStatus() {
  if (matrixText.length()) {
    renderMatrixText();
    return;
  }
  if (!tallyActive) matrixFill(RGB_COLOR_BLACK);
  if (matrixStatus == STATUS_CONNECTED && matrixConnectedIndicatorHidden) {
    if (tallyActive) matrixDrawPixel(0, matrixBackgroundColor);
    return;
  }
  matrixDrawPixel(0, matrixStatusColor());
}

void setMatrixStatus(uint8_t status) {
  const MatrixStatus nextStatus = static_cast<MatrixStatus>(status);
  if (nextStatus == STATUS_CONNECTED) {
    if (matrixStatus != STATUS_CONNECTED) {
      matrixConnectedSince = millis();
      matrixConnectedIndicatorHidden = false;
    }
  } else {
    matrixConnectedSince = 0;
    matrixConnectedIndicatorHidden = false;
  }
  matrixStatus = nextStatus;
  renderMatrixStatus();
}

void updateMatrixStatusIndicator() {
  if (matrixStatus == STATUS_CONNECTED && !matrixConnectedIndicatorHidden &&
      millis() - matrixConnectedSince >= connectedStatusDisplayMs) {
    matrixConnectedIndicatorHidden = true;
    renderMatrixStatus();
  }
}

// -------------------------------------------------------------------
// Config param helpers
// -------------------------------------------------------------------
#ifndef ATOMIC_POE_BUILD
String getParam(const String& name) {
  if (wifiManager.server && wifiManager.server->hasArg(name))
    return wifiManager.server->arg(name);
  return "";
}

void saveParamCallback() {
  String str_companionIP   = getParam("companionIP");
  String str_companionPort = getParam("companionPort");
  String str_rotation      = getParam("rotation");

  preferences.begin("companion", false);
  if (str_companionIP.length() > 0)   preferences.putString("companionip", str_companionIP);
  if (str_companionPort.length() > 0) preferences.putString("companionport", str_companionPort);
  if (str_rotation == "0" || str_rotation == "90" || str_rotation == "180" || str_rotation == "270")
    preferences.putInt("rotation", matrixRotation = str_rotation.toInt() / 90);
  preferences.end();
}
#endif

// ------------------------------------------------------------
// Config portal functions
// ------------------------------------------------------------
#ifndef ATOMIC_POE_BUILD
void startConfigPortal() {
  Serial.println("[WiFi] Entering CONFIG PORTAL mode");
  setMatrixStatus(STATUS_CONFIG);
  
  // Load Companion config from preferences (for default field values)
  preferences.begin("companion", true);
  String savedHost = preferences.getString("companionip", "Companion IP");
  String savedPort = preferences.getString("companionport", "16622");
  String savedRotation = String(preferences.getInt("rotation", 0) * 90);
  preferences.end();

  // Prepare WiFiManager with params
  custom_companionIP   = new WiFiManagerParameter("companionIP", "Companion IP", savedHost.c_str(), 40);
  custom_companionPort = new WiFiManagerParameter("companionPort", "Satellite Port", savedPort.c_str(), 6);
  custom_rotation      = new WiFiManagerParameter("rotation", "Matrix rotation (0, 90, 180, 270)", savedRotation.c_str(), 4);

  wifiManager.addParameter(custom_companionIP);
  wifiManager.addParameter(custom_companionPort);
  wifiManager.addParameter(custom_rotation);
  wifiManager.setSaveParamsCallback(saveParamCallback);

  std::vector<const char*> menu = { "wifi", "param", "info", "sep", "restart", "exit" };
  wifiManager.setMenu(menu);
  wifiManager.setClass("invert");
  wifiManager.setConfigPortalTimeout(0); // No timeout when we explicitly call config mode

  wifiManager.setAPCallback([](WiFiManager* wm) {
    Serial.println("[WiFi] Config portal started");
    // Draw WiFi icon in orange when portal is active
    int wificolor[] = {RGB_COLOR_BLACK, RGB_COLOR_ORANGE};
    drawNumberArray(icons[1], wificolor);
  });

  // Show setup icons while portal is active
  drawNumberArray(icons[7], infocolor);

  // Start AP + portal, blocks until user saves or exits
  String shortDeviceID = "m5atom-matrix_" + deviceID.substring(deviceID.length() - 5);
  wifiManager.startConfigPortal(shortDeviceID.c_str(), AP_password);
  Serial.printf("[WiFi] Config portal started - SSID: %s\n", shortDeviceID.c_str());

  // After returning, update our Companion host/port and persist
  strncpy(companion_host, custom_companionIP->getValue(), sizeof(companion_host));
  companion_host[sizeof(companion_host) - 1] = '\0';

  strncpy(companion_port, custom_companionPort->getValue(), sizeof(companion_port));
  companion_port[sizeof(companion_port) - 1] = '\0';
  const int rotationDegrees = String(custom_rotation->getValue()).toInt();
  if (rotationDegrees == 0 || rotationDegrees == 90 || rotationDegrees == 180 || rotationDegrees == 270)
    matrixRotation = rotationDegrees / 90;

  // Save to preferences
  preferences.begin("companion", false);
  preferences.putString("companionip", String(companion_host));
  preferences.putString("companionport", String(companion_port));
  preferences.putInt("rotation", matrixRotation);
  preferences.end();

  Serial.println("[WiFi] Config portal completed");
  Serial.printf("[WiFi] Companion Host: %s\n", companion_host);
  Serial.printf("[WiFi] Companion Port: %s\n", companion_port);
}
#endif

// -------------------------------------------------------------------
// External LED + Matrix color handling
// -------------------------------------------------------------------
void setExternalLedColor(uint8_t r, uint8_t g, uint8_t b) {
  lastColorR = r;
  lastColorG = g;
  lastColorB = b;

  Serial.print("[COLOR] raw r/g/b = ");
  Serial.print(r); Serial.print("/");
  Serial.print(g); Serial.print("/");
  Serial.println(b);

  // Atomic PoE owns these four pins, so only Wi-Fi builds drive the LED.
#ifndef ATOMIC_POE_BUILD
  writePwm(LED_PIN_RED, LEDC_CHANNEL_RED, ledEnabled ? min(255, int(r) * ledBrightnessPercent / 100) : 0);
  writePwm(LED_PIN_GREEN, LEDC_CHANNEL_GREEN, ledEnabled ? min(255, int(g) * ledBrightnessPercent / 100) : 0);
  writePwm(LED_PIN_BLUE, LEDC_CHANNEL_BLUE, ledEnabled ? min(255, int(b) * ledBrightnessPercent / 100) : 0);
#endif

  // Light the matrix in the tally colour, with one status pixel overlaid.
  int rgb = (r << 16) | (g << 8) | b;
  matrixBackgroundColor = rgb;
  matrixFill(rgb);
  tallyActive = (r != 0 || g != 0 || b != 0);
  renderMatrixStatus();
}

// -------------------------------------------------------------------
// Companion / Satellite API
// -------------------------------------------------------------------
void sendAddDevice() {
  String cmd;
  String companionDeviceID = "m5atom-matrix:" + deviceID.substring(deviceID.length() - 5); // Use last 5 chars like LEDMatrixClock
  
  cmd = "ADD-DEVICE DEVICEID=" + companionDeviceID +
        " PRODUCT_NAME=\"M5 Atom Matrix\" "
        "KEYS_TOTAL=1 KEYS_PER_ROW=1 "
        "COLORS=rgb TEXT=true BITMAPS=0";
  client.println(cmd);
  Serial.println("[API] Sent: " + cmd);
}

String decodeCompanionText(const String& encoded) {
  // Companion sends text as base64.  This small decoder accepts plain text as
  // well, which keeps manual API testing convenient.
  const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int value = 0;
  int bits = -8;
  String decoded;
  for (uint16_t i = 0; i < encoded.length(); i++) {
    const char c = encoded[i];
    if (c == '=') break;
    const char* pos = strchr(alphabet, c);
    if (!pos) return encoded;
    value = (value << 6) | (pos - alphabet);
    bits += 6;
    if (bits >= 0) {
      decoded += char((value >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return decoded.length() ? decoded : encoded;
}

void handleKeyStateText(const String& line) {
  const int textPos = line.indexOf("TEXT=");
  if (textPos < 0) return;
  const int firstQuote = line.indexOf('"', textPos);
  if (firstQuote < 0) return;
  const int secondQuote = line.indexOf('"', firstQuote + 1);
  if (secondQuote < 0) return;

  String text = decodeCompanionText(line.substring(firstQuote + 1, secondQuote));
  text.replace("\\n", " ");
  matrixText = text;
  matrixTextScroll = 0;
  lastTextScrollTime = millis();
  Serial.println("[API] TEXT = \"" + matrixText + "\"");
  renderMatrixStatus();
}

void handleKeyState(const String& line) {
  Serial.println("[API] KEY-STATE line: " + line);

  // COLOR="rgba(r,g,b,a)"
  int colorPos = line.indexOf("COLOR=");
  if (colorPos >= 0) {
    int start = colorPos + 6;
    int end = line.indexOf(' ', start);
    if (end < 0) end = line.length();
    String c = line.substring(start, end);
    c.trim();

    Serial.println("[API] COLOR raw: " + c);

    if (c.startsWith("\"") && c.endsWith("\""))
      c = c.substring(1, c.length() - 1);

    if (c.startsWith("rgba(")) {
      c.replace("rgba(", "");
      c.replace(")", "");
      c.replace(" ", "");

      int p1 = c.indexOf(',');
      int p2 = c.indexOf(',', p1+1);
      int p3 = c.indexOf(',', p2+1);

      int r = c.substring(0, p1).toInt();
      int g = c.substring(p1+1, p2).toInt();
      int b = c.substring(p2+1, p3).toInt();

      Serial.print("[API] Parsed COLOR r/g/b = ");
      Serial.print(r); Serial.print("/");
      Serial.print(g); Serial.print("/");
      Serial.println(b);

      setExternalLedColor(r, g, b);
    } else {
      Serial.println("[API] COLOR is not rgba(), ignoring.");
    }
  } else {
    Serial.println("[API] No COLOR= field in KEY-STATE.");
  }
}

void parseAPI(const String& apiData) {
  if (apiData.length() == 0) return;
  if (apiData.startsWith("PONG"))   return;

  Serial.println("[API] RX: " + apiData);

  if (apiData.startsWith("PING")) {
    String payload = apiData.substring(apiData.indexOf(' ') + 1);
    client.println("PONG " + payload);
    return;
  }

  if (apiData.startsWith("BRIGHTNESS")) {
    int valPos = apiData.indexOf("VALUE=");
    if (valPos < 0) return;
    String v = apiData.substring(valPos + 6);
    brightness = constrain(v.toInt(), 0, 100);
    applyMatrixBrightness();
    Serial.println("[API] BRIGHTNESS set to " + String(brightness) +
                   " (M5 brightness " + String(map(brightness, 0, 100, 0, MATRIX_MAX_BRIGHTNESS_PERCENT)) + "/100)");
    return;
  }

  if (apiData.startsWith("KEYS-CLEAR")) {
    Serial.println("[API] KEYS-CLEAR received");
    matrixOff();
    matrixText = "";
    tallyActive = false;
    renderMatrixStatus();
    setExternalLedColor(0,0,0);
    return;
  }

  if (apiData.startsWith("KEY-STATE")) {
    handleKeyState(apiData);
    handleKeyStateText(apiData);
    return;
  }
}

// ------------------------------------------------------------
// REST API Handlers for Companion Configuration
// ------------------------------------------------------------
void handleGetHost() {
  Serial.println("[REST] GET /api/host request received");
  Serial.println("[REST] Current companion_host: '" + String(companion_host) + "'");
  restServer.send(200, "text/plain", companion_host);
  Serial.println("[REST] GET /api/host: " + String(companion_host));
}

void handleGetPort() {
  Serial.println("[REST] GET /api/port request received");
  Serial.println("[REST] Current companion_port: '" + String(companion_port) + "'");
  restServer.send(200, "text/plain", companion_port);
  Serial.println("[REST] GET /api/port: " + String(companion_port));
}

void handleGetConfig() {
  String json = "{\"host\":\"" + String(companion_host) + "\",\"port\":" + String(companion_port) + "}";
  Serial.println("[REST] GET /api/config request received");
  Serial.println("[REST] Response JSON: " + json);
  restServer.send(200, "application/json", json);
  Serial.println("[REST] GET /api/config: " + json);
}

String jsonSetting(const String& body, const char* name) {
  const String key = String("\"") + name + "\"";
  int pos = body.indexOf(key); if (pos < 0) return "";
  pos = body.indexOf(':', pos + key.length()); if (pos < 0) return "";
  pos++; while (pos < body.length() && isspace(body[pos])) pos++;
  if (pos < body.length() && body[pos] == '\"') { const int end = body.indexOf('\"', ++pos); return end < 0 ? "" : body.substring(pos, end); }
  int end = pos; while (end < body.length() && body[end] != ',' && body[end] != '}') end++;
  String value = body.substring(pos, end); value.trim(); return value;
}

void handleSerialProvisioning() {
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n') {
      serialProvisionBuffer.trim();
      if (serialProvisionBuffer.startsWith("PROVISION ")) {
        const String body = serialProvisionBuffer.substring(10);
        const String ssid = jsonSetting(body, "ssid"), password = jsonSetting(body, "password");
        const String host = jsonSetting(body, "companionHost"), port = jsonSetting(body, "companionPort"), name = jsonSetting(body, "deviceName");
        if (port.length() && (port.toInt() < 1 || port.toInt() > 65535)) {
          Serial.println("PROVISION-ERROR invalid companionPort");
        } else {
          preferences.begin("companion", false);
          if (host.length()) { host.toCharArray(companion_host, sizeof(companion_host)); preferences.putString("companionip", host); }
          if (port.length()) { port.toCharArray(companion_port, sizeof(companion_port)); preferences.putString("companionport", port); }
          if (name.length()) { configuredDeviceName = name.substring(0, 48); preferences.putString("deviceName", configuredDeviceName); }
          preferences.end();
          Serial.println("PROVISION-OK");
#ifndef ATOMIC_POE_BUILD
          if (ssid.length()) { delay(100); WiFi.persistent(true); WiFi.begin(ssid.c_str(), password.c_str()); }
#endif
        }
      }
      serialProvisionBuffer = "";
    } else if (c != '\r' && serialProvisionBuffer.length() < 512) serialProvisionBuffer += c;
  }
}

void handleGetSettings() {
  restServer.send(200, "application/json", "{\"brightness\":" + String(brightness) + ",\"rotation\":" + String(matrixRotation * 90) + ",\"rgbScale\":" + String(matrixOutputPercent) + ",\"redScale\":" + String(matrixRedPercent) + ",\"greenScale\":" + String(matrixGreenPercent) + ",\"blueScale\":" + String(matrixBluePercent) + ",\"ledEnabled\":" + String(ledEnabled ? "true" : "false") + ",\"ledBrightness\":" + String(ledBrightnessPercent) + "}");
}

void handlePostSettings() {
  const String brightnessValue = jsonSetting(restServer.arg("plain"), "brightness");
  const String rotationValue = jsonSetting(restServer.arg("plain"), "rotation");
  const String rgbScaleValue = jsonSetting(restServer.arg("plain"), "rgbScale");
  const String redScaleValue = jsonSetting(restServer.arg("plain"), "redScale");
  const String greenScaleValue = jsonSetting(restServer.arg("plain"), "greenScale");
  const String blueScaleValue = jsonSetting(restServer.arg("plain"), "blueScale");
  const String ledEnabledValue = jsonSetting(restServer.arg("plain"), "ledEnabled");
  const String ledBrightnessValue = jsonSetting(restServer.arg("plain"), "ledBrightness");
  if (brightnessValue.length() && (brightnessValue.toInt() < 0 || brightnessValue.toInt() > 100)) { restServer.send(400, "text/plain", "brightness must be 0-100"); return; }
  if (rotationValue.length() && !(rotationValue == "0" || rotationValue == "90" || rotationValue == "180" || rotationValue == "270")) { restServer.send(400, "text/plain", "rotation must be 0, 90, 180, or 270"); return; }
  if (rgbScaleValue.length() && (rgbScaleValue.toInt() < 0 || rgbScaleValue.toInt() > MATRIX_MAX_RGB_SCALE_PERCENT)) { restServer.send(400, "text/plain", "rgbScale must be 0-100"); return; }
  if (redScaleValue.length() && (redScaleValue.toInt() < 0 || redScaleValue.toInt() > 100)) { restServer.send(400, "text/plain", "redScale must be 0-100"); return; }
  if (greenScaleValue.length() && (greenScaleValue.toInt() < 0 || greenScaleValue.toInt() > 100)) { restServer.send(400, "text/plain", "greenScale must be 0-100"); return; }
  if (blueScaleValue.length() && (blueScaleValue.toInt() < 0 || blueScaleValue.toInt() > 100)) { restServer.send(400, "text/plain", "blueScale must be 0-100"); return; }
  if (ledEnabledValue.length() && !(ledEnabledValue == "true" || ledEnabledValue == "false")) { restServer.send(400, "text/plain", "ledEnabled must be true or false"); return; }
  if (ledBrightnessValue.length() && (ledBrightnessValue.toInt() < 0 || ledBrightnessValue.toInt() > 200)) { restServer.send(400, "text/plain", "ledBrightness must be 0-200"); return; }
  if (!brightnessValue.length() && !rotationValue.length() && !rgbScaleValue.length() && !redScaleValue.length() && !greenScaleValue.length() && !blueScaleValue.length() && !ledEnabledValue.length() && !ledBrightnessValue.length()) { restServer.send(400, "text/plain", "provide a setting to update"); return; }
  if (brightnessValue.length()) {
    brightness = brightnessValue.toInt();
    applyMatrixBrightness();
  }
  if (rotationValue.length()) matrixRotation = rotationValue.toInt() / 90;
  if (rgbScaleValue.length()) matrixOutputPercent = rgbScaleValue.toInt();
  if (redScaleValue.length()) matrixRedPercent = redScaleValue.toInt();
  if (greenScaleValue.length()) matrixGreenPercent = greenScaleValue.toInt();
  if (blueScaleValue.length()) matrixBluePercent = blueScaleValue.toInt();
  if (ledEnabledValue.length()) ledEnabled = ledEnabledValue == "true";
  if (ledBrightnessValue.length()) ledBrightnessPercent = ledBrightnessValue.toInt();
  preferences.begin("companion", false);
  preferences.putInt("brightness", brightness);
  preferences.putInt("rotation", matrixRotation);
  preferences.putInt("rgbscale", matrixOutputPercent);
  preferences.putInt("redscale", matrixRedPercent);
  preferences.putInt("greenscale", matrixGreenPercent);
  preferences.putInt("bluescale", matrixBluePercent);
  preferences.putBool("ledEnabled", ledEnabled);
  preferences.putInt("ledBrightness", ledBrightnessPercent);
  preferences.end();
  setExternalLedColor(lastColorR, lastColorG, lastColorB);
  renderMatrixStatus();
  restServer.send(200, "application/json", "{\"ok\":true}");
}

void handlePostHardwareTest() {
  const String body = restServer.arg("plain");
  const String target = jsonSetting(body, "target");
  const String value = jsonSetting(body, "value");
  int color = -1;
  if (value == "red") color = 0xFF0000;
  else if (value == "green") color = 0x00FF00;
  else if (value == "blue") color = 0x0000FF;
  else if (value == "white") color = 0xFFFFFF;
  else if (value == "off") color = 0x000000;
  if (target == "led" && color >= 0) {
    setExternalLedColor((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
  } else if (target == "display" && color >= 0) {
    matrixText = "";
    matrixFill(color);
  } else if (target == "text" && value.length()) {
    matrixText = value.substring(0, 32);
    matrixTextScroll = 0;
    renderMatrixText();
  } else {
    restServer.send(400, "text/plain", "Use target led/display with red, green, blue, white, or off; or target text");
    return;
  }
  restServer.send(200, "application/json", "{\"ok\":true}");
}

void handlePostHost() {
  String newHost = "";
  
  Serial.println("[REST] POST /api/host request received");
  
  // Try to parse JSON first
  if (restServer.hasArg("plain")) {
    String body = restServer.arg("plain");
    body.trim();
    Serial.println("[REST] Request body: '" + body + "'");
    
    // Check if it's JSON format
    if (body.startsWith("{") && body.endsWith("}")) {
      Serial.println("[REST] Parsing JSON format");
      int hostPos = body.indexOf("\"host\":");
      if (hostPos >= 0) {
        int startQuote = body.indexOf("\"", hostPos + 7);
        int endQuote = body.indexOf("\"", startQuote + 1);
        if (startQuote >= 0 && endQuote > startQuote) {
          newHost = body.substring(startQuote + 1, endQuote);
          Serial.println("[REST] Extracted host from JSON: '" + newHost + "'");
        }
      }
    } else {
      // Plain text format
      newHost = body;
      Serial.println("[REST] Using plain text format: '" + newHost + "'");
    }
  } else {
    Serial.println("[REST] No plain body found in request");
  }
  
  newHost.trim();
  
  if (newHost.length() > 0 && newHost.length() < sizeof(companion_host)) {
    strncpy(companion_host, newHost.c_str(), sizeof(companion_host));
    companion_host[sizeof(companion_host) - 1] = '\0';
    
    Serial.println("[REST] Updating companion_host to: '" + String(companion_host) + "'");
    
    // Save to preferences
    Serial.println("[REST] Saving to preferences...");
    preferences.begin("companion", false);
    preferences.putString("companionip", String(companion_host));
    preferences.end();
    Serial.println("[REST] Preferences saved");
    
#ifndef ATOMIC_POE_BUILD
    // Keep the Wi-Fi portal field in sync.
    if (custom_companionIP) {
      custom_companionIP->setValue(companion_host, sizeof(companion_host));
      Serial.println("[REST] WiFiManager parameter updated");
    }
#endif
    
    restServer.send(200, "text/plain", "OK");
    Serial.println("[REST] POST /api/host: Updated to " + String(companion_host));
    
    // Reestablish connection
    if (client.connected()) {
      client.stop();
    }
  } else {
    Serial.println("[REST] Invalid host - length: " + String(newHost.length()) + " content: '" + newHost + "'");
    restServer.send(400, "text/plain", "Invalid host");
    Serial.println("[REST] POST /api/host: Invalid host - " + newHost);
  }
}

void handlePostPort() {
  String newPort = "";
  
  // Try to parse JSON first
  if (restServer.hasArg("plain")) {
    String body = restServer.arg("plain");
    body.trim();
    
    // Check if it's JSON format
    if (body.startsWith("{") && body.endsWith("}")) {
      int portPos = body.indexOf("\"port\":");
      if (portPos >= 0) {
        int startQuote = body.indexOf("\"", portPos + 7);
        int endQuote = body.indexOf("\"", startQuote + 1);
        if (startQuote >= 0 && endQuote > startQuote) {
          newPort = body.substring(startQuote + 1, endQuote);
        }
      }
    } else {
      // Plain text format
      newPort = body;
    }
  }
  
  newPort.trim();
  
  // Validate port number
  int portNum = newPort.toInt();
  if (portNum > 0 && portNum <= 65535) {
    strncpy(companion_port, newPort.c_str(), sizeof(companion_port));
    companion_port[sizeof(companion_port) - 1] = '\0';
    
    // Save to preferences
    preferences.begin("companion", false);
    preferences.putString("companionport", String(companion_port));
    preferences.end();
    
#ifndef ATOMIC_POE_BUILD
    if (custom_companionPort) {
      custom_companionPort->setValue(companion_port, sizeof(companion_port));
    }
#endif
    
    restServer.send(200, "text/plain", "OK");
    Serial.println("[REST] POST /api/port: Updated to " + String(companion_port));
    
    // Reestablish connection
    if (client.connected()) {
      client.stop();
    }
  } else {
    restServer.send(400, "text/plain", "Invalid port number");
    Serial.println("[REST] POST /api/port: Invalid port - " + newPort);
  }
}

void handlePostConfig() {
  String newHost = "";
  String newPort = "";
  
  Serial.println("[REST] POST /api/config request received");
  
  if (restServer.hasArg("plain")) {
    String body = restServer.arg("plain");
    body.trim();
    Serial.println("[REST] Request body: '" + body + "'");
    
    if (body.startsWith("{") && body.endsWith("}")) {
      Serial.println("[REST] Parsing JSON format");
      
      // Parse host
      int hostPos = body.indexOf("\"host\":");
      if (hostPos >= 0) {
        int startQuote = body.indexOf("\"", hostPos + 7);
        int endQuote = body.indexOf("\"", startQuote + 1);
        if (startQuote >= 0 && endQuote > startQuote) {
          newHost = body.substring(startQuote + 1, endQuote);
          Serial.println("[REST] Extracted host from JSON: '" + newHost + "'");
        }
      }
      
      // Parse port
      int portPos = body.indexOf("\"port\":");
      if (portPos >= 0) {
        // Try quoted port first
        int startQuote = body.indexOf("\"", portPos + 7);
        int endQuote = body.indexOf("\"", startQuote + 1);
        if (startQuote >= 0 && endQuote > startQuote) {
          newPort = body.substring(startQuote + 1, endQuote);
          Serial.println("[REST] Extracted quoted port from JSON: '" + newPort + "'");
        } else {
          // Try unquoted port number
          int startNum = portPos + 7;
          // Skip whitespace and colon
          while (startNum < body.length() && (body.charAt(startNum) == ' ' || body.charAt(startNum) == ':')) {
            startNum++;
          }
          // Find the end by looking for comma or closing brace
          int endNumComma = body.indexOf(",", startNum);
          int endNumBrace = body.indexOf("}", startNum);
          int endNum = -1;
          
          // Use the closest delimiter
          if (endNumComma >= 0 && endNumBrace >= 0) {
            endNum = (endNumComma < endNumBrace) ? endNumComma : endNumBrace;
          } else if (endNumComma >= 0) {
            endNum = endNumComma;
          } else if (endNumBrace >= 0) {
            endNum = endNumBrace;
          }
          
          if (endNum >= 0) {
            newPort = body.substring(startNum, endNum);
            newPort.trim();
            Serial.println("[REST] Extracted unquoted port from JSON: '" + newPort + "'");
          }
        }
      }
    }
  }
  
  newHost.trim();
  newPort.trim();
  
  // Validate
  bool hostValid = (newHost.length() > 0 && newHost.length() < sizeof(companion_host));
  int portNum = newPort.toInt();
  bool portValid = (portNum > 0 && portNum <= 65535);
  
  if (hostValid && portValid) {
    strncpy(companion_host, newHost.c_str(), sizeof(companion_host));
    companion_host[sizeof(companion_host) - 1] = '\0';
    strncpy(companion_port, newPort.c_str(), sizeof(companion_port));
    companion_port[sizeof(companion_port) - 1] = '\0';
    
    // Save to preferences
    preferences.begin("companion", false);
    preferences.putString("companionip", String(companion_host));
    preferences.putString("companionport", String(companion_port));
    preferences.end();
    
#ifndef ATOMIC_POE_BUILD
    if (custom_companionIP) {
      custom_companionIP->setValue(companion_host, sizeof(companion_host));
    }
    if (custom_companionPort) {
      custom_companionPort->setValue(companion_port, sizeof(companion_port));
    }
#endif
    
    restServer.send(200, "text/plain", "OK");
    Serial.println("[REST] POST /api/config: Updated host=" + String(companion_host) + " port=" + String(companion_port));
    
    // Reestablish connection
    if (client.connected()) {
      client.stop();
    }
  } else {
    Serial.println("[REST] Invalid config - host: '" + newHost + "' (valid: " + String(hostValid) + ") port: '" + newPort + "' (valid: " + String(portValid) + ")");
    restServer.send(400, "text/plain", "Invalid config");
  }
}

const char* firmwareUpdateUser = "admin";
// Empty by default: updates are open until the owner elects to protect them.
String firmwareUpdatePassword = "";

bool requireFirmwareUpdateAuth() {
  if (firmwareUpdatePassword.length() == 0 || restServer.authenticate(firmwareUpdateUser, firmwareUpdatePassword.c_str())) return true;
  restServer.requestAuthentication();
  return false;
}

void handleFirmwareUpdatePage() {
  if (!requireFirmwareUpdateAuth()) return;
  restServer.send(200, "text/html", "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><h2>Firmware update</h2><p>Select the release application <code>.bin</code> file. Do not power off while updating.</p><form method=POST action=/update enctype=multipart/form-data><input type=file name=firmware accept='.bin' required><button type=submit>Install and reboot</button></form><hr><h3>Optional protection</h3><form method=POST action=/update/password><input type=password name=password placeholder='Leave blank to remove'><button type=submit>Save update password</button></form>");
}

void handleFirmwareUpload() {
  if (firmwareUpdatePassword.length() && !restServer.authenticate(firmwareUpdateUser, firmwareUpdatePassword.c_str())) return;
  auto& upload = restServer.upload();
  if (upload.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
  else if (upload.status == UPLOAD_FILE_WRITE) Update.write(upload.buf, upload.currentSize);
  else if (upload.status == UPLOAD_FILE_END) Update.end(true);
  else if (upload.status == UPLOAD_FILE_ABORTED) Update.abort();
}

void handleFirmwareUpdateResult() {
  if (!requireFirmwareUpdateAuth()) return;
  const bool success = !Update.hasError();
  restServer.send(success ? 200 : 500, "text/plain", success ? "Update complete. Rebooting..." : "Firmware update failed.");
  if (success) { delay(500); ESP.restart(); }
}

void handleFirmwareUpdatePassword() {
  if (!requireFirmwareUpdateAuth()) return;
  firmwareUpdatePassword = restServer.arg("password");
  preferences.begin("companion", false); preferences.putString("updatepassword", firmwareUpdatePassword); preferences.end();
  restServer.send(200, "text/plain", firmwareUpdatePassword.length() ? "Update password saved." : "Update password removed.");
}

String statusJsonEscape(String value) {
  value.replace("\\", "\\\\"); value.replace("\"", "\\\"");
  value.replace("\n", "\\n"); value.replace("\r", "\\r");
  return value;
}

void handleStatus() {
  String json = "{\"deviceName\":\"" + statusJsonEscape(configuredDeviceName.length() ? configuredDeviceName : "M5 Atom Matrix") + "\",\"deviceId\":\"" + statusJsonEscape(deviceID) + "\",\"firmware\":\"" FIRMWARE_VERSION "\",";
#ifdef ATOMIC_POE_BUILD
  json += "\"network\":\"ethernet\",\"networkConnected\":" + String(Ethernet.linkStatus() == LinkON ? "true" : "false") + ",";
#else
  json += "\"network\":\"wifi\",\"networkConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ssid\":\"" + statusJsonEscape(WiFi.SSID()) + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",";
#endif
  json += "\"companionConnected\":" + String(client.connected() ? "true" : "false") + ",";
  json += "\"companion\":\"" + statusJsonEscape(String(companion_host) + ":" + companion_port) + "\",";
  json += "\"brightness\":" + String(brightness) + ",\"controllerCap\":" + String(MATRIX_MAX_BRIGHTNESS_PERCENT) + ",\"fastLedBrightness\":" + String(matrixFastLedBrightness()) + ",\"rgbScale\":" + String(matrixOutputPercent) + ",\"redScale\":" + String(matrixRedPercent) + ",\"greenScale\":" + String(matrixGreenPercent) + ",\"blueScale\":" + String(matrixBluePercent) + ",\"ledEnabled\":" + String(ledEnabled ? "true" : "false") + ",\"ledBrightness\":" + String(ledBrightnessPercent) + ",\"buttonPressed\":" + String(M5.Btn.isPressed() ? "true" : "false") + ",\"statusIndicatorHidden\":" + String(matrixConnectedIndicatorHidden ? "true" : "false") + ",\"text\":\"" + statusJsonEscape(matrixText) + "\",";
  json += "\"color\":{\"r\":" + String(lastColorR) + ",\"g\":" + String(lastColorG) + ",\"b\":" + String(lastColorB) + "},";
  json += "\"uptimeSeconds\":" + String(millis() / 1000) + "}";
  restServer.send(200, "application/json", json);
}

void handleConfigPage() {
  const String html =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>M5 Atom Matrix</title><h2>M5 Atom Matrix</h2><h3>Live troubleshooting status</h3>"
    "<div id=state>Loading...</div><p>Incoming text: <code id=t>-</code></p>"
    "<p>Incoming colour: <span id=sw style='display:inline-block;width:2em;height:1em;border:1px solid'></span> <code id=c>-</code></p>"
    "<p>Network: "
#ifdef ATOMIC_POE_BUILD
    "Atomic PoE / W5500"
#else
    "Wi-Fi"
#endif
    "</p><label>Companion host <input id=h value='" + String(companion_host) +
    "'></label><br><label>Port <input id=p value='" + String(companion_port) +
    "'></label><br><button onclick=s()>Save Companion</button> <a href=/update>Firmware update</a>"
    "<hr><label>Matrix RGB scale (%) <input id=m type=number min=0 max=100 value='" + String(matrixOutputPercent) +
    "'></label><br><label>Red (%) <input id=rs type=number min=0 max=100 value='" + String(matrixRedPercent) +
    "'></label> <label>Green (%) <input id=gs type=number min=0 max=100 value='" + String(matrixGreenPercent) +
    "'></label> <label>Blue (%) <input id=bs type=number min=0 max=100 value='" + String(matrixBluePercent) +
    "'></label><br><button onclick=q()>Save matrix levels</button>"
    "<br><label><input id=le type=checkbox" + String(ledEnabled ? " checked" : "") + "> External RGB LED enabled</label> <label>LED scale <input id=lb type=number min=0 max=200 value='" + String(ledBrightnessPercent) + "'>%</label> <button onclick=e()>Save LED</button>"
    "<hr><b>Hardware tests</b><p>Button: <strong id=bt>released</strong></p>"
    "<p>External LED: <button onclick=tt('led','red')>Red</button> <button onclick=tt('led','green')>Green</button> <button onclick=tt('led','blue')>Blue</button> <button onclick=tt('led','white')>White</button> <button onclick=tt('led','off')>Off</button></p>"
    "<p>5x5 display: <button onclick=tt('display','red')>Red</button> <button onclick=tt('display','green')>Green</button> <button onclick=tt('display','blue')>Blue</button> <button onclick=tt('display','white')>White</button> <button onclick=tt('display','off')>Off</button></p>"
    "<p><input id=tx placeholder='Matrix test text'><button onclick=tt('text',tx.value)>Show text</button></p>"
    "<p><small>Master and per-channel scales are direct multipliers. LED controller brightness is capped at 100/100 (FastLED 40/255).</small></p>"
    "<pre id=o></pre><script>async function s(){let r=await fetch('/api/config',{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify({host:h.value,port:+p.value})});"
    "o.textContent=await r.text()}async function q(){let r=await fetch('/api/settings',{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify({rgbScale:+m.value,redScale:+rs.value,greenScale:+gs.value,blueScale:+bs.value})});"
    "o.textContent=await r.text()}async function e(){let r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ledEnabled:le.checked,ledBrightness:+lb.value})});o.textContent=await r.text()}async function tt(target,value){let r=await fetch('/api/test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({target,value})});o.textContent=await r.text()}async function u(){try{let x=await(await fetch('/api/status')).json();"
    "state.textContent=(x.networkConnected?'Network connected':'Network disconnected')+' | '+"
    "(x.companionConnected?'Companion connected':'Companion disconnected')+' | '+(x.ip||x.network);"
    "t.textContent=x.text||'(none)';bt.textContent=x.buttonPressed?'PRESSED':'released';let q=x.color;c.textContent=`rgb(${q.r}, ${q.g}, ${q.b})`;"
    "sw.style.background=`rgb(${q.r},${q.g},${q.b})`}catch(e){state.textContent='Status unavailable'}}"
    "u();setInterval(u,2000)</script>";
  restServer.send(200, "text/html", html);
}

void setupRestServer() {
  restServer.on("/", HTTP_GET, handleConfigPage);
  restServer.on("/api/host", HTTP_GET, handleGetHost);
  restServer.on("/api/port", HTTP_GET, handleGetPort);
  restServer.on("/api/config", HTTP_GET, handleGetConfig);
  restServer.on("/api/settings", HTTP_GET, handleGetSettings);
  restServer.on("/api/status", HTTP_GET, handleStatus);
  
  restServer.on("/api/host", HTTP_POST, handlePostHost);
  restServer.on("/api/port", HTTP_POST, handlePostPort);
  restServer.on("/api/config", HTTP_POST, handlePostConfig);
  restServer.on("/api/settings", HTTP_POST, handlePostSettings);
  restServer.on("/api/test", HTTP_POST, handlePostHardwareTest);
  restServer.on("/update", HTTP_GET, handleFirmwareUpdatePage);
  restServer.on("/update", HTTP_POST, handleFirmwareUpdateResult, handleFirmwareUpload);
  restServer.on("/update/password", HTTP_POST, handleFirmwareUpdatePassword);
  
  restServer.begin();
  Serial.println("[REST] REST API server started on port 9999");
  Serial.println("[REST] Available endpoints:");
  Serial.println("[REST]   GET  /api/host");
  Serial.println("[REST]   GET  /api/port");
  Serial.println("[REST]   GET  /api/config");
  Serial.println("[REST]   POST /api/host");
  Serial.println("[REST]   POST /api/port");
  Serial.println("[REST]   POST /api/config");
}

// -------------------------------------------------------------------
// WiFi + Config Portal
// -------------------------------------------------------------------
#ifndef ATOMIC_POE_BUILD
void connectToNetwork() {
  if (stationIP != IPAddress(0,0,0,0))
    wifiManager.setSTAStaticIPConfig(stationIP, stationGW, stationMask);

  WiFi.mode(WIFI_STA);
  logger("Connecting to SSID: " + String(WiFi.SSID()), "info");

  custom_companionIP   = new WiFiManagerParameter("companionIP", "Companion IP", companion_host, 40);
  custom_companionPort = new WiFiManagerParameter("companionPort", "Satellite Port", companion_port, 6);
  String rotationString = String(matrixRotation * 90);
  custom_rotation      = new WiFiManagerParameter("rotation", "Matrix rotation (0, 90, 180, 270)", rotationString.c_str(), 4);

  wifiManager.addParameter(custom_companionIP);
  wifiManager.addParameter(custom_companionPort);
  wifiManager.addParameter(custom_rotation);
  wifiManager.setSaveParamsCallback(saveParamCallback);

  std::vector<const char*> menu = { "wifi", "param", "info", "sep", "restart", "exit" };
  wifiManager.setMenu(menu);
  wifiManager.setClass("invert");
  wifiManager.setConfigPortalTimeout(180); // 3 minutes auto portal if WiFi fails

  // Set hostname in WiFiManager to prevent ESP32 default override
  String wifiHostname = "m5atom-matrix_" + deviceID.substring(deviceID.length() - 5);
  wifiManager.setHostname(wifiHostname.c_str());
  Serial.printf("[WiFi] WiFiManager hostname set to: %s\n", wifiHostname.c_str());

  wifiManager.setAPCallback([](WiFiManager* wm) {
    Serial.println("[WiFi] Config portal started");
    // Draw WiFi icon in orange when portal is active
    int wificolor[] = {RGB_COLOR_BLACK, RGB_COLOR_ORANGE};
    drawNumberArray(icons[1], wificolor);
  });

  // Normal autoConnect behaviour (connect to WiFi, or start portal if no WiFi)
  wifiManager.setConfigPortalBlocking(false);
  // WiFi connect animation (wifi rings)
  drawNumberArray(icons[3], wificolor);
  delay(300);
  drawNumberArray(icons[2], wificolor);
  delay(300);
  drawNumberArray(icons[1], wificolor);
  delay(300);

  // Use shortened device ID for WiFi portal name (underscore format)
  String shortDeviceID = "m5atom-matrix_" + deviceID.substring(deviceID.length() - 5);  // Use last 5 chars like LEDMatrixClock
  bool res = wifiManager.autoConnect(shortDeviceID.c_str(), AP_password);
  Serial.printf("[WiFi] AutoConnect - SSID: %s\n", shortDeviceID.c_str());

  if (!res) {
    logger("Failed to connect", "error");
    drawNumberArray(icons[9], badcolor); // error icon
    Serial.println("[WiFi] Failed to connect, starting config portal...");
    // WiFiManager will automatically start config portal on failure
    // No need to restart - let WiFiManager handle it
  } else {
    logger("connected...yay :)", "info");
    drawNumberArray(icons[11], readycolor); // good tick
    delay(400);
    
    // Verify and reset hostname after connection to ensure it persists
    String currentHostname = WiFi.getHostname();
    String expectedHostname = "m5atom-matrix_" + deviceID.substring(deviceID.length() - 5);
    if (currentHostname != expectedHostname) {
      Serial.printf("[WiFi] Hostname mismatch, resetting from '%s' to '%s'\n", currentHostname.c_str(), expectedHostname.c_str());
      WiFi.setHostname(expectedHostname.c_str());
      delay(100);
      Serial.printf("[WiFi] Hostname reset to: %s\n", WiFi.getHostname());
    } else {
      Serial.printf("[WiFi] Hostname confirmed: %s\n", currentHostname.c_str());
    }
  }
}
#else
void connectToNetwork() {
  uint8_t ethernetMac[6];
  esp_read_mac(ethernetMac, ESP_MAC_WIFI_STA);

  Serial.println("[Ethernet] Initialising Atomic PoE W5500");
  SPI.begin(22, 23, 33, -1);
  Ethernet.init(19);
  while (Ethernet.begin(ethernetMac, 15000, 4000) == 0) {
    Serial.println("[Ethernet] DHCP failed; retrying");
    drawNumberArray(icons[9], badcolor);
    delay(5000);
  }
  Serial.print("[Ethernet] DHCP address: ");
  Serial.println(Ethernet.localIP());
  drawNumberArray(icons[11], readycolor);
}
#endif

// -------------------------------------------------------------------
// Companion discovery
// -------------------------------------------------------------------
void initializeMDNS() {
#ifdef ATOMIC_POE_BUILD
  Serial.println("[mDNS] W5500 build: use the DHCP address and wired setup page");
  return;
#else
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[mDNS] WiFi is not connected; discovery is unavailable");
    return;
  }

  const String shortId = deviceID.substring(deviceID.length() - 5);
  const String hostname = "m5atom-matrix_" + shortId;
  const String instanceName = "m5atom-matrix:" + shortId;

  if (!MDNS.begin(hostname.c_str())) {
    Serial.println("[mDNS] Error setting up mDNS responder!");
    return;
  }

  MDNS.setInstanceName(instanceName.c_str());
  if (!MDNS.addService("companion-satellite", "tcp", 9999)) {
    Serial.println("[mDNS] companion-satellite service registration failed!");
    return;
  }

  MDNS.addServiceTxt("companion-satellite", "tcp", "restEnabled", "true");
  MDNS.addServiceTxt("companion-satellite", "tcp", "deviceId", shortId.c_str());
  MDNS.addServiceTxt("companion-satellite", "tcp", "prefix", "m5atom-matrix");
  MDNS.addServiceTxt("companion-satellite", "tcp", "productName", "M5 Atom Matrix");
  MDNS.addServiceTxt("companion-satellite", "tcp", "apiVersion", "4");
  Serial.printf("[mDNS] Ready: %s.local (%s)\n", hostname.c_str(), instanceName.c_str());
#endif
}

// -------------------------------------------------------------------
// SETUP
// -------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("Booting M5 Atom Matrix Companion v4…");

  // Build the stable device ID from the ESP32 factory MAC.
#ifndef ATOMIC_POE_BUILD
  WiFi.mode(WIFI_STA);
  delay(100);
  uint8_t mac[6];
  WiFi.macAddress(mac);
#else
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif

  char macBuf[13];
  sprintf(macBuf, "%02X%02X%02X%02X%02X%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  deviceID = "m5atom-matrix_";
  deviceID += macBuf;
  deviceID.toUpperCase();

  Serial.print("DeviceID: ");
  Serial.println(deviceID);

  // Load preferences (host/port override)
  preferences.begin("companion", false);
  if (preferences.getString("companionip").length() > 0)
    preferences.getString("companionip").toCharArray(companion_host, sizeof(companion_host));

  if (preferences.getString("companionport").length() > 0)
    preferences.getString("companionport").toCharArray(companion_port, sizeof(companion_port));
  brightness = preferences.getInt("brightness", 100);
  ledEnabled = preferences.getBool("ledEnabled", true);
  ledBrightnessPercent = constrain(preferences.getInt("ledBrightness", 100), 0, 200);
  configuredDeviceName = preferences.getString("deviceName", "");
  matrixRotation = preferences.getInt("rotation", 0);
  if (matrixRotation < 0 || matrixRotation > 3) matrixRotation = 0;
  matrixOutputPercent = preferences.getInt("rgbscale", MATRIX_MAX_RGB_SCALE_PERCENT);
  if (matrixOutputPercent < 0 || matrixOutputPercent > MATRIX_MAX_RGB_SCALE_PERCENT) matrixOutputPercent = MATRIX_MAX_RGB_SCALE_PERCENT;
  matrixRedPercent = preferences.getInt("redscale", 100);
  matrixGreenPercent = preferences.getInt("greenscale", 100);
  matrixBluePercent = preferences.getInt("bluescale", 100);
  if (matrixRedPercent < 0 || matrixRedPercent > 100) matrixRedPercent = 100;
  if (matrixGreenPercent < 0 || matrixGreenPercent > 100) matrixGreenPercent = 100;
  if (matrixBluePercent < 0 || matrixBluePercent > 100) matrixBluePercent = 100;
  firmwareUpdatePassword = preferences.getString("updatepassword", "");
  preferences.end();

  Serial.print("Companion Host: ");
  Serial.println(companion_host);
  Serial.print("Companion Port: ");
  Serial.println(companion_port);

  // Save battery by turning off Bluetooth
  btStop();

  // Init M5 Atom
  M5.begin(true, false, true);
  delay(50);
  applyMatrixBrightness();
  matrixOff();

  // Boot icon (simple “setup” sequence)
  drawNumberArray(icons[7], infocolor);
  delay(300);
  drawNumberArray(icons[8], infocolor);
  delay(300);
  drawNumberArray(icons[7], infocolor);
  delay(300);
  matrixOff();

  // External LED setup (the Atomic PoE base owns all four pins).
#ifndef ATOMIC_POE_BUILD
  pinMode(LED_PIN_GND, OUTPUT);
  digitalWrite(LED_PIN_GND, LOW);

  Serial.println("[LED] Initialising PWM (esp32-hal-ledc, pin-based)...");
  bool okR = attachPwm(LED_PIN_RED,   LEDC_CHANNEL_RED);
  bool okG = attachPwm(LED_PIN_GREEN, LEDC_CHANNEL_GREEN);
  bool okB = attachPwm(LED_PIN_BLUE,  LEDC_CHANNEL_BLUE);

  Serial.print("[LED] ledcAttach RED: ");   Serial.println(okR);
  Serial.print("[LED] ledcAttach GREEN: "); Serial.println(okG);
  Serial.print("[LED] ledcAttach BLUE: ");  Serial.println(okB);

  setExternalLedColor(0,0,0);

  Serial.println("[LED] Running power-on colour test (R/G/B)...");
  setExternalLedColor(255, 0, 0);
  delay(250);
  setExternalLedColor(0, 255, 0);
  delay(250);
  setExternalLedColor(0, 0, 255);
  delay(250);
  setExternalLedColor(0,0,0);
#endif

  // Match AtomS3 setup entry: hold the button while powering on/restarting.
#ifndef ATOMIC_POE_BUILD
  M5.update();
  if (M5.Btn.isPressed()) {
    Serial.println("[Boot] Button held: starting setup AP");
    startConfigPortal();
  }
#endif
  
  connectToNetwork();

  // ArduinoOTA is Wi-Fi-specific; both variants retain browser updates.
#ifndef ATOMIC_POE_BUILD
  ArduinoOTA.setHostname(deviceID.c_str());
  ArduinoOTA.setPassword("companion-satellite");
  ArduinoOTA.begin();
#endif

  // Start REST API server after WiFi is connected
  setupRestServer();

  initializeMDNS();

  // Show “waiting for Companion” icon (single dot)
  setMatrixStatus(STATUS_WIFI);
  
  Serial.println("[System] Setup complete, entering main loop.");
}

// -------------------------------------------------------------------
// LOOP
// -------------------------------------------------------------------
void loop() {
  // LED_DisPlay::run() resets FastLED brightness to 20 after each refresh.
  // Reapply the configured value continuously so it persists across frames.
  FastLED.setBrightness(matrixFastLedBrightness());
  M5.update();
#ifndef ATOMIC_POE_BUILD
  ArduinoOTA.handle();
#else
  Ethernet.maintain();
#endif
  restServer.handleClient();
#ifndef ATOMIC_POE_BUILD
  wifiManager.process();
#endif
  handleSerialProvisioning();

  unsigned long now = millis();
  updateMatrixTextScroll();
  updateMatrixStatusIndicator();

  // Companion connect / reconnect
  if (!client.connected() && (now - lastConnectTry >= connectRetryMs)) {
    lastConnectTry = now;
    Serial.print("[NET] Connecting to Companion ");
    Serial.print(companion_host);
    Serial.print(":");
    Serial.println(companion_port);

    if (client.connect(companion_host, atoi(companion_port))) {
      Serial.println("[NET] Connected to Companion API");
      setMatrixStatus(STATUS_CONNECTED);
      // Good icon when Companion connects
      drawNumberArray(icons[11], readycolor);
      delay(300);
      renderMatrixStatus();
      sendAddDevice();
      lastPingTime = millis();
    } else {
      Serial.println("[NET] Companion connect failed");
      setMatrixStatus(STATUS_ERROR);
      drawNumberArray(icons[9], badcolor); // error icon briefly
      delay(200);
      renderMatrixStatus();
    }
  }

  if (client.connected()) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) parseAPI(line);
    }

    if (M5.Btn.wasPressed()) {
      Serial.println("[BTN] Short press -> KEY=0 PRESSED=true");
      String companionDeviceID = "m5atom-matrix:" + deviceID.substring(deviceID.length() - 5);
      client.println("KEY-PRESS DEVICEID=" + companionDeviceID + " KEY=0 PRESSED=true");
    }

    if (M5.Btn.wasReleased()) {
      Serial.println("[BTN] Release -> KEY=0 PRESSED=false");
      String companionDeviceID = "m5atom-matrix:" + deviceID.substring(deviceID.length() - 5);
      client.println("KEY-PRESS DEVICEID=" + companionDeviceID + " KEY=0 PRESSED=false");
    }

    if (now - lastPingTime >= pingIntervalMs) {
      client.println("PING m5atom");
      lastPingTime = now;
    }
  }

  delay(10);
}

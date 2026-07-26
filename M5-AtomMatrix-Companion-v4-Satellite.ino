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
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <vector>
#include <esp32-hal-ledc.h>   // core 3.x LEDC helpers
#include <WebServer.h>
#include <ESPmDNS.h>

Preferences preferences;
WiFiManager wifiManager;
WiFiClient client;

// REST API Server for Companion configuration
WebServer restServer(9999);

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

// Boot counter for config portal trigger
const uint8_t BOOT_FAIL_LIMIT = 1;
int bootCountCached = 0;

// AP password for config portal (empty = open)
const char* AP_password = "";

// Timing
unsigned long lastPingTime    = 0;
unsigned long lastConnectTry  = 0;
const unsigned long connectRetryMs = 5000;
const unsigned long pingIntervalMs  = 2000;

// Brightness (0–100)
int brightness = 100;

// The Atom Matrix is 25 WS2812C LEDs driven from the Atom's small power
// supply.  Keep the colour data itself at 20% as a second, hardware-level
// safeguard: Companion can send full-white (255,255,255), which otherwise
// makes the whole panel uncomfortably bright and hot.
//
// This is deliberately separate from `brightness`, which is the Companion
// control value and also drives the external tally LED.
const uint8_t MATRIX_OUTPUT_SCALE_PERCENT = 20;

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
WiFiManagerParameter* custom_companionIP;
WiFiManagerParameter* custom_companionPort;

// Logger
void logger(const String& s, const String& type = "info") {
  Serial.println(s);
}

// -------------------------------------------------------------------
// Matrix drawing helpers (Tally-Arbiter style)
// -------------------------------------------------------------------
int scaleMatrixColor(int rgb) {
  const uint8_t r = ((rgb >> 16) & 0xFF) * MATRIX_OUTPUT_SCALE_PERCENT / 100;
  const uint8_t g = ((rgb >> 8)  & 0xFF) * MATRIX_OUTPUT_SCALE_PERCENT / 100;
  const uint8_t b = (rgb & 0xFF) * MATRIX_OUTPUT_SCALE_PERCENT / 100;
  return (r << 16) | (g << 8) | b;
}

void matrixDrawPixel(uint8_t pixel, int rgb) {
  M5.dis.drawpix(pixel, scaleMatrixColor(rgb));
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
  if (!tallyActive) matrixFill(RGB_COLOR_BLACK);
  matrixDrawPixel(0, matrixStatusColor());
}

void setMatrixStatus(uint8_t status) {
  matrixStatus = static_cast<MatrixStatus>(status);
  renderMatrixStatus();
}

// -------------------------------------------------------------------
// Config param helpers
// -------------------------------------------------------------------
String getParam(const String& name) {
  if (wifiManager.server && wifiManager.server->hasArg(name))
    return wifiManager.server->arg(name);
  return "";
}

void saveParamCallback() {
  String str_companionIP   = getParam("companionIP");
  String str_companionPort = getParam("companionPort");

  preferences.begin("companion", false);
  if (str_companionIP.length() > 0)   preferences.putString("companionip", str_companionIP);
  if (str_companionPort.length() > 0) preferences.putString("companionport", str_companionPort);
  preferences.end();
}

// ------------------------------------------------------------
// Boot counter management
// ------------------------------------------------------------
int eepromReadBootCounter() {
  preferences.begin("companion", true);
  int count = preferences.getInt("bootCounter", 0);
  preferences.end();
  return count;
}

void eepromWriteBootCounter(int count) {
  preferences.begin("companion", false);
  preferences.putInt("bootCounter", count);
  preferences.end();
}

// ------------------------------------------------------------
// Config portal functions
// ------------------------------------------------------------
void startConfigPortal() {
  Serial.println("[WiFi] Entering CONFIG PORTAL mode");
  matrixStatus = STATUS_CONFIG;
  
  // Load Companion config from preferences (for default field values)
  preferences.begin("companion", true);
  String savedHost = preferences.getString("companionip", "Companion IP");
  String savedPort = preferences.getString("companionport", "16622");
  preferences.end();

  // Prepare WiFiManager with params
  custom_companionIP   = new WiFiManagerParameter("companionIP", "Companion IP", savedHost.c_str(), 40);
  custom_companionPort = new WiFiManagerParameter("companionPort", "Satellite Port", savedPort.c_str(), 6);

  wifiManager.addParameter(custom_companionIP);
  wifiManager.addParameter(custom_companionPort);
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

  // Save to preferences
  preferences.begin("companion", false);
  preferences.putString("companionip", String(companion_host));
  preferences.putString("companionport", String(companion_port));
  preferences.end();

  Serial.println("[WiFi] Config portal completed");
  Serial.printf("[WiFi] Companion Host: %s\n", companion_host);
  Serial.printf("[WiFi] Companion Port: %s\n", companion_port);
}

// -------------------------------------------------------------------
// External LED + Matrix color handling
// -------------------------------------------------------------------
void setExternalLedColor(uint8_t r, uint8_t g, uint8_t b) {
  lastColorR = r;
  lastColorG = g;
  lastColorB = b;

  uint8_t scaledR = r * max(brightness, 15) / 100;
  uint8_t scaledG = g * max(brightness, 15) / 100;
  uint8_t scaledB = b * max(brightness, 15) / 100;

  Serial.print("[COLOR] raw r/g/b = ");
  Serial.print(r); Serial.print("/");
  Serial.print(g); Serial.print("/");
  Serial.print(b);
  Serial.print("  scaled = ");
  Serial.print(scaledR); Serial.print("/");
  Serial.print(scaledG); Serial.print("/");
  Serial.println(scaledB);

  // External RGB LED using new core 3.x API (pin-based)
  writePwm(LED_PIN_RED,   LEDC_CHANNEL_RED,   scaledR);
  writePwm(LED_PIN_GREEN, LEDC_CHANNEL_GREEN, scaledG);
  writePwm(LED_PIN_BLUE,  LEDC_CHANNEL_BLUE,  scaledB);

  // Light the matrix in the tally colour, with one status pixel overlaid.
  int rgb = (scaledR << 16) | (scaledG << 8) | scaledB;
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
        "COLORS=rgb TEXT=false BITMAPS=0";
  client.println(cmd);
  Serial.println("[API] Sent: " + cmd);
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
    String v = apiData.substring(valPos + 6);
    brightness = v.toInt();
    Serial.println("[API] BRIGHTNESS set to " + String(brightness));
    setExternalLedColor(lastColorR, lastColorG, lastColorB);
    return;
  }

  if (apiData.startsWith("KEYS-CLEAR")) {
    Serial.println("[API] KEYS-CLEAR received");
    matrixOff();
    tallyActive = false;
    renderMatrixStatus();
    setExternalLedColor(0,0,0);
    return;
  }

  if (apiData.startsWith("KEY-STATE")) {
    handleKeyState(apiData);
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

void handleGetSettings() {
  restServer.send(200, "application/json", "{\"brightness\":" + String(brightness) + "}");
}

void handlePostSettings() {
  const String value = jsonSetting(restServer.arg("plain"), "brightness");
  if (!value.length() || value.toInt() < 0 || value.toInt() > 100) { restServer.send(400, "text/plain", "brightness must be 0-100"); return; }
  brightness = value.toInt();
  preferences.begin("companion", false); preferences.putInt("brightness", brightness); preferences.end();
  setExternalLedColor(lastColorR, lastColorG, lastColorB);
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
    
    // Update WiFiManager parameter
    if (custom_companionIP) {
      custom_companionIP->setValue(companion_host, sizeof(companion_host));
      Serial.println("[REST] WiFiManager parameter updated");
    }
    
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
    
    // Update WiFiManager parameter
    if (custom_companionPort) {
      custom_companionPort->setValue(companion_port, sizeof(companion_port));
    }
    
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
    
    // Update WiFiManager parameters
    if (custom_companionIP) {
      custom_companionIP->setValue(companion_host, sizeof(companion_host));
    }
    if (custom_companionPort) {
      custom_companionPort->setValue(companion_port, sizeof(companion_port));
    }
    
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
  HTTPUpload& upload = restServer.upload();
  if (upload.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
  else if (upload.status == UPLOAD_FILE_WRITE) Update.write(upload.buf, upload.currentSize);
  else if (upload.status == UPLOAD_FILE_END) Update.end(true);
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

void setupRestServer() {
  restServer.on("/api/host", HTTP_GET, handleGetHost);
  restServer.on("/api/port", HTTP_GET, handleGetPort);
  restServer.on("/api/config", HTTP_GET, handleGetConfig);
  restServer.on("/api/settings", HTTP_GET, handleGetSettings);
  
  restServer.on("/api/host", HTTP_POST, handlePostHost);
  restServer.on("/api/port", HTTP_POST, handlePostPort);
  restServer.on("/api/config", HTTP_POST, handlePostConfig);
  restServer.on("/api/settings", HTTP_POST, handlePostSettings);
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
void connectToNetwork() {
  if (stationIP != IPAddress(0,0,0,0))
    wifiManager.setSTAStaticIPConfig(stationIP, stationGW, stationMask);

  WiFi.mode(WIFI_STA);
  logger("Connecting to SSID: " + String(WiFi.SSID()), "info");

  custom_companionIP   = new WiFiManagerParameter("companionIP", "Companion IP", companion_host, 40);
  custom_companionPort = new WiFiManagerParameter("companionPort", "Satellite Port", companion_port, 6);

  wifiManager.addParameter(custom_companionIP);
  wifiManager.addParameter(custom_companionPort);
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

// -------------------------------------------------------------------
// Companion discovery
// -------------------------------------------------------------------
void initializeMDNS() {
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
}

// -------------------------------------------------------------------
// SETUP
// -------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("Booting M5 Atom Matrix Companion v4…");

  // Make sure WiFi is initialised so MAC is valid
  WiFi.mode(WIFI_STA);
  delay(100);

  // Build deviceID from full MAC
  uint8_t mac[6];
  WiFi.macAddress(mac);

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
  M5.dis.setBrightness(20);  // M5Stack's recommended safe Atom Matrix level
  matrixOff();

  // Boot icon (simple “setup” sequence)
  drawNumberArray(icons[7], infocolor);
  delay(300);
  drawNumberArray(icons[8], infocolor);
  delay(300);
  drawNumberArray(icons[7], infocolor);
  delay(300);
  matrixOff();

  // External LED setup
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

  // WiFi connect (with icons)
  
  // Boot counter logic for config portal trigger
  bootCountCached = eepromReadBootCounter();
  Serial.printf("[Boot] Boot counter read: %u\n", bootCountCached);
  
  if (bootCountCached == 1) {
    // Boot counter 1 → trigger config portal (user reset during boot animations)
    Serial.println("[Boot] Boot counter 1 → triggering config portal");
    eepromWriteBootCounter(0);  // Reset immediately
    bootCountCached = 0;
    startConfigPortal();
    // startConfigPortal() will handle setup icons
    return;  // Skip normal boot sequence
  } else {
    // Boot counter 0 (or any other value) → normal boot
    Serial.println("[Boot] Boot counter 0 → normal boot");
    // Set boot counter to 1 during boot animations so user can reset to trigger portal
    eepromWriteBootCounter(1);
  }
  
  connectToNetwork();

  // OTA
  ArduinoOTA.setHostname(deviceID.c_str());
  ArduinoOTA.setPassword("companion-satellite");
  ArduinoOTA.begin();

  // Start REST API server after WiFi is connected
  setupRestServer();

  initializeMDNS();

  // Show “waiting for Companion” icon (single dot)
  setMatrixStatus(STATUS_WIFI);
  
  // Successful boot completed - set boot counter to 0
  eepromWriteBootCounter(0);
  Serial.println("[Boot] Successful boot completed - boot counter reset to 0");
  
  Serial.println("[System] Setup complete, entering main loop.");
}

// -------------------------------------------------------------------
// LOOP
// -------------------------------------------------------------------
void loop() {
  M5.update();
  ArduinoOTA.handle();
  restServer.handleClient();

  unsigned long now = millis();

  // Companion connect / reconnect
  if (!client.connected() && (now - lastConnectTry >= connectRetryMs)) {
    lastConnectTry = now;
    Serial.print("[NET] Connecting to Companion ");
    Serial.print(companion_host);
    Serial.print(":");
    Serial.println(companion_port);

    if (client.connect(companion_host, atoi(companion_port))) {
      Serial.println("[NET] Connected to Companion API");
      matrixStatus = STATUS_CONNECTED;
      // Good icon when Companion connects
      drawNumberArray(icons[11], readycolor);
      delay(300);
      renderMatrixStatus();
      sendAddDevice();
      lastPingTime = millis();
    } else {
      Serial.println("[NET] Companion connect failed");
      matrixStatus = STATUS_ERROR;
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

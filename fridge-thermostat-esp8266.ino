#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <PubSubClient.h>

// ===================== Pins =====================
#define PIN_SSR        D1
#define PIN_ONEWIRE    D2
#define PIN_LED_STRIP  D5
#define PIN_OLED_SDA   D6
#define PIN_OLED_SCL   D7
#define PIN_STATUSLED  LED_BUILTIN

// ===================== OLED =====================
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_ADDR      0x3C   // try 0x3D if needed

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool displayOk = false;

// ===================== NeoPixel =====================
#define LED_COUNT      8
#define LED_BRIGHTNESS 20

Adafruit_NeoPixel strip(LED_COUNT, PIN_LED_STRIP, NEO_GRB + NEO_KHZ800);

// ===================== AP =====================
const char* AP_SSID = "Fridge-Control";
const char* AP_PASS = "12345678";

ESP8266WebServer server(80);

// ===================== Station Wi-Fi =====================
// Fill with your home Wi-Fi so the device can reach Home Assistant / MQTT broker
const char* STA_SSID = "YOUR_WIFI_NAME";
const char* STA_PASS = "YOUR_WIFI_PASSWORD";

// ===================== MQTT =====================
// Replace with your broker settings
const char* MQTT_HOST = "192.168.0.118";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASSWORD = "";

// Device/topic naming
const char* DEVICE_NAME = "Fridge Vitrine";
const char* DEVICE_ID   = "fridge_vitrine_1";

// Topic layout
const char* TOPIC_STATUS          = "fridge/vitrine1/status";
const char* TOPIC_TEMP_STATE      = "fridge/vitrine1/state/temp";
const char* TOPIC_TEMP_ON_STATE   = "fridge/vitrine1/state/temp_on";
const char* TOPIC_TEMP_OFF_STATE  = "fridge/vitrine1/state/temp_off";
const char* TOPIC_ENABLED_STATE   = "fridge/vitrine1/state/enabled";
const char* TOPIC_COMP_STATE      = "fridge/vitrine1/state/compressor";
const char* TOPIC_SENSORERR_STATE = "fridge/vitrine1/state/sensor_error";
const char* TOPIC_WIFI_STATE      = "fridge/vitrine1/state/wifi";
const char* TOPIC_IP_STATE        = "fridge/vitrine1/state/ip";

const char* TOPIC_TEMP_ON_SET     = "fridge/vitrine1/set/temp_on";
const char* TOPIC_TEMP_OFF_SET    = "fridge/vitrine1/set/temp_off";
const char* TOPIC_ENABLED_SET     = "fridge/vitrine1/set/enabled";
const char* TOPIC_FORCE_OFF_SET   = "fridge/vitrine1/set/force_off";

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// ===================== DS18B20 =====================
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature sensors(&oneWire);

// ===================== Timings =====================
#define MIN_OFF_TIME_MS     60000UL
#define MAX_RUN_TIME_MS     3600000UL
#define ERROR_ON_MS         1200000UL
#define ERROR_OFF_MS        2400000UL

// ===================== EEPROM =====================
#define EEPROM_SIZE 64
#define EEPROM_MAGIC 0x42AA5511

struct Settings {
  uint32_t magic;
  float tempOn;
  float tempOff;
  uint8_t enabled;
};

Settings cfg;

// ===================== State =====================
float currentTemp = -127.0f;
bool sensorError = true;
bool compressorOnState = false;

unsigned long offTime = 0;
unsigned long runTime = 0;
unsigned long nowMs = 0;
unsigned long lastSensorReadMs = 0;
unsigned long lastLogMs = 0;
unsigned long lastMqttStateMs = 0;
unsigned long lastWifiReconnectMs = 0;
unsigned long lastMqttReconnectMs = 0;

bool lastCompPublished = false;
bool lastSensorErrPublished = true;
bool mqttDiscoverySent = false;

// ===================== Forward declarations =====================
void publishMqttState(bool forceAll = false);

// ===================== Helpers =====================
bool stationConfigured() {
  return strlen(STA_SSID) > 0 && String(STA_SSID) != "YOUR_WIFI_NAME";
}

bool mqttConfigured() {
  return strlen(MQTT_HOST) > 0;
}

void setStatusLed(bool on) {
  digitalWrite(PIN_STATUSLED, on ? LOW : HIGH); // inverted on ESP8266
}

void compressorOn() {
  digitalWrite(PIN_SSR, HIGH);   // SSR: HIGH = ON
  compressorOnState = true;
  runTime = nowMs;
  Serial.println("COMPRESSOR: ON");
  publishMqttState(true);
}

void compressorOff() {
  digitalWrite(PIN_SSR, LOW);    // SSR: LOW = OFF
  compressorOnState = false;
  offTime = nowMs;
  Serial.println("COMPRESSOR: OFF");
  publishMqttState(true);
}

bool sensorOk(float t) {
  if (t == DEVICE_DISCONNECTED_C) return false;
  if (t < -40.0f || t > 80.0f) return false;
  return true;
}

float parseFloatArg(const String& argValue, float defaultValue) {
  String v = argValue;
  v.trim();
  v.replace(",", ".");
  float parsed = v.toFloat();

  if (parsed == 0.0f) {
    if (v == "0" || v == "0.0" || v == "0,0") return 0.0f;
    return defaultValue;
  }
  return parsed;
}

unsigned long startDelayRemainingMs() {
  if (compressorOnState) return 0;
  unsigned long offDelta = nowMs - offTime;
  if (offDelta >= MIN_OFF_TIME_MS) return 0;
  return MIN_OFF_TIME_MS - offDelta;
}

String formatSeconds(unsigned long ms) {
  return String(ms / 1000UL);
}

// ===================== EEPROM =====================
void saveSettings() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);

  bool needSave = false;

  if (cfg.magic != EEPROM_MAGIC || isnan(cfg.tempOn) || isnan(cfg.tempOff)) {
    cfg.magic = EEPROM_MAGIC;
    cfg.tempOn = 6.0f;
    cfg.tempOff = 3.0f;
    cfg.enabled = 1;
    needSave = true;
  }

  if (cfg.tempOff >= cfg.tempOn) {
    cfg.tempOn = 6.0f;
    cfg.tempOff = 3.0f;
    cfg.enabled = 1;
    needSave = true;
  }

  if (needSave) saveSettings();
}

// ===================== NeoPixel =====================
uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return strip.Color(r, g, b);
}

uint32_t blendColor(
  uint8_t r1, uint8_t g1, uint8_t b1,
  uint8_t r2, uint8_t g2, uint8_t b2,
  float k
) {
  if (k < 0.0f) k = 0.0f;
  if (k > 1.0f) k = 1.0f;

  uint8_t r = (uint8_t)(r1 + (r2 - r1) * k);
  uint8_t g = (uint8_t)(g1 + (g2 - g1) * k);
  uint8_t b = (uint8_t)(b1 + (b2 - b1) * k);

  return rgb(r, g, b);
}

uint32_t temperatureColor(float t) {
  if (t <= cfg.tempOff) return rgb(0, 0, 255);
  if (t >= cfg.tempOn)  return rgb(255, 0, 0);

  float mid = (cfg.tempOff + cfg.tempOn) / 2.0f;

  if (t < mid) {
    float k = (t - cfg.tempOff) / (mid - cfg.tempOff);
    return blendColor(0, 0, 255, 0, 255, 0, k);
  }

  float k = (t - mid) / (cfg.tempOn - mid);
  return blendColor(0, 255, 0, 255, 0, 0, k);
}

void setAllPixels(uint32_t color) {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

void updateLedStrip() {
  if (!cfg.enabled) {
    setAllPixels(rgb(40, 40, 40));
    return;
  }

  if (sensorError) {
    bool blink = ((nowMs / 500UL) % 2) == 0;
    setAllPixels(blink ? rgb(255, 0, 0) : rgb(0, 0, 0));
    return;
  }

  setAllPixels(temperatureColor(currentTemp));
}

// ===================== OLED =====================
void drawDisplay() {
  if (!displayOk) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(compressorOnState ? "COMP ON" : "COMP OFF");

  display.setCursor(74, 0);
  display.print(WiFi.status() == WL_CONNECTED ? "W" : "-");
  display.print(mqttClient.connected() ? "M" : "-");

  if (sensorError) {
    display.setTextSize(2);
    display.setCursor(10, 20);
    display.println("SENSOR");
    display.setCursor(18, 42);
    display.println("ERROR");
  } else {
    display.setTextSize(4);
    display.setCursor(0, 16);
    display.print(String(currentTemp, 1));
    display.setTextSize(2);
    display.setCursor(104, 24);
    display.print("C");
  }

  display.setTextSize(1);
  display.setCursor(0, 56);

  if (!cfg.enabled) {
    display.print("Cooling disabled");
  } else if (!compressorOnState) {
    unsigned long remain = startDelayRemainingMs();
    if (remain > 0) {
      display.print("Delay ");
      display.print(remain / 1000UL);
      display.print("s");
    } else {
      display.print("Ready ");
      display.print(cfg.tempOff, 1);
      display.print("/");
      display.print(cfg.tempOn, 1);
    }
  } else {
    display.print("ON:");
    display.print(cfg.tempOn, 1);
    display.print(" OFF:");
    display.print(cfg.tempOff, 1);
  }

  display.display();
}

// ===================== MQTT =====================
void mqttPublish(const char* topic, const char* payload, bool retained = true) {
  if (mqttClient.connected()) {
    mqttClient.publish(topic, payload, retained);
  }
}

void mqttPublishFloat(const char* topic, float value, uint8_t decimals = 1, bool retained = true) {
  char buf[24];
  dtostrf(value, 0, decimals, buf);
  mqttPublish(topic, buf, retained);
}

void mqttPublishBoolOnOff(const char* topic, bool value, bool retained = true) {
  mqttPublish(topic, value ? "ON" : "OFF", retained);
}

void sendMqttDiscovery() {
  if (!mqttClient.connected()) return;
  if (mqttDiscoverySent) return;

  char payload[1024];

  snprintf(payload, sizeof(payload),
    "{"
    "\"name\":\"Temperature\","
    "\"uniq_id\":\"%s_temp\","
    "\"stat_t\":\"%s\","
    "\"unit_of_meas\":\"°C\","
    "\"dev_cla\":\"temperature\","
    "\"stat_cla\":\"measurement\","
    "\"avty_t\":\"%s\","
    "\"pl_avail\":\"online\","
    "\"pl_not_avail\":\"offline\","
    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"OpenAI\",\"mdl\":\"Wemos D1 mini Pro fridge\"}"
    "}",
    DEVICE_ID, TOPIC_TEMP_STATE, TOPIC_STATUS, DEVICE_ID, DEVICE_NAME
  );
  mqttPublish("homeassistant/sensor/fridge_vitrine_1/temp/config", payload, true);

  snprintf(payload, sizeof(payload),
    "{"
    "\"name\":\"Cooling\","
    "\"uniq_id\":\"%s_enabled\","
    "\"stat_t\":\"%s\","
    "\"cmd_t\":\"%s\","
    "\"pl_on\":\"ON\","
    "\"pl_off\":\"OFF\","
    "\"avty_t\":\"%s\","
    "\"pl_avail\":\"online\","
    "\"pl_not_avail\":\"offline\","
    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\"}"
    "}",
    DEVICE_ID, TOPIC_ENABLED_STATE, TOPIC_ENABLED_SET, TOPIC_STATUS, DEVICE_ID, DEVICE_NAME
  );
  mqttPublish("homeassistant/switch/fridge_vitrine_1/enabled/config", payload, true);

  snprintf(payload, sizeof(payload),
    "{"
    "\"name\":\"Turn ON\","
    "\"uniq_id\":\"%s_temp_on\","
    "\"stat_t\":\"%s\","
    "\"cmd_t\":\"%s\","
    "\"min\":-10,"
    "\"max\":30,"
    "\"step\":0.5,"
    "\"unit_of_meas\":\"°C\","
    "\"mode\":\"box\","
    "\"avty_t\":\"%s\","
    "\"pl_avail\":\"online\","
    "\"pl_not_avail\":\"offline\","
    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\"}"
    "}",
    DEVICE_ID, TOPIC_TEMP_ON_STATE, TOPIC_TEMP_ON_SET, TOPIC_STATUS, DEVICE_ID, DEVICE_NAME
  );
  mqttPublish("homeassistant/number/fridge_vitrine_1/temp_on/config", payload, true);

  snprintf(payload, sizeof(payload),
    "{"
    "\"name\":\"Turn OFF\","
    "\"uniq_id\":\"%s_temp_off\","
    "\"stat_t\":\"%s\","
    "\"cmd_t\":\"%s\","
    "\"min\":-20,"
    "\"max\":25,"
    "\"step\":0.5,"
    "\"unit_of_meas\":\"°C\","
    "\"mode\":\"box\","
    "\"avty_t\":\"%s\","
    "\"pl_avail\":\"online\","
    "\"pl_not_avail\":\"offline\","
    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\"}"
    "}",
    DEVICE_ID, TOPIC_TEMP_OFF_STATE, TOPIC_TEMP_OFF_SET, TOPIC_STATUS, DEVICE_ID, DEVICE_NAME
  );
  mqttPublish("homeassistant/number/fridge_vitrine_1/temp_off/config", payload, true);

  snprintf(payload, sizeof(payload),
    "{"
    "\"name\":\"Compressor\","
    "\"uniq_id\":\"%s_compressor\","
    "\"stat_t\":\"%s\","
    "\"pl_on\":\"ON\","
    "\"pl_off\":\"OFF\","
    "\"avty_t\":\"%s\","
    "\"pl_avail\":\"online\","
    "\"pl_not_avail\":\"offline\","
    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\"}"
    "}",
    DEVICE_ID, TOPIC_COMP_STATE, TOPIC_STATUS, DEVICE_ID, DEVICE_NAME
  );
  mqttPublish("homeassistant/binary_sensor/fridge_vitrine_1/compressor/config", payload, true);

  snprintf(payload, sizeof(payload),
    "{"
    "\"name\":\"Sensor Error\","
    "\"uniq_id\":\"%s_sensor_error\","
    "\"dev_cla\":\"problem\","
    "\"stat_t\":\"%s\","
    "\"pl_on\":\"ON\","
    "\"pl_off\":\"OFF\","
    "\"avty_t\":\"%s\","
    "\"pl_avail\":\"online\","
    "\"pl_not_avail\":\"offline\","
    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\"}"
    "}",
    DEVICE_ID, TOPIC_SENSORERR_STATE, TOPIC_STATUS, DEVICE_ID, DEVICE_NAME
  );
  mqttPublish("homeassistant/binary_sensor/fridge_vitrine_1/sensor_error/config", payload, true);

  mqttDiscoverySent = true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  String topicStr = String(topic);

  if (topicStr == TOPIC_TEMP_ON_SET) {
    float v = parseFloatArg(msg, cfg.tempOn);
    if (v > cfg.tempOff) {
      cfg.tempOn = v;
      saveSettings();
      publishMqttState(true);
    }
  } else if (topicStr == TOPIC_TEMP_OFF_SET) {
    float v = parseFloatArg(msg, cfg.tempOff);
    if (v < cfg.tempOn) {
      cfg.tempOff = v;
      saveSettings();
      publishMqttState(true);
    }
  } else if (topicStr == TOPIC_ENABLED_SET) {
    if (msg == "ON" || msg == "1" || msg == "on") {
      cfg.enabled = 1;
      saveSettings();
      publishMqttState(true);
    } else if (msg == "OFF" || msg == "0" || msg == "off") {
      cfg.enabled = 0;
      saveSettings();
      compressorOff();
      publishMqttState(true);
    }
  } else if (topicStr == TOPIC_FORCE_OFF_SET) {
    if (msg == "ON" || msg == "1" || msg == "off" || msg == "OFF") {
      compressorOff();
      publishMqttState(true);
    }
  }
}

bool mqttConnect() {
  if (!mqttConfigured()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (mqttClient.connected()) return true;

  char clientId[64];
  snprintf(clientId, sizeof(clientId), "fridge-%06lx", ESP.getChipId());

  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqttClient.connect(clientId, MQTT_USER, MQTT_PASSWORD, TOPIC_STATUS, 1, true, "offline");
  } else {
    ok = mqttClient.connect(clientId, TOPIC_STATUS, 1, true, "offline");
  }

  if (ok) {
    mqttPublish(TOPIC_STATUS, "online", true);
    mqttClient.subscribe(TOPIC_TEMP_ON_SET);
    mqttClient.subscribe(TOPIC_TEMP_OFF_SET);
    mqttClient.subscribe(TOPIC_ENABLED_SET);
    mqttClient.subscribe(TOPIC_FORCE_OFF_SET);
    sendMqttDiscovery();
    publishMqttState(true);
  }

  return ok;
}

void publishMqttState(bool forceAll) {
  if (!mqttClient.connected()) return;

  mqttPublishFloat(TOPIC_TEMP_STATE, currentTemp, 2, true);
  mqttPublishFloat(TOPIC_TEMP_ON_STATE, cfg.tempOn, 1, true);
  mqttPublishFloat(TOPIC_TEMP_OFF_STATE, cfg.tempOff, 1, true);
  mqttPublishBoolOnOff(TOPIC_ENABLED_STATE, cfg.enabled, true);
  mqttPublishBoolOnOff(TOPIC_COMP_STATE, compressorOnState, true);
  mqttPublishBoolOnOff(TOPIC_SENSORERR_STATE, sensorError, true);

  mqttPublish(TOPIC_WIFI_STATE, WiFi.status() == WL_CONNECTED ? "ON" : "OFF", true);

  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    mqttPublish(TOPIC_IP_STATE, ip.c_str(), true);
  }

  lastCompPublished = compressorOnState;
  lastSensorErrPublished = sensorError;
  lastMqttStateMs = nowMs;
}

// ===================== Wi-Fi =====================
void ensureStationConnection() {
  if (!stationConfigured()) return;
  if (WiFi.status() == WL_CONNECTED) return;
  if (nowMs - lastWifiReconnectMs < 10000UL && lastWifiReconnectMs != 0) return;

  lastWifiReconnectMs = nowMs;
  Serial.println("WiFi STA reconnect...");
  WiFi.begin(STA_SSID, STA_PASS);
}

// ===================== HTML =====================
String htmlPage() {
  String s;
  s.reserve(5200);

  unsigned long remainMs = startDelayRemainingMs();

  s += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>Fridge Control</title>";
  s += "<style>";
  s += "body{font-family:Arial;margin:20px;background:#0f1115;color:#e8e8e8;}";
  s += ".card{max-width:520px;background:#1b1f27;padding:20px;border-radius:14px;box-shadow:0 2px 12px rgba(0,0,0,.35);}";
  s += "input,button,select,a.btn{width:100%;padding:12px;margin:8px 0;font-size:16px;border-radius:10px;border:1px solid #444;box-sizing:border-box;}";
  s += "input,select{background:#2a2f39;color:#fff;}";
  s += "button,a.btn{background:#555;color:#fff;text-decoration:none;display:block;text-align:center;}";
  s += ".ok{color:#56d364;font-weight:bold;}.err{color:#ff6b6b;font-weight:bold;}.warn{color:#ffd166;font-weight:bold;}";
  s += "hr{border:none;border-top:1px solid #444;margin:18px 0;}";
  s += ".small{font-size:14px;color:#b8b8b8;}";
  s += "</style></head><body><div class='card'>";

  s += "<h2>Fridge Thermostat</h2>";

  s += "<p><b>AP SSID:</b> ";
  s += AP_SSID;
  s += "</p>";

  s += "<p><b>AP IP:</b> ";
  s += WiFi.softAPIP().toString();
  s += "</p>";

  s += "<p><b>Home Wi-Fi:</b> ";
  s += (WiFi.status() == WL_CONNECTED) ? "<span class='ok'>CONNECTED</span>" : "<span class='err'>DISCONNECTED</span>";
  s += "</p>";

  if (WiFi.status() == WL_CONNECTED) {
    s += "<p><b>LAN IP:</b> ";
    s += WiFi.localIP().toString();
    s += "</p>";
  }

  s += "<p><b>MQTT:</b> ";
  s += mqttClient.connected() ? "<span class='ok'>CONNECTED</span>" : "<span class='err'>DISCONNECTED</span>";
  s += "</p>";

  s += "<p><b>Temperature:</b> ";
  if (sensorError) {
    s += "<span class='err'>Sensor error</span>";
  } else {
    s += String(currentTemp, 2);
    s += " &deg;C";
  }
  s += "</p>";

  s += "<p><b>Compressor:</b> ";
  s += compressorOnState ? "<span class='ok'>ON</span>" : "<span class='err'>OFF</span>";
  s += "</p>";

  s += "<p><b>Cooling enabled:</b> ";
  s += cfg.enabled ? "<span class='ok'>YES</span>" : "<span class='err'>NO</span>";
  s += "</p>";

  s += "<p><b>Turn ON at:</b> ";
  s += String(cfg.tempOn, 1);
  s += " &deg;C</p>";

  s += "<p><b>Turn OFF at:</b> ";
  s += String(cfg.tempOff, 1);
  s += " &deg;C</p>";

  if (!compressorOnState) {
    if (remainMs > 0) {
      s += "<p><b>Start delay:</b> <span class='warn'>";
      s += formatSeconds(remainMs);
      s += " sec remaining</span></p>";
    } else {
      s += "<p><b>Start delay:</b> <span class='ok'>Ready</span></p>";
    }
  }

  s += "<p class='small'>Page does not auto-refresh. Tap Refresh to update values.</p>";
  s += "<a class='btn' href='/'>Refresh</a>";

  s += "<hr>";
  s += "<form action='/set' method='get'>";

  s += "<label>Turn ON at temperature (&deg;C)</label>";
  s += "<input type='text' inputmode='decimal' name='ton' value='";
  s += String(cfg.tempOn, 1);
  s += "'>";

  s += "<label>Turn OFF at temperature (&deg;C)</label>";
  s += "<input type='text' inputmode='decimal' name='toff' value='";
  s += String(cfg.tempOff, 1);
  s += "'>";

  s += "<label>Cooling enabled</label>";
  s += "<select name='ena'>";
  s += cfg.enabled
       ? "<option value='1' selected>Enabled</option><option value='0'>Disabled</option>"
       : "<option value='1'>Enabled</option><option value='0' selected>Disabled</option>";
  s += "</select>";

  s += "<button type='submit'>Save</button>";
  s += "</form>";

  s += "<form action='/forceoff' method='get'><button type='submit'>Force OFF now</button></form>";
  s += "</div></body></html>";
  return s;
}

// ===================== HTTP =====================
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", htmlPage());
}

void handleSet() {
  float newTempOn = cfg.tempOn;
  float newTempOff = cfg.tempOff;
  uint8_t newEnabled = cfg.enabled;

  if (server.hasArg("ton")) newTempOn = parseFloatArg(server.arg("ton"), cfg.tempOn);
  if (server.hasArg("toff")) newTempOff = parseFloatArg(server.arg("toff"), cfg.tempOff);
  if (server.hasArg("ena")) newEnabled = (server.arg("ena") == "1") ? 1 : 0;

  if (newTempOff >= newTempOn) {
    server.send(400, "text/plain; charset=utf-8", "Error: OFF temperature must be lower than ON temperature");
    return;
  }

  cfg.tempOn = newTempOn;
  cfg.tempOff = newTempOff;
  cfg.enabled = newEnabled;

  if (!cfg.enabled) compressorOff();

  saveSettings();
  publishMqttState(true);

  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleForceOff() {
  compressorOff();
  publishMqttState(true);
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_SSR, OUTPUT);
  pinMode(PIN_STATUSLED, OUTPUT);

  digitalWrite(PIN_SSR, LOW);
  setStatusLed(false);

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();

  loadSettings();

  sensors.begin();
  sensors.setResolution(10);
  sensors.setWaitForConversion(true);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (displayOk) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(8, 10);
    display.println("Fridge");
    display.println("Start...");
    display.display();
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  if (stationConfigured()) {
    WiFi.begin(STA_SSID, STA_PASS);
  }

  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/forceoff", handleForceOff);
  server.begin();

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  nowMs = millis();
  offTime = nowMs;

  updateLedStrip();
  drawDisplay();

  Serial.println();
  Serial.println("Fridge thermostat started");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Temp ON: ");
  Serial.println(cfg.tempOn);
  Serial.print("Temp OFF: ");
  Serial.println(cfg.tempOff);
}

// ===================== Loop =====================
void loop() {
  nowMs = millis();
  server.handleClient();

  ensureStationConnection();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (nowMs - lastMqttReconnectMs >= 5000UL || lastMqttReconnectMs == 0) {
        lastMqttReconnectMs = nowMs;
        mqttConnect();
      }
    } else {
      mqttClient.loop();
    }
  }

  if (nowMs - lastSensorReadMs >= 1000UL || lastSensorReadMs == 0) {
    lastSensorReadMs = nowMs;
    sensors.requestTemperatures();
    currentTemp = sensors.getTempCByIndex(0);
    sensorError = !sensorOk(currentTemp);
  }

  if (!cfg.enabled) {
    if (compressorOnState) compressorOff();
    setStatusLed(false);
    updateLedStrip();
    drawDisplay();

    if (mqttClient.connected() && (nowMs - lastMqttStateMs >= 5000UL)) {
      publishMqttState(false);
    }

    if (nowMs - lastLogMs >= 2000UL) {
      lastLogMs = nowMs;
      Serial.print("Temp: ");
      Serial.print(currentTemp);
      Serial.print(" | Cooling disabled | WiFi: ");
      Serial.print(WiFi.status() == WL_CONNECTED ? "ON" : "OFF");
      Serial.print(" | MQTT: ");
      Serial.println(mqttClient.connected() ? "ON" : "OFF");
    }

    delay(100);
    return;
  }

  if (!sensorError) {
    setStatusLed(false);

    if (!compressorOnState) {
      unsigned long offDelta = nowMs - offTime;
      if (offDelta >= MIN_OFF_TIME_MS && currentTemp >= cfg.tempOn) {
        compressorOn();
      }
    } else {
      unsigned long runDelta = nowMs - runTime;
      if (currentTemp <= cfg.tempOff || runDelta >= MAX_RUN_TIME_MS) {
        compressorOff();
      }
    }
  } else {
    setStatusLed(true);

    if (!compressorOnState) {
      unsigned long offDelta = nowMs - offTime;
      if (offDelta >= ERROR_OFF_MS) {
        compressorOn();
      }
    } else {
      unsigned long runDelta = nowMs - runTime;
      if (runDelta >= ERROR_ON_MS) {
        compressorOff();
      }
    }
  }

  updateLedStrip();
  drawDisplay();

  if (mqttClient.connected()) {
    if ((nowMs - lastMqttStateMs >= 5000UL) ||
        (lastCompPublished != compressorOnState) ||
        (lastSensorErrPublished != sensorError)) {
      publishMqttState(false);
    }
  }

  if (nowMs - lastLogMs >= 2000UL) {
    lastLogMs = nowMs;
    Serial.print("Temp: ");
    Serial.print(currentTemp);
    Serial.print(" | Compressor: ");
    Serial.print(compressorOnState ? "ON" : "OFF");
    Serial.print(" | WiFi: ");
    Serial.print(WiFi.status() == WL_CONNECTED ? "ON" : "OFF");
    Serial.print(" | MQTT: ");
    Serial.print(mqttClient.connected() ? "ON" : "OFF");
    Serial.print(" | Delay left: ");
    Serial.print(startDelayRemainingMs() / 1000UL);
    if (sensorError) Serial.print(" | SENSOR ERROR");
    Serial.println(" sec");
  }

  delay(100);
}
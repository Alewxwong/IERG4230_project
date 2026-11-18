#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ===== EDIT THESE THREE VALUES =====
const char* WIFI_SSID = "LEON2995";
const char* WIFI_PASSWORD = "21yA552(";
const char* SERVER_BASE = "http://192.168.1.21:5000";

const char* DEVICE_ID = "TRUCK_01";
const int SDA_PIN = 21;
const int SCL_PIN = 22;

const uint8_t LIS3DH_ADDR = 0x19;
const uint8_t AHT10_ADDR = 0x38;
const uint8_t OLED_ADDR = 0x3C;

const uint8_t LIS_WHO_AM_I = 0x0F;
const uint8_t LIS_CTRL_REG1 = 0x20;
const uint8_t LIS_CTRL_REG4 = 0x23;
const uint8_t LIS_OUT_X_L = 0x28;

Adafruit_SSD1306 display(128, 64, &Wire, -1);

String shipmentCode = "";
String stageName = "IDLE";
bool journeyActive = false;
bool oledOK = false;
bool wifiOK = false;

float minTemperature = 20.0;
float maxTemperature = 27.0;
float moderateImpactThreshold = 0.35;
float severeImpactThreshold = 0.80;
int allowedExcursionSeconds = 20;

float temperatureC = 0.0;
float humidityPct = 0.0;
float magnitudeG = 1.0;
float previousMagnitudeG = 1.0;
float rawImpactG = 0.0;
float peakImpactG = 0.0;
String lastClassification = "NORMAL";

unsigned long lastAccelRead = 0;
unsigned long lastClimateRead = 0;
unsigned long lastOLEDUpdate = 0;
unsigned long lastSummaryUpload = 0;
unsigned long lastConfigFetch = 0;
unsigned long lastImpactEvent = 0;
unsigned long lastWiFiAttempt = 0;

const unsigned long ACCEL_INTERVAL_MS = 10;       // 100 Hz
const unsigned long CLIMATE_INTERVAL_MS = 2000;
const unsigned long OLED_INTERVAL_MS = 500;
const unsigned long SUMMARY_INTERVAL_MS = 10000;
const unsigned long CONFIG_INTERVAL_MS = 5000;
const unsigned long IMPACT_COOLDOWN_MS = 1500;

bool writeRegister(uint8_t device, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(device);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readRegister(uint8_t device, uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(device);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)device, 1) != 1) return false;
  value = Wire.read();
  return true;
}

bool readRegisters(uint8_t device, uint8_t firstReg, uint8_t* data, uint8_t length) {
  Wire.beginTransmission(device);
  Wire.write(firstReg | 0x80);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)device, (int)length) != length) return false;
  for (uint8_t i = 0; i < length; i++) data[i] = Wire.read();
  return true;
}

bool initLIS3DH() {
  uint8_t id = 0;
  if (!readRegister(LIS3DH_ADDR, LIS_WHO_AM_I, id) || id != 0x33) return false;
  bool a = writeRegister(LIS3DH_ADDR, LIS_CTRL_REG1, 0x57); // 100 Hz, XYZ
  bool b = writeRegister(LIS3DH_ADDR, LIS_CTRL_REG4, 0x88); // BDU, +/-2g, HR
  delay(100);
  return a && b;
}

bool readLIS3DH(float &xG, float &yG, float &zG) {
  uint8_t data[6];
  if (!readRegisters(LIS3DH_ADDR, LIS_OUT_X_L, data, 6)) return false;
  int16_t regX = (int16_t)(((uint16_t)data[1] << 8) | data[0]);
  int16_t regY = (int16_t)(((uint16_t)data[3] << 8) | data[2]);
  int16_t regZ = (int16_t)(((uint16_t)data[5] << 8) | data[4]);
  int16_t rawX = regX / 16;
  int16_t rawY = regY / 16;
  int16_t rawZ = regZ / 16;
  xG = rawX * 0.001f;
  yG = rawY * 0.001f;
  zG = rawZ * 0.001f;
  return true;
}

bool readAHT10(float &temp, float &humidity) {
  Wire.beginTransmission(AHT10_ADDR);
  Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(80);
  if (Wire.requestFrom((int)AHT10_ADDR, 6) != 6) return false;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  if (d[0] & 0x80) return false;
  uint32_t rawH = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t rawT = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  humidity = rawH * 100.0f / 1048576.0f;
  temp = rawT * 200.0f / 1048576.0f - 50.0f;
  return temp >= -50 && temp <= 100 && humidity >= 0 && humidity <= 100;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) { wifiOK = true; return; }
  if (millis() - lastWiFiAttempt < 5000) return;
  lastWiFiAttempt = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) delay(250);
  wifiOK = WiFi.status() == WL_CONNECTED;
}

bool httpPost(const String &path, const String &json) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, String(SERVER_BASE) + path)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  http.end();
  return code >= 200 && code < 300;
}

void uploadEvent(const String &eventType, float impact) {
  if (!journeyActive || shipmentCode.length() == 0) return;
  StaticJsonDocument<384> doc;
  doc["shipment_code"] = shipmentCode;
  doc["device_id"] = DEVICE_ID;
  doc["kind"] = "event";
  doc["event_type"] = eventType;
  doc["impact_g"] = impact;
  String body; serializeJson(doc, body);
  httpPost("/api/telemetry", body);
}

void uploadSummary() {
  if (!journeyActive || shipmentCode.length() == 0) return;
  StaticJsonDocument<512> doc;
  doc["shipment_code"] = shipmentCode;
  doc["device_id"] = DEVICE_ID;
  doc["kind"] = "summary";
  doc["temperature"] = temperatureC;
  doc["humidity"] = humidityPct;
  doc["magnitude_g"] = magnitudeG;
  doc["peak_impact_g"] = peakImpactG;
  doc["classification"] = lastClassification;
  String body; serializeJson(doc, body);
  if (httpPost("/api/telemetry", body)) peakImpactG = 0.0;
}

void fetchConfig() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, String(SERVER_BASE) + "/api/config/" + DEVICE_ID)) return;
  int code = http.GET();
  if (code == 200) {
    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err) {
      journeyActive = doc["journey_active"] | false;
      if (journeyActive) {
        shipmentCode = doc["shipment_code"].as<String>();
        stageName = doc["stage"].as<String>();
        minTemperature = doc["minimum_temperature"] | minTemperature;
        maxTemperature = doc["maximum_temperature"] | maxTemperature;
        allowedExcursionSeconds = doc["allowed_excursion_seconds"] | allowedExcursionSeconds;
        moderateImpactThreshold = doc["moderate_impact_threshold"] | moderateImpactThreshold;
        severeImpactThreshold = doc["severe_impact_threshold"] | severeImpactThreshold;
      } else {
        shipmentCode = "";
        stageName = "IDLE";
      }
    }
  }
  http.end();
}

void drawOLED() {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(shipmentCode.length() ? shipmentCode : "NO SHIPMENT");
  display.setCursor(72, 0);
  display.print(wifiOK ? "WiFi OK" : "WiFi --");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 15);
  display.print(temperatureC, 1); display.print("C");
  display.setTextSize(1);
  display.setCursor(80, 16);
  display.print(humidityPct, 0); display.print("%RH");
  display.setCursor(0, 38);
  display.print("Stage: "); display.print(stageName);
  display.setCursor(0, 49);
  display.print("Impact: "); display.print(lastClassification);
  display.setCursor(0, 59);
  display.print("Peak "); display.print(peakImpactG, 2); display.print("g");
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  bool motionOK = initLIS3DH();
  oledOK = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOK) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Cold-chain node");
    display.println(motionOK ? "LIS3DH: OK" : "LIS3DH: FAIL");
    display.println("Connecting WiFi...");
    display.display();
  }
  connectWiFi();
  Serial.println("ESP32 cold-chain firmware started");
}

void loop() {
  unsigned long now = millis();
  connectWiFi();

  if (now - lastAccelRead >= ACCEL_INTERVAL_MS) {
    lastAccelRead = now;
    float x, y, z;
    if (readLIS3DH(x, y, z)) {
      magnitudeG = sqrtf(x*x + y*y + z*z);
      rawImpactG = fabsf(magnitudeG - previousMagnitudeG);
      previousMagnitudeG = magnitudeG;
      if (rawImpactG > peakImpactG) peakImpactG = rawImpactG;
      String detected = "NORMAL";
      if (rawImpactG >= severeImpactThreshold) detected = "SEVERE";
      else if (rawImpactG >= moderateImpactThreshold) detected = "MODERATE";
      if (detected != "NORMAL" && now - lastImpactEvent >= IMPACT_COOLDOWN_MS) {
        lastImpactEvent = now;
        lastClassification = detected;
        uploadEvent(detected == "SEVERE" ? "SEVERE_IMPACT" : "MODERATE_IMPACT", rawImpactG);
      } else if (now - lastImpactEvent > 2500) {
        lastClassification = "NORMAL";
      }
    }
  }

  if (now - lastClimateRead >= CLIMATE_INTERVAL_MS) {
    lastClimateRead = now;
    readAHT10(temperatureC, humidityPct);
  }
  if (now - lastOLEDUpdate >= OLED_INTERVAL_MS) {
    lastOLEDUpdate = now;
    wifiOK = WiFi.status() == WL_CONNECTED;
    drawOLED();
  }
  if (now - lastConfigFetch >= CONFIG_INTERVAL_MS) {
    lastConfigFetch = now;
    fetchConfig();
  }
  if (now - lastSummaryUpload >= SUMMARY_INTERVAL_MS) {
    lastSummaryUpload = now;
    uploadSummary();
  }
}

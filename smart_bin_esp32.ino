/*
 * Smart Bin - ESP32
 * -----------------
 * Reads two HC-SR04 ultrasonic sensors, converts the distance to a
 * fill percentage, and POSTs a JSON message to a configurable URL.
 *
 * JSON payload:
 * {
 *   "bin_id":     "BIN-001",
 *   "timestamp":  "2026-07-15T14:03:22Z",   // UTC, ISO-8601
 *   "fill_pct":   72.5,                       // averaged fill %
 *   "sensors": [                              // optional detail
 *     { "id": 1, "distance_cm": 11.2, "fill_pct": 71.0 },
 *     { "id": 2, "distance_cm": 10.4, "fill_pct": 74.0 }
 *   ]
 * }
 *
 * WiFi + device settings are entered through a captive portal
 * (WiFiManager) on first boot and stored in NVS flash, so they
 * survive normal firmware re-flashes.
 *
 * Libraries required (install via Arduino Library Manager):
 *   - WiFiManager           by tzapu
 *   - ArduinoJson           by Benoit Blanchon
 * (WiFi, HTTPClient, WiFiClientSecure, Preferences ship with the ESP32 core.)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>        // https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>        // https://arduinojson.org
#include <Preferences.h>
#include <time.h>

// ------------------------------------------------------------------
// PIN CONFIGURATION  (change to match your wiring)
// ------------------------------------------------------------------
// Each HC-SR04 needs a TRIG (output) and ECHO (input) pin.
// NOTE: HC-SR04 ECHO puts out 5 V. Use a voltage divider
// (1k / 2k) or a level shifter on each ECHO line to protect the
// ESP32's 3.3 V GPIOs.
const int SENSOR1_TRIG_PIN = 5;
const int SENSOR1_ECHO_PIN = 18;
const int SENSOR2_TRIG_PIN = 17;
const int SENSOR2_ECHO_PIN = 16;

// Pin held LOW during boot forces the config portal to re-open
// (wire a button from this pin to GND). Set to -1 to disable.
const int CONFIG_BUTTON_PIN = 0;   // GPIO0 = the onboard BOOT button

// On-board LED for status feedback (2 on most dev boards, -1 to disable)
const int STATUS_LED_PIN = 2;

// ------------------------------------------------------------------
// BIN GEOMETRY  (calibrate these for your physical bin)
// ------------------------------------------------------------------
// Distance the sensor reads when the bin is EMPTY  (sensor -> bottom).
// Distance the sensor reads when the bin is FULL   (sensor -> top of trash).
// fill% = (EMPTY - measured) / (EMPTY - FULL) * 100, clamped to 0..100.
float BIN_EMPTY_DISTANCE_CM = 40.0;   // e.g. lid-mounted sensor 40 cm above floor
float BIN_FULL_DISTANCE_CM  = 5.0;    // "full" when trash is 5 cm below the sensor

// ------------------------------------------------------------------
// TIMING
// ------------------------------------------------------------------
const unsigned long REPORT_INTERVAL_MS = 60UL * 1000UL;   // send every 60 s
const unsigned long SENSOR_TIMEOUT_US  = 30000UL;         // ~5 m max echo wait
const int           SAMPLES_PER_READ   = 5;               // median-ish averaging

// NTP servers for timestamps (UTC)
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.nist.gov";

// ------------------------------------------------------------------
// GLOBALS
// ------------------------------------------------------------------
Preferences prefs;

// User-configurable settings (defaults; overwritten from NVS / portal)
String binId    = "BIN-001";
String serverUrl = "";   // e.g. https://example.com/api/bins/report

unsigned long lastReportMs = 0;

// ------------------------------------------------------------------
// SETUP
// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Smart Bin ESP32 booting ===");

  if (STATUS_LED_PIN >= 0) pinMode(STATUS_LED_PIN, OUTPUT);

  // Ultrasonic pins
  pinMode(SENSOR1_TRIG_PIN, OUTPUT);
  pinMode(SENSOR1_ECHO_PIN, INPUT);
  pinMode(SENSOR2_TRIG_PIN, OUTPUT);
  pinMode(SENSOR2_ECHO_PIN, INPUT);
  digitalWrite(SENSOR1_TRIG_PIN, LOW);
  digitalWrite(SENSOR2_TRIG_PIN, LOW);

  if (CONFIG_BUTTON_PIN >= 0) pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

  // Load saved device settings from NVS
  loadSettings();

  // Bring up WiFi (captive portal on first boot / when creds missing)
  setupWiFi();

  // Time sync for timestamps
  configTime(0, 0, NTP_SERVER1, NTP_SERVER2);   // 0,0 = UTC, no DST offset
  waitForTime();

  Serial.printf("Bin ID    : %s\n", binId.c_str());
  Serial.printf("Server URL: %s\n", serverUrl.c_str());
  Serial.println("Setup complete.\n");
}

// ------------------------------------------------------------------
// MAIN LOOP
// ------------------------------------------------------------------
void loop() {
  // Allow forcing the config portal by holding the BOOT button
  if (CONFIG_BUTTON_PIN >= 0 && digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    Serial.println("Config button held -> opening portal...");
    delay(50);
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
      openConfigPortal();
    }
  }

  unsigned long now = millis();
  if (now - lastReportMs >= REPORT_INTERVAL_MS || lastReportMs == 0) {
    lastReportMs = now;
    takeReadingAndReport();
  }

  delay(50);
}

// ==================================================================
// SETTINGS (NVS)
// ==================================================================
void loadSettings() {
  prefs.begin("smartbin", true);   // read-only
  binId     = prefs.getString("bin_id", binId);
  serverUrl = prefs.getString("server_url", serverUrl);
  BIN_EMPTY_DISTANCE_CM = prefs.getFloat("empty_cm", BIN_EMPTY_DISTANCE_CM);
  BIN_FULL_DISTANCE_CM  = prefs.getFloat("full_cm",  BIN_FULL_DISTANCE_CM);
  prefs.end();
}

void saveSettings() {
  prefs.begin("smartbin", false);  // read-write
  prefs.putString("bin_id", binId);
  prefs.putString("server_url", serverUrl);
  prefs.putFloat("empty_cm", BIN_EMPTY_DISTANCE_CM);
  prefs.putFloat("full_cm",  BIN_FULL_DISTANCE_CM);
  prefs.end();
  Serial.println("Settings saved to NVS.");
}

// ==================================================================
// WIFI + CAPTIVE PORTAL
// ==================================================================
void setupWiFi() {
  WiFiManager wm;

  // Custom fields shown in the captive portal, pre-filled with saved values
  WiFiManagerParameter pBinId("bin_id", "Bin ID", binId.c_str(), 40);
  WiFiManagerParameter pUrl("server_url", "Server URL (https://...)", serverUrl.c_str(), 200);
  char emptyBuf[16]; dtostrf(BIN_EMPTY_DISTANCE_CM, 0, 1, emptyBuf);
  char fullBuf[16];  dtostrf(BIN_FULL_DISTANCE_CM,  0, 1, fullBuf);
  WiFiManagerParameter pEmpty("empty_cm", "Empty distance (cm)", emptyBuf, 8);
  WiFiManagerParameter pFull("full_cm",  "Full distance (cm)",  fullBuf, 8);

  wm.addParameter(&pBinId);
  wm.addParameter(&pUrl);
  wm.addParameter(&pEmpty);
  wm.addParameter(&pFull);

  wm.setConfigPortalTimeout(180);   // seconds before giving up and retrying

  // Tries saved creds first; if none/failed, opens AP "SmartBin-Setup"
  bool connected = wm.autoConnect("SmartBin-Setup", "binsetup123");

  if (wm.getWiFiIsSaved()) {
    // Portal ran and user may have edited the custom fields -> persist them
    applyPortalParams(pBinId, pUrl, pEmpty, pFull);
  }

  if (!connected) {
    Serial.println("WiFi failed; restarting in 5 s...");
    delay(5000);
    ESP.restart();
  }

  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
  if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, HIGH);
}

// On-demand portal (triggered by holding the config button)
void openConfigPortal() {
  WiFiManager wm;

  WiFiManagerParameter pBinId("bin_id", "Bin ID", binId.c_str(), 40);
  WiFiManagerParameter pUrl("server_url", "Server URL (https://...)", serverUrl.c_str(), 200);
  char emptyBuf[16]; dtostrf(BIN_EMPTY_DISTANCE_CM, 0, 1, emptyBuf);
  char fullBuf[16];  dtostrf(BIN_FULL_DISTANCE_CM,  0, 1, fullBuf);
  WiFiManagerParameter pEmpty("empty_cm", "Empty distance (cm)", emptyBuf, 8);
  WiFiManagerParameter pFull("full_cm",  "Full distance (cm)",  fullBuf, 8);

  wm.addParameter(&pBinId);
  wm.addParameter(&pUrl);
  wm.addParameter(&pEmpty);
  wm.addParameter(&pFull);

  wm.setConfigPortalTimeout(180);
  if (wm.startConfigPortal("SmartBin-Setup", "binsetup123")) {
    applyPortalParams(pBinId, pUrl, pEmpty, pFull);
  }
  Serial.println("Portal closed; restarting...");
  delay(1000);
  ESP.restart();
}

void applyPortalParams(WiFiManagerParameter& pBinId,
                       WiFiManagerParameter& pUrl,
                       WiFiManagerParameter& pEmpty,
                       WiFiManagerParameter& pFull) {
  binId     = String(pBinId.getValue());
  serverUrl = String(pUrl.getValue());
  BIN_EMPTY_DISTANCE_CM = atof(pEmpty.getValue());
  BIN_FULL_DISTANCE_CM  = atof(pFull.getValue());
  saveSettings();
}

// ==================================================================
// TIME
// ==================================================================
void waitForTime() {
  Serial.print("Syncing time");
  struct tm timeinfo;
  int tries = 0;
  while (!getLocalTime(&timeinfo) && tries < 20) {
    Serial.print(".");
    delay(500);
    tries++;
  }
  Serial.println();
  if (tries >= 20) {
    Serial.println("WARNING: NTP sync failed; timestamps may be wrong.");
  }
}

String getIso8601Timestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01T00:00:00Z";
  }
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

// ==================================================================
// ULTRASONIC READING
// ==================================================================
// Returns distance in cm, or -1.0 on timeout / no echo.
float readUltrasonicOnce(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, SENSOR_TIMEOUT_US);
  if (duration == 0) return -1.0;          // timeout
  return (duration * 0.0343f) / 2.0f;      // speed of sound ~343 m/s
}

// Averages several valid samples for stability.
float readUltrasonic(int trigPin, int echoPin) {
  float sum = 0;
  int valid = 0;
  for (int i = 0; i < SAMPLES_PER_READ; i++) {
    float d = readUltrasonicOnce(trigPin, echoPin);
    if (d > 0) { sum += d; valid++; }
    delay(60);   // >60 ms between pings avoids echo overlap
  }
  if (valid == 0) return -1.0;
  return sum / valid;
}

// Convert a measured distance to a 0..100 fill percentage.
float distanceToFillPct(float distanceCm) {
  if (distanceCm < 0) return -1.0;   // invalid
  float span = BIN_EMPTY_DISTANCE_CM - BIN_FULL_DISTANCE_CM;
  if (span <= 0) return -1.0;        // misconfigured geometry
  float pct = (BIN_EMPTY_DISTANCE_CM - distanceCm) / span * 100.0f;
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// ==================================================================
// READ + REPORT
// ==================================================================
void takeReadingAndReport() {
  float d1 = readUltrasonic(SENSOR1_TRIG_PIN, SENSOR1_ECHO_PIN);
  float d2 = readUltrasonic(SENSOR2_TRIG_PIN, SENSOR2_ECHO_PIN);

  float f1 = distanceToFillPct(d1);
  float f2 = distanceToFillPct(d2);

  // Average only the sensors that returned a valid reading
  float fillSum = 0; int fillCount = 0;
  if (f1 >= 0) { fillSum += f1; fillCount++; }
  if (f2 >= 0) { fillSum += f2; fillCount++; }

  if (fillCount == 0) {
    Serial.println("Both sensors failed to read; skipping this cycle.");
    return;
  }
  float fillAvg = fillSum / fillCount;

  Serial.printf("S1: %.1f cm (%.1f%%)  S2: %.1f cm (%.1f%%)  ->  avg %.1f%%\n",
                d1, f1, d2, f2, fillAvg);

  String payload = buildJson(fillAvg, d1, f1, d2, f2);
  sendReport(payload);
}

String buildJson(float fillAvg, float d1, float f1, float d2, float f2) {
  StaticJsonDocument<384> doc;
  doc["bin_id"]    = binId;
  doc["timestamp"] = getIso8601Timestamp();
  doc["fill_pct"]  = roundf(fillAvg * 10) / 10.0;   // 1 decimal place

  JsonArray sensors = doc.createNestedArray("sensors");
  JsonObject s1 = sensors.createNestedObject();
  s1["id"] = 1;
  if (d1 >= 0) { s1["distance_cm"] = roundf(d1 * 10) / 10.0; s1["fill_pct"] = roundf(f1 * 10) / 10.0; }
  else         { s1["distance_cm"] = nullptr;                s1["fill_pct"] = nullptr; }
  JsonObject s2 = sensors.createNestedObject();
  s2["id"] = 2;
  if (d2 >= 0) { s2["distance_cm"] = roundf(d2 * 10) / 10.0; s2["fill_pct"] = roundf(f2 * 10) / 10.0; }
  else         { s2["distance_cm"] = nullptr;                s2["fill_pct"] = nullptr; }

  String out;
  serializeJson(doc, out);
  return out;
}

void sendReport(const String& payload) {
  if (serverUrl.length() == 0) {
    Serial.println("No server URL set. Payload would be:");
    Serial.println(payload);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi down; attempting reconnect...");
    WiFi.reconnect();
    delay(3000);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Still offline; dropping this report.");
      return;
    }
  }

  HTTPClient http;
  bool isHttps = serverUrl.startsWith("https://");

  if (isHttps) {
    WiFiClientSecure client;
    // Prototype: skip certificate validation. For production, pin the
    // server's root CA with client.setCACert(rootCaPem).
    client.setInsecure();
    http.begin(client, serverUrl);
    postJson(http, payload);
  } else {
    http.begin(serverUrl);
    postJson(http, payload);
  }
}

void postJson(HTTPClient& http, const String& payload) {
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  if (code > 0) {
    Serial.printf("HTTP %d  <- %s\n", code, payload.c_str());
    if (code >= 200 && code < 300) blinkOk();
  } else {
    Serial.printf("POST failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

void blinkOk() {
  if (STATUS_LED_PIN < 0) return;
  for (int i = 0; i < 2; i++) {
    digitalWrite(STATUS_LED_PIN, LOW);  delay(80);
    digitalWrite(STATUS_LED_PIN, HIGH); delay(80);
  }
}

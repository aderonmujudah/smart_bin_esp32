/*
 * Smart Bin - ESP32
 * -----------------
 * Reads two HC-SR04 ultrasonic sensors, converts each distance to a
 * fill percentage (each sensor has its own empty/full calibration,
 * since they can be mounted on bins of different sizes/shapes), and
 * POSTs a JSON message to a configurable URL. The JSON is also always
 * printed to the Serial Monitor at 115200 baud.
 *
 * JSON payload (each bin reported independently -- no combined average):
 * {
 *   "timestamp": "2026-07-15T14:03:22+01:00",   // WAT (UTC+1), ISO-8601
 *   "bins": [
 *     { "bin_id": "BIN-001", "distance_mm": 112.0, "fill_pct": 71.0 },
 *     { "bin_id": "BIN-002", "distance_mm": 104.0, "fill_pct": 74.0 }
 *   ]
 * }
 *
 * WiFi credentials are entered by the user through a captive portal
 * (WiFiManager) on first boot: the ESP32 opens a WiFi access point
 * named "SmartBin-Setup", the user connects to it with a phone/laptop
 * and picks their home/office WiFi network + password from a web page.
 * Those credentials are stored by the ESP32 core's own WiFi driver in
 * NVS (Non-Volatile Storage), a dedicated flash partition that is NOT
 * touched by a normal "Upload" of new firmware (only "Erase Flash" or
 * changing the partition table wipes it), so WiFi survives re-flashing.
 * Device settings (bin IDs, server URL, calibration distances) are
 * likewise saved to NVS via the Preferences library (see loadSettings()
 * / saveSettings() below).
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

// ==================================================================
// ====================  EDIT THIS SECTION  ========================
// ==================================================================
// Everything you're likely to need to change lives here: WiFi,
// pin wiring, and per-bin calibration distances.

// ---- WiFi ---------------------------------------------------------
// Fill these in with your home/office network to auto-connect on
// every boot with no captive portal needed. Leave both as "" to skip
// straight to the "SmartBin-Setup" portal instead (see README).
// NOTE: whichever network gets used (hardcoded here, or entered later
// in the portal) is saved by the ESP32's WiFi driver into NVS flash,
// a partition normal firmware uploads do NOT erase, so it survives
// re-flashing.
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

// ---- Pin wiring -----------------------------------------------------
// Each HC-SR04 needs a TRIG (output) and ECHO (input) pin.
// NOTE: HC-SR04 ECHO puts out 5 V. Use a voltage divider
// (1k / 2k) or a level shifter on each ECHO line to protect the
// ESP32's 3.3 V GPIOs.
const int SENSOR1_TRIG_PIN = 17;
const int SENSOR1_ECHO_PIN = 5;
const int SENSOR2_TRIG_PIN = 26;
const int SENSOR2_ECHO_PIN = 25;

// Pin held LOW during boot forces the config portal to re-open
// (wire a button from this pin to GND). Set to -1 to disable.
const int CONFIG_BUTTON_PIN = 0;   // GPIO0 = the onboard BOOT button

// On-board LED for status feedback (2 on most dev boards, -1 to disable)
const int STATUS_LED_PIN = 2;

// ---- Bin geometry (calibrate independently per bin/sensor) ---------
// Distance the sensor reads when its bin is EMPTY  (sensor -> bottom).
// Distance the sensor reads when its bin is FULL   (sensor -> top of trash).
// fill% = (EMPTY - measured) / (EMPTY - FULL) * 100, clamped to 0..100.
// Each bin can have a different size/sensor mounting height, so these
// are tracked separately per sensor rather than shared.
float SENSOR1_EMPTY_DISTANCE_MM = 154.0;  // bin 1: measured empty reading
float SENSOR1_FULL_DISTANCE_MM  = 20.0;   // bin 1: measured full reading
float SENSOR2_EMPTY_DISTANCE_MM = 108.0;  // bin 2: measured empty reading
float SENSOR2_FULL_DISTANCE_MM  = 20.0;   // bin 2: measured full reading

// ---- Device identity / server ---------------------------------------
// Defaults; can also be changed later from the config portal without
// re-flashing. Each bin gets its own ID since they're reported separately.
String bin1Id    = "BIN-001";
String bin2Id    = "BIN-002";
String serverUrl = "";   // e.g. https://example.com/api/bins/report

// ==================================================================
// ==================  ADVANCED SETTINGS  ==========================
// ==================================================================
// Rarely need to change these.
const unsigned long REPORT_INTERVAL_MS = 5UL * 1000UL;    // send every 5 s
const unsigned long SENSOR_TIMEOUT_US  = 30000UL;         // ~5 m max echo wait
const int           SAMPLES_PER_READ   = 5;               // median-ish averaging

// NTP servers for timestamps (UTC)
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.nist.gov";

// ------------------------------------------------------------------
// GLOBALS
// ------------------------------------------------------------------
Preferences prefs;

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

  // Time sync for timestamps (WAT = UTC+1, no DST)
  configTime(3600, 0, NTP_SERVER1, NTP_SERVER2);
  waitForTime();

  Serial.printf("Bin 1 ID  : %s\n", bin1Id.c_str());
  Serial.printf("Bin 2 ID  : %s\n", bin2Id.c_str());
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
  bin1Id    = prefs.getString("bin1_id", bin1Id);
  bin2Id    = prefs.getString("bin2_id", bin2Id);
  serverUrl = prefs.getString("server_url", serverUrl);
  SENSOR1_EMPTY_DISTANCE_MM = prefs.getFloat("s1_empty_mm", SENSOR1_EMPTY_DISTANCE_MM);
  SENSOR1_FULL_DISTANCE_MM  = prefs.getFloat("s1_full_mm",  SENSOR1_FULL_DISTANCE_MM);
  SENSOR2_EMPTY_DISTANCE_MM = prefs.getFloat("s2_empty_mm", SENSOR2_EMPTY_DISTANCE_MM);
  SENSOR2_FULL_DISTANCE_MM  = prefs.getFloat("s2_full_mm",  SENSOR2_FULL_DISTANCE_MM);
  prefs.end();
}

void saveSettings() {
  prefs.begin("smartbin", false);  // read-write
  prefs.putString("bin1_id", bin1Id);
  prefs.putString("bin2_id", bin2Id);
  prefs.putString("server_url", serverUrl);
  prefs.putFloat("s1_empty_mm", SENSOR1_EMPTY_DISTANCE_MM);
  prefs.putFloat("s1_full_mm",  SENSOR1_FULL_DISTANCE_MM);
  prefs.putFloat("s2_empty_mm", SENSOR2_EMPTY_DISTANCE_MM);
  prefs.putFloat("s2_full_mm",  SENSOR2_FULL_DISTANCE_MM);
  prefs.end();
  Serial.println("Settings saved to NVS.");
}

// ==================================================================
// WIFI + CAPTIVE PORTAL
// ==================================================================
void setupWiFi() {
  // If a WiFi network is hardcoded above, try it first so the bin can
  // boot straight onto your network with no portal interaction at all.
  if (strlen(WIFI_SSID) > 0) {
    Serial.printf("Connecting to hardcoded WiFi \"%s\"", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WiFi connected. IP: ");
      Serial.println(WiFi.localIP());
      if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, HIGH);
      return;
    }

    // Hardcoded attempt failed (wrong password / AP out of range / etc).
    // Abort the pending connection cleanly before handing off to
    // WiFiManager below -- otherwise its own connection attempt collides
    // with this still-in-progress one ("sta is connecting, cannot set
    // config") and AutoConnect fails immediately.
    Serial.println("Hardcoded WiFi failed; falling back to config portal.");
    WiFi.disconnect(true, true);
    delay(500);
  }

  WiFiManager wm;

  // Custom fields shown in the captive portal, pre-filled with saved values
  WiFiManagerParameter pBin1Id("bin1_id", "Bin 1 ID", bin1Id.c_str(), 40);
  WiFiManagerParameter pBin2Id("bin2_id", "Bin 2 ID", bin2Id.c_str(), 40);
  WiFiManagerParameter pUrl("server_url", "Server URL (https://...)", serverUrl.c_str(), 200);
  char empty1Buf[16]; dtostrf(SENSOR1_EMPTY_DISTANCE_MM, 0, 1, empty1Buf);
  char full1Buf[16];  dtostrf(SENSOR1_FULL_DISTANCE_MM,  0, 1, full1Buf);
  char empty2Buf[16]; dtostrf(SENSOR2_EMPTY_DISTANCE_MM, 0, 1, empty2Buf);
  char full2Buf[16];  dtostrf(SENSOR2_FULL_DISTANCE_MM,  0, 1, full2Buf);
  WiFiManagerParameter pEmpty1("s1_empty_mm", "Bin 1 empty distance (mm)", empty1Buf, 8);
  WiFiManagerParameter pFull1("s1_full_mm",  "Bin 1 full distance (mm)",  full1Buf, 8);
  WiFiManagerParameter pEmpty2("s2_empty_mm", "Bin 2 empty distance (mm)", empty2Buf, 8);
  WiFiManagerParameter pFull2("s2_full_mm",  "Bin 2 full distance (mm)",  full2Buf, 8);

  wm.addParameter(&pBin1Id);
  wm.addParameter(&pBin2Id);
  wm.addParameter(&pUrl);
  wm.addParameter(&pEmpty1);
  wm.addParameter(&pFull1);
  wm.addParameter(&pEmpty2);
  wm.addParameter(&pFull2);

  wm.setConfigPortalTimeout(180);   // seconds before giving up and retrying

  // Tries saved creds first; if none/failed, opens AP "SmartBin-Setup"
  bool connected = wm.autoConnect("SmartBin-Setup", "binsetup123");

  if (wm.getWiFiIsSaved()) {
    // Portal ran and user may have edited the custom fields -> persist them
    applyPortalParams(pBin1Id, pBin2Id, pUrl, pEmpty1, pFull1, pEmpty2, pFull2);
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

  WiFiManagerParameter pBin1Id("bin1_id", "Bin 1 ID", bin1Id.c_str(), 40);
  WiFiManagerParameter pBin2Id("bin2_id", "Bin 2 ID", bin2Id.c_str(), 40);
  WiFiManagerParameter pUrl("server_url", "Server URL (https://...)", serverUrl.c_str(), 200);
  char empty1Buf[16]; dtostrf(SENSOR1_EMPTY_DISTANCE_MM, 0, 1, empty1Buf);
  char full1Buf[16];  dtostrf(SENSOR1_FULL_DISTANCE_MM,  0, 1, full1Buf);
  char empty2Buf[16]; dtostrf(SENSOR2_EMPTY_DISTANCE_MM, 0, 1, empty2Buf);
  char full2Buf[16];  dtostrf(SENSOR2_FULL_DISTANCE_MM,  0, 1, full2Buf);
  WiFiManagerParameter pEmpty1("s1_empty_mm", "Bin 1 empty distance (mm)", empty1Buf, 8);
  WiFiManagerParameter pFull1("s1_full_mm",  "Bin 1 full distance (mm)",  full1Buf, 8);
  WiFiManagerParameter pEmpty2("s2_empty_mm", "Bin 2 empty distance (mm)", empty2Buf, 8);
  WiFiManagerParameter pFull2("s2_full_mm",  "Bin 2 full distance (mm)",  full2Buf, 8);

  wm.addParameter(&pBin1Id);
  wm.addParameter(&pBin2Id);
  wm.addParameter(&pUrl);
  wm.addParameter(&pEmpty1);
  wm.addParameter(&pFull1);
  wm.addParameter(&pEmpty2);
  wm.addParameter(&pFull2);

  wm.setConfigPortalTimeout(180);
  if (wm.startConfigPortal("SmartBin-Setup", "binsetup123")) {
    applyPortalParams(pBin1Id, pBin2Id, pUrl, pEmpty1, pFull1, pEmpty2, pFull2);
  }
  Serial.println("Portal closed; restarting...");
  delay(1000);
  ESP.restart();
}

void applyPortalParams(WiFiManagerParameter& pBin1Id,
                       WiFiManagerParameter& pBin2Id,
                       WiFiManagerParameter& pUrl,
                       WiFiManagerParameter& pEmpty1,
                       WiFiManagerParameter& pFull1,
                       WiFiManagerParameter& pEmpty2,
                       WiFiManagerParameter& pFull2) {
  bin1Id    = String(pBin1Id.getValue());
  bin2Id    = String(pBin2Id.getValue());
  serverUrl = String(pUrl.getValue());
  SENSOR1_EMPTY_DISTANCE_MM = atof(pEmpty1.getValue());
  SENSOR1_FULL_DISTANCE_MM  = atof(pFull1.getValue());
  SENSOR2_EMPTY_DISTANCE_MM = atof(pEmpty2.getValue());
  SENSOR2_FULL_DISTANCE_MM  = atof(pFull2.getValue());
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

// getLocalTime() already returns WAT (UTC+1) since configTime() was set up
// with a 3600s offset, so the ISO-8601 suffix must be "+01:00", not "Z"
// ("Z" specifically means zero/UTC offset).
String getIso8601Timestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01T00:00:00+01:00";
  }
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+01:00", &timeinfo);
  return String(buf);
}

// ==================================================================
// ULTRASONIC READING
// ==================================================================
// Returns distance in mm, or -1.0 on timeout / no echo.
float readUltrasonicOnce(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, SENSOR_TIMEOUT_US);
  if (duration == 0) return -1.0;          // timeout
  return (duration * 0.343f) / 2.0f;       // speed of sound ~343 m/s
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

// Convert a measured distance to a 0..100 fill percentage, using the
// empty/full calibration for that specific bin/sensor.
float distanceToFillPct(float distanceMm, float emptyMm, float fullMm) {
  if (distanceMm < 0) return -1.0;   // invalid
  float span = emptyMm - fullMm;
  if (span <= 0) return -1.0;        // misconfigured geometry
  float pct = (emptyMm - distanceMm) / span * 100.0f;
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

  float f1 = distanceToFillPct(d1, SENSOR1_EMPTY_DISTANCE_MM, SENSOR1_FULL_DISTANCE_MM);
  float f2 = distanceToFillPct(d2, SENSOR2_EMPTY_DISTANCE_MM, SENSOR2_FULL_DISTANCE_MM);

  if (f1 < 0 && f2 < 0) {
    Serial.println("Both bins failed to read; skipping this cycle.");
    return;
  }

  Serial.printf("%s: %.1f mm (%.1f%%)   %s: %.1f mm (%.1f%%)\n",
                bin1Id.c_str(), d1, f1, bin2Id.c_str(), d2, f2);

  String payload = buildJson(d1, f1, d2, f2);

  Serial.println("JSON payload:");
  Serial.println(payload);

  sendReport(payload);
}

// Each bin is reported independently -- there is no combined/averaged
// fill_pct, since bin 1 and bin 2 are physically separate bins.
String buildJson(float d1, float f1, float d2, float f2) {
  StaticJsonDocument<384> doc;
  doc["timestamp"] = getIso8601Timestamp();

  JsonArray bins = doc.createNestedArray("bins");

  JsonObject b1 = bins.createNestedObject();
  b1["bin_id"] = bin1Id;
  if (d1 >= 0) { b1["distance_mm"] = roundf(d1 * 10) / 10.0; b1["fill_pct"] = roundf(f1 * 10) / 10.0; }
  else         { b1["distance_mm"] = nullptr;                b1["fill_pct"] = nullptr; }

  JsonObject b2 = bins.createNestedObject();
  b2["bin_id"] = bin2Id;
  if (d2 >= 0) { b2["distance_mm"] = roundf(d2 * 10) / 10.0; b2["fill_pct"] = roundf(f2 * 10) / 10.0; }
  else         { b2["distance_mm"] = nullptr;                b2["fill_pct"] = nullptr; }

  String out;
  serializeJson(doc, out);
  return out;
}

void sendReport(const String& payload) {
  if (serverUrl.length() == 0) {
    Serial.println("No server URL set; skipping HTTP POST.");
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

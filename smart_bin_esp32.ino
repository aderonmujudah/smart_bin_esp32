/*
 * Smart Bin - ESP32
 * -----------------
 * Reads two HC-SR04 ultrasonic sensors, converts each distance to a
 * fill percentage (each sensor has its own empty/full calibration,
 * since they can be mounted on bins of different sizes/shapes), and
 * POSTs a JSON message to a configurable URL. The JSON is also always
 * printed to the Serial Monitor at 115200 baud.
 *
 * Each bin is reported independently as its own POST request (both bins
 * share one deviceId, since they're the same ESP32):
 * {
 *   "deviceId": "DEVICE-001",
 *   "binId": "BIN-001",
 *   "fillPercentage": 72,
 *   "recordedAt": "2026-07-15T14:03:22.000Z"   // UTC, ISO-8601
 * }
 *
 * Multiple WiFi networks can be configured (home, office, phone hotspot,
 * etc.) -- on every boot the ESP32 loops through all of them (via
 * WiFiMulti) and connects to whichever is in range. Networks are entered
 * either as hardcoded defaults in this file, or through a captive portal
 * (WiFiManager): the ESP32 opens a WiFi access point named
 * "SmartBin-Setup", the user connects to it with a phone/laptop and
 * enters SSID/password for one or more networks from a web page. All
 * configured networks, plus device settings (bin IDs, server URL,
 * calibration distances), are saved to NVS (Non-Volatile Storage) via
 * the Preferences library (see loadSettings() / saveSettings() below) --
 * a dedicated flash partition that is NOT touched by a normal "Upload" of
 * new firmware (only "Erase Flash" or changing the partition table wipes
 * it), so everything survives re-flashing.
 *
 * Libraries required (install via Arduino Library Manager):
 *   - WiFiManager           by tzapu
 *   - ArduinoJson           by Benoit Blanchon
 * (WiFi, HTTPClient, WiFiClientSecure, Preferences ship with the ESP32 core.)
 */

#include <WiFi.h>
#include <WiFiMulti.h>
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
// Fill in as many networks as you want tried on every boot (home,
// office, phone hotspot, etc.) -- the ESP32 loops through all of them
// and connects to whichever is in range. Leave a slot's SSID as "" to
// skip it. More networks can be added later without re-flashing via the
// "SmartBin-Setup" config portal (see README).
// NOTE: all configured networks (hardcoded here, or added later via the
// portal) are saved to NVS flash, a partition normal firmware uploads
// do NOT erase, so they survive re-flashing.
const int MAX_WIFI_NETWORKS = 4;
struct WifiCredential { String ssid; String password; };
WifiCredential wifiNetworks[MAX_WIFI_NETWORKS] = {
  { "", "" },
  { "", "" },
  { "", "" },
  { "", "" },
};

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
float SENSOR2_EMPTY_DISTANCE_MM = 258.0;  // bin 2: measured empty reading
float SENSOR2_FULL_DISTANCE_MM  = 20.0;   // bin 2: measured full reading

// ---- Device identity / server ---------------------------------------
// Defaults; can also be changed later from the config portal without
// re-flashing. Both bins share one deviceId (same ESP32); each still gets
// its own binId since they're reported independently.
String deviceId  = "DEVICE-001";
String bin1Id    = "BIN-001";
String bin2Id    = "BIN-002";
String serverUrl = "https://honorable-oyster-125.eu-west-1.convex.site/hardware/readings";   // e.g. https://example.com/api/bins/report

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

  Serial.printf("Device ID : %s\n", deviceId.c_str());
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
  deviceId  = prefs.getString("device_id", deviceId);
  bin1Id    = prefs.getString("bin1_id", bin1Id);
  bin2Id    = prefs.getString("bin2_id", bin2Id);
  // Only override the hardcoded default if a non-empty URL was actually
  // saved before -- otherwise a previously-empty saved value would keep
  // clobbering a new hardcoded default on every boot.
  String savedUrl = prefs.getString("server_url", serverUrl);
  if (savedUrl.length() > 0) serverUrl = savedUrl;
  SENSOR1_EMPTY_DISTANCE_MM = prefs.getFloat("s1_empty_mm", SENSOR1_EMPTY_DISTANCE_MM);
  SENSOR1_FULL_DISTANCE_MM  = prefs.getFloat("s1_full_mm",  SENSOR1_FULL_DISTANCE_MM);
  SENSOR2_EMPTY_DISTANCE_MM = prefs.getFloat("s2_empty_mm", SENSOR2_EMPTY_DISTANCE_MM);
  SENSOR2_FULL_DISTANCE_MM  = prefs.getFloat("s2_full_mm",  SENSOR2_FULL_DISTANCE_MM);
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    char ssidKey[16], passKey[16];
    snprintf(ssidKey, sizeof(ssidKey), "wifi%d_ssid", i);
    snprintf(passKey, sizeof(passKey), "wifi%d_pass", i);
    wifiNetworks[i].ssid     = prefs.getString(ssidKey, wifiNetworks[i].ssid);
    wifiNetworks[i].password = prefs.getString(passKey, wifiNetworks[i].password);
  }
  prefs.end();
}

void saveSettings() {
  prefs.begin("smartbin", false);  // read-write
  prefs.putString("device_id", deviceId);
  prefs.putString("bin1_id", bin1Id);
  prefs.putString("bin2_id", bin2Id);
  prefs.putString("server_url", serverUrl);
  prefs.putFloat("s1_empty_mm", SENSOR1_EMPTY_DISTANCE_MM);
  prefs.putFloat("s1_full_mm",  SENSOR1_FULL_DISTANCE_MM);
  prefs.putFloat("s2_empty_mm", SENSOR2_EMPTY_DISTANCE_MM);
  prefs.putFloat("s2_full_mm",  SENSOR2_FULL_DISTANCE_MM);
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    char ssidKey[16], passKey[16];
    snprintf(ssidKey, sizeof(ssidKey), "wifi%d_ssid", i);
    snprintf(passKey, sizeof(passKey), "wifi%d_pass", i);
    prefs.putString(ssidKey, wifiNetworks[i].ssid);
    prefs.putString(passKey, wifiNetworks[i].password);
  }
  prefs.end();
  Serial.println("Settings saved to NVS.");
}

// Adds a newly-connected network to the try-list (matching by SSID updates
// its password instead of adding a duplicate), so it's tried automatically
// on future boots alongside the rest of wifiNetworks[].
void rememberWifiNetwork(const String& ssid, const String& password) {
  if (ssid.length() == 0) return;
  int freeSlot = -1;
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (wifiNetworks[i].ssid == ssid) {
      wifiNetworks[i].password = password;
      return;
    }
    if (freeSlot < 0 && wifiNetworks[i].ssid.length() == 0) freeSlot = i;
  }
  if (freeSlot >= 0) {
    wifiNetworks[freeSlot].ssid     = ssid;
    wifiNetworks[freeSlot].password = password;
  } else {
    Serial.println("WiFi network list full; not remembering new network (increase MAX_WIFI_NETWORKS).");
  }
}

// ==================================================================
// WIFI + CAPTIVE PORTAL
// ==================================================================
void setupWiFi() {
  // Try every configured network first (hardcoded above, or added earlier
  // via the portal) so the bin can boot straight onto whichever is in
  // range with no portal interaction at all. WiFiMulti loops over all of
  // them and connects to the strongest one it can reach.
  WiFiMulti wifiMulti;
  int configured = 0;
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (wifiNetworks[i].ssid.length() > 0) {
      wifiMulti.addAP(wifiNetworks[i].ssid.c_str(), wifiNetworks[i].password.c_str());
      configured++;
    }
  }

  if (configured > 0) {
    Serial.printf("Trying %d saved WiFi network(s)", configured);
    WiFi.mode(WIFI_STA);
    unsigned long start = millis();
    while (wifiMulti.run() != WL_CONNECTED && millis() - start < 20000UL) {
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi connected to \"%s\". IP: ", WiFi.SSID().c_str());
      Serial.println(WiFi.localIP());
      if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, HIGH);
      return;
    }

    // None of the saved networks were reachable (out of range / all
    // passwords stale / etc). Abort the pending connection cleanly before
    // handing off to WiFiManager below -- otherwise its own connection
    // attempt collides with this still-in-progress one ("sta is
    // connecting, cannot set config") and AutoConnect fails immediately.
    Serial.println("No saved WiFi networks reachable; falling back to config portal.");
    WiFi.disconnect(true, true);
    delay(500);
  }

  runConfigPortal(/*forcePortal=*/false);
}

// On-demand portal (triggered by holding the config button)
void openConfigPortal() {
  runConfigPortal(/*forcePortal=*/true);
}

// Shared by setupWiFi() (auto-connect, falls back to portal) and
// openConfigPortal() (always opens the portal). Restarts the ESP32 when done
// either way, since WiFi/settings state is cleanest resolved by a fresh boot.
void runConfigPortal(bool forcePortal) {
  WiFiManager wm;

  WiFiManagerParameter pDeviceId("device_id", "Device ID", deviceId.c_str(), 40);
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

  wm.addParameter(&pDeviceId);
  wm.addParameter(&pBin1Id);
  wm.addParameter(&pBin2Id);
  wm.addParameter(&pUrl);
  wm.addParameter(&pEmpty1);
  wm.addParameter(&pFull1);
  wm.addParameter(&pEmpty2);
  wm.addParameter(&pFull2);

  // Extra "WiFi N SSID/Password" fields so more backup networks can be
  // added straight from the portal, without re-flashing. Allocated on the
  // heap since WiFiManagerParameter keeps referencing these until the
  // portal closes, well past this loop's scope.
  WiFiManagerParameter* ssidParams[MAX_WIFI_NETWORKS];
  WiFiManagerParameter* passParams[MAX_WIFI_NETWORKS];
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    char ssidLabel[24], passLabel[24], ssidId[16], passId[16];
    snprintf(ssidLabel, sizeof(ssidLabel), "WiFi %d SSID", i + 1);
    snprintf(passLabel, sizeof(passLabel), "WiFi %d Password", i + 1);
    snprintf(ssidId, sizeof(ssidId), "wifi%d_ssid", i);
    snprintf(passId, sizeof(passId), "wifi%d_pass", i);
    ssidParams[i] = new WiFiManagerParameter(ssidId, ssidLabel, wifiNetworks[i].ssid.c_str(), 40);
    passParams[i] = new WiFiManagerParameter(passId, passLabel, wifiNetworks[i].password.c_str(), 64);
    wm.addParameter(ssidParams[i]);
    wm.addParameter(passParams[i]);
  }

  wm.setConfigPortalTimeout(180);   // seconds before giving up and retrying

  bool connected = forcePortal
      ? wm.startConfigPortal("SmartBin-Setup", "binsetup123")
      : wm.autoConnect("SmartBin-Setup", "binsetup123");

  // Portal ran and user may have edited the custom fields, or connected to
  // a new network through WiFiManager's own "Configure WiFi" flow -> persist
  // everything, including that new network, into wifiNetworks[].
  if (forcePortal ? connected : wm.getWiFiIsSaved()) {
    deviceId  = String(pDeviceId.getValue());
    bin1Id    = String(pBin1Id.getValue());
    bin2Id    = String(pBin2Id.getValue());
    serverUrl = String(pUrl.getValue());
    SENSOR1_EMPTY_DISTANCE_MM = atof(pEmpty1.getValue());
    SENSOR1_FULL_DISTANCE_MM  = atof(pFull1.getValue());
    SENSOR2_EMPTY_DISTANCE_MM = atof(pEmpty2.getValue());
    SENSOR2_FULL_DISTANCE_MM  = atof(pFull2.getValue());
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
      wifiNetworks[i].ssid     = String(ssidParams[i]->getValue());
      wifiNetworks[i].password = String(passParams[i]->getValue());
    }
    if (connected) rememberWifiNetwork(WiFi.SSID(), wm.getWiFiPass());
    saveSettings();
  }

  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    delete ssidParams[i];
    delete passParams[i];
  }

  if (forcePortal) {
    Serial.println("Portal closed; restarting...");
    delay(1000);
    ESP.restart();
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

// recordedAt is reported in UTC (suffix "Z"), regardless of the WAT
// offset configTime() was set up with for local-time use elsewhere.
String getIso8601UtcTimestamp() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &timeinfo);
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

  String recordedAt = getIso8601UtcTimestamp();

  // Each bin is reported independently, as its own POST request -- there
  // is no combined/averaged fill_pct, since bin 1 and bin 2 are physically
  // separate bins. A bin whose sensor failed to read this cycle is skipped
  // rather than sending a bogus fillPercentage.
  if (f1 >= 0) {
    String payload1 = buildBinJson(bin1Id, f1, recordedAt);
    Serial.println("JSON payload (bin 1):");
    Serial.println(payload1);
    sendReport(payload1);
  }

  if (f2 >= 0) {
    String payload2 = buildBinJson(bin2Id, f2, recordedAt);
    Serial.println("JSON payload (bin 2):");
    Serial.println(payload2);
    sendReport(payload2);
  }
}

String buildBinJson(const String& binId, float fillPct, const String& recordedAt) {
  StaticJsonDocument<256> doc;
  doc["deviceId"] = deviceId;
  doc["binId"] = binId;
  doc["fillPercentage"] = (int)roundf(fillPct);
  doc["recordedAt"] = recordedAt;

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

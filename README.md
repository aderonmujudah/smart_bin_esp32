 # Smart Bin (ESP32)

A prototype smart waste bin. An ESP32 reads two ultrasonic sensors, works out how
full the bin is, and posts a JSON message to a web endpoint over HTTPS at a fixed
interval.

---

## What it sends

Every reporting cycle (default: 60 s) the ESP32 POSTs a JSON body like this:

```json
{
  "bin_id": "BIN-001",
  "timestamp": "2026-07-15T14:03:22Z",
  "fill_pct": 72.5,
  "sensors": [
    { "id": 1, "distance_cm": 11.2, "fill_pct": 71.0 },
    { "id": 2, "distance_cm": 10.4, "fill_pct": 74.0 }
  ]
}
```

| Field        | Meaning                                                             |
|--------------|--------------------------------------------------------------------|
| `bin_id`     | Unique bin identifier (set in the config portal).                  |
| `timestamp`  | UTC time the message left the ESP32, ISO-8601 (`...Z`).            |
| `fill_pct`   | **Required value** — average fill % across the working sensors.    |
| `sensors[]`  | Per-sensor detail (raw distance + individual fill %). Optional.    |

> The three fields you asked for are `bin_id`, `timestamp`, and `fill_pct`.
> The `sensors` array is extra detail you can ignore server-side if you don't need it.
> A sensor that failed to read reports `distance_cm: null` / `fill_pct: null` and is
> left out of the average.

The same JSON is also always printed to the **Serial Monitor at 115200 baud**
every reporting cycle, whether or not the HTTP POST succeeds (or even if no
Server URL is set yet) — handy for checking readings without a server.

---

## Why HTTPS (and not MQTT)

I chose **HTTPS POST** because:

- You only need to give a URL — no message broker to run or configure.
- Any web framework (Flask, Express, FastAPI, a serverless function, etc.) can
  receive it directly.
- It's encrypted in transit.

If you later prefer **MQTT** (better for many bins / low bandwidth / push), the
`sendReport()` function is the only place that would change — the sensor and
percentage logic stays identical. Ask and I'll add an MQTT variant.

---

## Hardware

- 1 × ESP32 dev board (e.g. ESP32-WROOM DevKit v1)
- 2 × HC-SR04 ultrasonic distance sensors
- Voltage dividers or a logic-level shifter for the two **ECHO** lines (see below)
- Jumper wires, breadboard
- Optional: a push button (or just use the onboard **BOOT** button on GPIO0)

### ⚠️ 5 V ECHO warning
HC-SR04 runs on 5 V and its **ECHO** pin outputs 5 V, but ESP32 GPIOs are **3.3 V
only**. Drop each ECHO down before it reaches the ESP32, using a simple divider:

```
ECHO ──[ 1kΩ ]──┬──> ESP32 GPIO
                │
             [ 2kΩ ]
                │
               GND
```

(TRIG can be driven directly from the ESP32 — 3.3 V is enough to trigger it.)

### Wiring (defaults in the code)

| Signal          | HC-SR04 #1 | HC-SR04 #2 | ESP32 pin        |
|-----------------|------------|------------|------------------|
| VCC             | VCC        | VCC        | 5V / VIN         |
| GND             | GND        | GND        | GND              |
| TRIG            | Trig       |            | GPIO 5           |
| ECHO (÷ divider)| Echo       |            | GPIO 18          |
| TRIG            |            | Trig       | GPIO 17          |
| ECHO (÷ divider)|            | Echo       | GPIO 16          |

Change the pin numbers at the top of `smart_bin_esp32.ino` if your wiring differs.

Sensor placement: mount both sensors in the lid pointing straight down, ideally
over different areas of the bin, so uneven piles of trash average out to a sensible
fill level.

---

## Software setup

### 1. Install the Arduino IDE + ESP32 support
- Install the [Arduino IDE](https://www.arduino.cc/en/software).
- **File → Preferences → Additional Boards Manager URLs**, add:
  `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
- **Tools → Board → Boards Manager**, search *esp32*, install **esp32 by Espressif**.
- **Tools → Board**, pick your board (e.g. *ESP32 Dev Module*).

### 2. Install the required libraries
**Sketch → Include Library → Manage Libraries**, install:
- **WiFiManager** by *tzapu*
- **ArduinoJson** by *Benoit Blanchon*

(`WiFi`, `HTTPClient`, `WiFiClientSecure`, `Preferences` come with the ESP32 core.)

### 3. Flash the sketch
- Open `smart_bin_esp32.ino`.
- Select the board and the correct COM port under **Tools**.
- Click **Upload**.

---

## Configuration

All the settings you're likely to need to change — WiFi, pin wiring, and
per-bin calibration distances — are collected in one **"EDIT THIS SECTION"**
block near the top of `smart_bin_esp32.ino`, right after the `#include`s.

### Option A: hardcode your WiFi in the code
Set `WIFI_SSID` / `WIFI_PASSWORD` in that block to your network's credentials
and re-upload. The bin will connect straight to that network on every boot —
no portal needed. Leave both as `""` to use the captive portal instead (below).

### Option B: captive portal (no re-flashing needed)
On first boot (or whenever no WiFi is saved/reachable, and `WIFI_SSID` is left
blank) the ESP32 starts its own WiFi access point:

1. On your phone/laptop, connect to the WiFi network **`SmartBin-Setup`**
   (password **`binsetup123`**).
2. A configuration page opens automatically (if not, browse to `http://192.168.4.1`).
3. Tap **Configure WiFi** and you'll see:
   - Your home/office WiFi network + password
   - **Bin ID** — e.g. `BIN-001`
   - **Server URL** — the endpoint to POST to, e.g.
     `https://your-server.com/api/bins/report`
   - **Bin 1 empty distance (cm)** / **Bin 1 full distance (cm)** — calibration
     for sensor 1 (see below)
   - **Bin 2 empty distance (cm)** / **Bin 2 full distance (cm)** — calibration
     for sensor 2, set independently since bin 2 can be a different size/shape
4. **Save.** The ESP32 stores everything and reboots onto your network.

### Where settings are stored (survives re-flashing)
- **WiFi credentials** — whether typed into the portal or hardcoded via
  `WIFI_SSID`/`WIFI_PASSWORD` — are saved by the ESP32's WiFi driver in **NVS
  flash**.
- **Bin ID, Server URL, and calibration** are saved with the `Preferences`
  library, also in **NVS**.

NVS lives in a separate flash partition from your program, so **uploading new
firmware does NOT erase your saved settings.** They are only wiped by a full chip
erase (`esptool.py erase_flash`, or "Erase All Flash Before Sketch Upload" in the
IDE).

### Re-opening the portal later
Hold the **BOOT** button (GPIO0) while the bin is running to reopen the config
portal and change any setting. (Set `CONFIG_BUTTON_PIN` to `-1` in the code to
disable this.)

---

## Calibrating the fill percentage

The fill percentage is computed from the measured distance:

```
fill% = (EMPTY_distance - measured_distance) / (EMPTY_distance - FULL_distance) * 100
```

clamped to 0–100. **Each sensor/bin has its own Empty/Full pair** — bin 1 and bin 2
don't have to be the same size or have the sensor mounted at the same height. So
you need two numbers per bin, in centimetres:

- **Empty distance** — what the sensor reads when its bin is **empty**
  (roughly the distance from the sensor down to the bin floor).
- **Full distance** — what the sensor reads when its bin is **full**
  (how close the trash gets to the sensor before you consider it 100%).

**How to measure:** for each bin, empty it, open the Serial Monitor (115200 baud)
and note the reported `cm` for that sensor — that's its *Empty distance*. Then hold
something at the "full" level over that same bin and note that `cm` — that's its
*Full distance*. Enter both pairs in the portal (or edit `SENSOR1_EMPTY_DISTANCE_CM`
/ `SENSOR1_FULL_DISTANCE_CM` / `SENSOR2_EMPTY_DISTANCE_CM` / `SENSOR2_FULL_DISTANCE_CM`
in the code).

---

## Receiving the data on your server

You just need an endpoint that accepts an HTTP `POST` with a JSON body. Example
in Python/Flask:

```python
from flask import Flask, request

app = Flask(__name__)

@app.route("/api/bins/report", methods=["POST"])
def report():
    data = request.get_json()
    print(data["bin_id"], data["timestamp"], data["fill_pct"])
    # ...store in a database, trigger an alert if fill_pct > 90, etc.
    return {"status": "ok"}, 200
```

Point the **Server URL** field at this endpoint (must be reachable from the
internet if the bin isn't on the same LAN).

### About the TLS certificate
For a prototype the code uses `client.setInsecure()` — it makes the HTTPS request
but does **not** verify the server's certificate. That's fine for testing. For a
real deployment, replace it with your server's root CA using
`client.setCACert(rootCaPem)` (there's a comment marking the exact spot in
`sendReport()`).

---

## Tuning

Constants near the top of `smart_bin_esp32.ino`:

| Constant                     | Default   | Purpose                                  |
|------------------------------|-----------|------------------------------------------|
| `REPORT_INTERVAL_MS`         | 60 000    | How often to send a report (ms).         |
| `SAMPLES_PER_READ`           | 5         | Samples averaged per sensor reading.     |
| `SENSOR_TIMEOUT_US`          | 30 000    | Max echo wait before a reading is "fail".|
| `SENSOR1_EMPTY_DISTANCE_CM`  | 40.0      | Bin 1 distance when empty (also in portal).|
| `SENSOR1_FULL_DISTANCE_CM`   | 5.0       | Bin 1 distance when full (also in portal). |
| `SENSOR2_EMPTY_DISTANCE_CM`  | 40.0      | Bin 2 distance when empty (also in portal).|
| `SENSOR2_FULL_DISTANCE_CM`   | 5.0       | Bin 2 distance when full (also in portal). |

---

## Troubleshooting

| Symptom                                   | Likely cause / fix                                             |
|-------------------------------------------|----------------------------------------------------------------|
| Can't see `SmartBin-Setup` network        | Power-cycle; portal only opens when WiFi isn't saved/reachable. |
| Fill % stuck at 0 or 100                   | Calibration off — recheck Empty/Full distances.                |
| Distance reads as `-1` / both sensors fail | Check ECHO voltage divider & wiring; TRIG/ECHO not swapped.     |
| `POST failed`                             | Bad/unreachable Server URL, or no internet on that WiFi.        |
| Timestamp is `1970-...`                    | NTP didn't sync — check internet access; it retries each boot.  |
| Settings lost after upload                | You did a *full chip erase*. Normal uploads keep NVS intact.    |

---

## File overview

```
smart_bin_esp32/
├── smart_bin_esp32.ino   # ESP32 firmware (sensors, WiFi, JSON, HTTPS)
└── README.md             # this file
```

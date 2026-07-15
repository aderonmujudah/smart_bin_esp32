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

## First-boot configuration (captive portal)

On first boot (or whenever no WiFi is saved) the ESP32 starts its own WiFi
access point:

1. On your phone/laptop, connect to the WiFi network **`SmartBin-Setup`**
   (password **`binsetup123`**).
2. A configuration page opens automatically (if not, browse to `http://192.168.4.1`).
3. Tap **Configure WiFi** and you'll see:
   - Your home/office WiFi network + password
   - **Bin ID** — e.g. `BIN-001`
   - **Server URL** — the endpoint to POST to, e.g.
     `https://your-server.com/api/bins/report`
   - **Empty distance (cm)** and **Full distance (cm)** — calibration (see below)
4. **Save.** The ESP32 stores everything and reboots onto your network.

### Where settings are stored (survives re-flashing)
- **WiFi credentials** are saved by WiFiManager in the ESP32's **NVS flash**.
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

clamped to 0–100. So you need two numbers, in centimetres:

- **Empty distance** — what the sensor reads when the bin is **empty**
  (roughly the distance from the sensor down to the bin floor).
- **Full distance** — what the sensor reads when the bin is **full**
  (how close the trash gets to the sensor before you consider it 100%).

**How to measure:** empty the bin, open the Serial Monitor (115200 baud) and note
the reported `cm` — that's your *Empty distance*. Then hold something at the "full"
level and note that `cm` — that's your *Full distance*. Enter both in the portal
(or edit `BIN_EMPTY_DISTANCE_CM` / `BIN_FULL_DISTANCE_CM` in the code).

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

| Constant                | Default   | Purpose                                  |
|-------------------------|-----------|------------------------------------------|
| `REPORT_INTERVAL_MS`    | 60 000    | How often to send a report (ms).         |
| `SAMPLES_PER_READ`      | 5         | Samples averaged per sensor reading.     |
| `SENSOR_TIMEOUT_US`     | 30 000    | Max echo wait before a reading is "fail".|
| `BIN_EMPTY_DISTANCE_CM` | 40.0      | Distance when empty (also set in portal).|
| `BIN_FULL_DISTANCE_CM`  | 5.0       | Distance when full (also set in portal). |

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

# Library AtmoSensor

An ESP8266-based environment monitor for a 3D printing room. Tracks temperature, humidity, barometric pressure, and light level. Displays readings on an onboard OLED and sends alerts and status updates via a shared Telegram bot. Designed to scale — multiple devices can share one bot, each addressable individually or all at once.

---

## Hardware

- **Board**: ideaspark ESP8266 NodeMCU with onboard 0.96" SSD1306 OLED (128x64, yellow top strip / blue main area)
- **DHT11** — temperature and humidity sensor
- **BMP180** — barometric pressure sensor
- **BH1750** — ambient light sensor

### Wiring

| Sensor | VCC | GND | SCL | SDA | DATA |
|--------|-----|-----|-----|-----|------|
| DHT11  | 3.3V | GND | — | — | D7 |
| BMP180 | 3.3V | GND | D1 | D2 | — |
| BH1750 | 3.3V | GND | D1 | D2 | — |
| OLED   | onboard | onboard | GPIO14 | GPIO12 | — |

> **Note**: BMP180 and BH1750 share the I2C bus on D1/D2. The BH1750 provides the pull-up resistors that the BMP180 depends on — both must be physically connected for the BMP180 to work. The OLED uses a separate software I2C bus on GPIO14/GPIO12 to avoid conflicts.

---

## Features

### OLED Display
- Cycles through four sensor readings every 4 seconds: **TEMPERATURE**, **HUMIDITY**, **LUX**, **BAROMETER**
- Top yellow strip shows a WiFi connection status icon and the current sensor label
- Bottom blue area shows the current value in large text
- Units: °F, %, lx, hPa

### Display Sleep / Wake — Light Sensor as a Switch

The BH1750 light sensor does double duty: it reads ambient lux as a sensor value *and* acts as a presence switch to control the OLED display. This means no physical buttons are needed to wake the screen.

**How it works:**
- The screen starts on at boot
- After **5 minutes** of no significant light change the screen turns off automatically
- The display wakes and the 5 minute timer resets on either of two triggers:
  - **Lights turning on or off** — any lux change of ≥ 5 lux wakes the screen. Walking into the room and flipping a light switch is enough.
  - **Covering the sensor** — briefly placing your hand or finger over the BH1750 causes a rapid drop to near zero lux, which is detected as an intentional wake gesture regardless of how bright or dim the room is

**Why this matters:**
In a room that goes completely dark overnight (like a 3D printing room with no windows), the display would otherwise stay on forever burning the OLED. The light sensor lets the screen behave contextually — on when the room is in use, off when it isn't — without any extra hardware or user interaction beyond what's already natural.

The lux value itself is still displayed on screen and reported in Telegram `/status` as normal.

### Telegram Bot
All devices in the ecosystem share one Telegram bot. Commands can be broadcast to all devices or targeted at a specific one by appending the device name.

| Command | Description |
|---------|-------------|
| `/status` | All devices respond with their current readings |
| `/status library` | Only the library device responds |
| `/stop` | All devices suppress humidity alerts |
| `/stop library` | Only the library device suppresses humidity alerts |

On boot each device sends: `[Device Name] online.`

### Humidity Alerting
A multi-stage alert system triggers when humidity exceeds the threshold (default 50%):

1. **First alert fires** — bot sends a warning message
2. **No response** — alert repeats every **5 minutes** until any message is received
3. **Response received** — repeating stops, a **1 hour hold** begins
4. **Still high after 1 hour** — the full alert cycle restarts from step 1
5. **`/stop` sent** — all alerts suppressed; system resets only after humidity drops below threshold and naturally rises above it again

This prevents alert fatigue while ensuring the problem is not silently ignored.

---

## Configuration

All sensitive credentials and device identity are stored in `secrets.h` which is gitignored. Copy `secrets.h.example` to `secrets.h` and fill in your values:

```cpp
#define WIFI_SSID "your_ssid"
#define WIFI_PASSWORD "your_password"
#define BOT_TOKEN "your_bot_token"
#define CHAT_ID "your_chat_id"
#define DEVICE_NAME "library"
```

`DEVICE_NAME` is the identifier used in targeted Telegram commands. It should be lowercase, one word, and unique across all devices in your ecosystem.

Other configurable defines at the top of the sketch:

| Define | Default | Description |
|--------|---------|-------------|
| `HUMIDITY_THRESHOLD` | `50.0` | % RH that triggers alerts |
| `SCREEN_DURATION` | `300000` | Screen timeout in ms (5 min) |
| `LUX_CHANGE_THRESHOLD` | `5.0` | Lux delta required to wake screen |
| `ALERT_REPEAT_INTERVAL` | `300000` | Ms between repeated humidity alerts (5 min) |
| `ALERT_RESTART_INTERVAL` | `3600000` | Ms before alert cycle restarts after ack (1 hour) |

---

## Adding a New Device to the Ecosystem

This sketch is designed to be redeployed as-is for every new sensor node. No code changes are required — only a new `secrets.h` with a unique `DEVICE_NAME`.

**Steps:**

1. Clone or copy this repo to a new folder for the new device
2. Copy `secrets.h.example` to `secrets.h`
3. Fill in credentials — use the **same** `BOT_TOKEN` and `CHAT_ID` as existing devices
4. Set a unique `DEVICE_NAME` for the new device, e.g. `"bedroom"`, `"server-room"`, `"workshop"`
5. Adjust `HUMIDITY_THRESHOLD` and other defines as appropriate for the new location
6. Upload to the new ESP8266

Once deployed, the new device will automatically join the shared bot ecosystem. You can address it individually with `/status bedroom` or broadcast to all devices with `/status`.

**Example ecosystem:**

| Device Name | Location | Threshold |
|-------------|----------|-----------|
| `library` | 3D print room | 50% |
| `bedroom` | Bedroom | 60% |
| `workshop` | Workshop | 55% |

---

## Libraries

| Library | Author |
|---------|--------|
| DHT sensor library | Adafruit |
| Adafruit BMP085 Library | Adafruit |
| BH1750 | Christopher Laws |
| U8g2 | oliver |
| UniversalTelegramBot | Brian Lough |
| ArduinoJson | Benoit Blanchon |

---

## Board Setup

- **Board**: NodeMCU 1.0 (ESP-12E Module)
- **Board package URL**: `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
- **FQBN**: `esp8266:esp8266:nodemcuv2`
- **Port**: `/dev/ttyUSB0` (CH340 driver built into Linux kernel)

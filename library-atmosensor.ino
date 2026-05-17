// ============================================================
// AtmoSensor — ESP8266 Environment Monitor
// Tracks temperature, humidity, pressure, and light.
// Displays readings on onboard OLED, sends Telegram alerts.
// Multiple devices share one bot via DEVICE_NAME targeting.
// ============================================================

#include <Arduino.h>
#include <math.h>
#include <U8g2lib.h>           // OLED display driver
#include <Wire.h>              // I2C communication for BMP180 and BH1750
#include <ESP8266WiFi.h>       // WiFi connection
#include <WiFiClientSecure.h>  // HTTPS client for Telegram API
#include <UniversalTelegramBot.h> // Telegram bot library
#include <ArduinoJson.h>       // JSON parsing required by Telegram library
#include <DHT.h>               // DHT11 temperature and humidity sensor
#include <Adafruit_BMP085.h>   // BMP180 barometric pressure sensor
#include <BH1750.h>            // BH1750 ambient light sensor
#include "secrets.h"           // WiFi credentials, bot token, chat ID, device name
                               // secrets.h is gitignored — never committed to repo
#include <user_interface.h>

// Pull WiFi credentials from secrets.h into const char* variables
// because WiFi.begin() expects const char* not #define strings
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ============================================================
// CONFIGURABLE THRESHOLDS AND TIMERS
// Adjust these per deployment without touching any logic
// ============================================================
#define DHTPIN D7                      // DHT11 data pin
#define DHTTYPE DHT11                  // Sensor type (DHT11 vs DHT22)
#define HUMIDITY_THRESHOLD 50.0        // % RH that triggers humidity alerts
#define SCREEN_DURATION 300000         // Screen timeout in ms (300000 = 5 minutes)
#define LUX_CHANGE_THRESHOLD 5.0       // Lux delta required to wake screen
#define ALERT_REPEAT_INTERVAL 300000   // Ms between repeated humidity alerts (5 min)
#define ALERT_RESTART_INTERVAL 3600000 // Ms before alert cycle restarts after /hold (1 hour)
#define SENSOR_READ_INTERVAL 2000      // Ms between DHT/BMP sensor reads; DHT11 should not be polled rapidly
#define DISPLAY_REFRESH_INTERVAL 500   // Ms between OLED redraws; avoids needless I2C work

// ============================================================
// RTC MEMORY — persists across watchdog resets and WiFi drops
// but clears on actual power loss. Used to track online notification
// state, the last processed Telegram update ID, and alert suppression
// state so commands do not replay after a reset.
// ============================================================
struct RTCData {
  uint32_t magic;
  bool onlineSent;
  long lastUpdateId;
  bool holdActive;
  bool stopSuppressed;
  bool humidityWasLow;
};
RTCData rtcData;
#define RTC_MAGIC 0xA7105027

// ============================================================
// SENSOR AND DISPLAY OBJECTS
// ============================================================
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP085 bmp;
BH1750 lightMeter;

// OLED setup — SW_I2C (software I2C) is required because the onboard OLED
// on the ideaspark board uses GPIO14 (SCL) and GPIO12 (SDA), which conflicts
// with the hardware I2C bus on D1/D2 used by BMP180 and BH1750.
// Using SW_I2C gives the OLED its own separate bus to avoid conflicts.
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, 14, 12, U8X8_PIN_NONE);

// ============================================================
// TELEGRAM BOT SETUP
// BOT_TOKEN and CHAT_ID come from secrets.h
// WiFiClientSecure handles HTTPS — setInsecure() skips cert validation
// which is necessary on ESP8266 due to limited SSL support
// ============================================================
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// ============================================================
// DISPLAY STATE
// Labels cycle every 4 seconds across the four sensor readings
// ============================================================
const char* labels[] = {"TEMPERATURE", "HUMIDITY", "LUX", "BAROMETER"};
int labelIndex = 0;            // Which label/reading is currently shown
unsigned long lastSwitch = 0;  // Timestamp of last label switch

// ============================================================
// TIMING VARIABLES
// All use millis() — non-blocking, no delays in main loop
// ============================================================
unsigned long lastBotCheck = 0;      // Last time bot was polled for messages
unsigned long screenTimeout = 0;     // Timestamp of last screen wake event
unsigned long lastAlertSent = 0;     // Timestamp of last humidity alert sent
unsigned long ackReceivedTime = 0;   // Timestamp when /hold was received
unsigned long lastSensorRead = 0;     // Last DHT/BMP sensor sample time
unsigned long lastDisplayDraw = 0;    // Last OLED redraw time

// ============================================================
// STATE FLAGS
// Track the current state of the screen, bot, and alert system
// ============================================================
bool screenOn = true;          // Whether the OLED is currently on
bool waitingForAck = false;    // Alert sent, waiting for /hold or /stop response
bool ackReceived = false;      // /hold received, in 1 hour hold period
bool stopSuppressed = false;   // /stop received, alerts fully suppressed
bool humidityWasLow = false;   // Tracks if humidity dipped below threshold after /stop
                               // Required to detect a full natural cycle before re-alerting
bool botOnlineSent = false;    // Whether the startup online message has been sent
                               // Prevents sending it multiple times if WiFi reconnects
bool dhtTempValid = false;     // Last DHT temperature read succeeded
bool dhtHumidityValid = false; // Last DHT humidity read succeeded
bool bmpValid = false;         // BMP180 initialized successfully
bool bh1750Valid = false;      // BH1750 initialized successfully
bool pressureValid = false;    // Last pressure read succeeded
bool luxValid = false;         // Last lux read succeeded

// ============================================================
// SENSOR READINGS
// Global floats updated each loop cycle
// ============================================================
float tempF = 0;
float humidity = 0;
float pressure = 0;
float lux = 0;

// Lux debounce variables — prevent screen flickering from sensor noise
// lastLux: previous valid reading used for change comparison
// luxSampleInitialized: prevents a false wake from the first sample after boot
// lastLuxSample: timestamp to enforce 1 second between samples
float lastLux = 0;
bool luxSampleInitialized = false;
unsigned long lastLuxSample = 0;

// Last processed Telegram update ID. Stored in RTC memory so old /hold
// or /stop commands are not replayed after watchdog resets or reconnects.
long lastProcessedUpdateId = 0;

// ============================================================
// TELEGRAM SEND HELPERS
// All Telegram sends go through one WiFi check and one success check.
// This prevents offline attempts from blocking local display behavior and
// prevents failed sends from being treated as delivered alerts.
// ============================================================
bool telegramAvailable() {
  return WiFi.status() == WL_CONNECTED;
}

bool sendTelegramMessage(const String& message) {
  if (!telegramAvailable()) {
    return false;
  }

  ESP.wdtDisable(); // Disable hardware watchdog — SSL can take too long otherwise
  bool ok = bot.sendMessage(CHAT_ID, message, "");
  ESP.wdtEnable(0); // Re-enable hardware watchdog

  return ok;
}

void saveRtcData() {
  system_rtc_mem_write(64, &rtcData, sizeof(rtcData));
}

void saveAlertState() {
  rtcData.holdActive = ackReceived;
  rtcData.stopSuppressed = stopSuppressed;
  rtcData.humidityWasLow = humidityWasLow;
  saveRtcData();
}

void rememberTelegramUpdate(long updateId) {
  if (updateId <= lastProcessedUpdateId) {
    return;
  }

  lastProcessedUpdateId = updateId;
  bot.last_message_received = updateId;
  rtcData.lastUpdateId = updateId;
  saveRtcData();
}

void appendReadingOrError(String& msg, const __FlashStringHelper* label,
                          float value, uint8_t precision,
                          const char* unit, bool valid) {
  msg += label;
  if (valid) {
    msg += String(value, precision);
    msg += ' ';
    msg += unit;
  } else {
    msg += F("ERR ");
    msg += unit;
  }
  msg += '\n';
}

// ============================================================
// SEND HUMIDITY ALERT
// Fires when humidity crosses threshold, repeats every 5 min
// until /hold or /stop is received. Returns true only if Telegram
// accepted the message.
// ============================================================
bool sendHumidityAlert() {
  if (!dhtHumidityValid) {
    return false;
  }

String alert;
alert.reserve(360);
alert += F("⚠️ ");
alert += DEVICE_NAME;
alert += F(" humidity at ");
alert += String(humidity, 1);
alert += F("%\n\n");
alert += F("Reply /hold ");
alert += DEVICE_NAME;
alert += F(" to pause alerts for 1 hour.\n");
alert += F("Reply /stop ");
alert += DEVICE_NAME;
alert += F(" to suppress alerts until humidity drops and recovers naturally.\n");
alert += F("Reply /status to review sensors.\n");
alert += F("Reply /hold or /stop to suppress alerts ON ALL DEVICES.");

  bool ok = sendTelegramMessage(alert);
  if (ok) {
    lastAlertSent = millis(); // Record when this alert was actually sent
    waitingForAck = true;     // Mark that we're waiting for a response
  }

  return ok;
}

// ============================================================
// SEND STATUS
// Returns all available sensor readings. Triggered by
// /status or /status [device] command.
// ============================================================
void sendStatus() {
  String msg;
  msg.reserve(128);
  msg += DEVICE_NAME;
  msg += F(" status:\n");
  appendReadingOrError(msg, F("Temp: "), tempF, 1, "F", dhtTempValid);
  appendReadingOrError(msg, F("Humidity: "), humidity, 1, "%", dhtHumidityValid);
  appendReadingOrError(msg, F("Pressure: "), pressure, 0, "hPa", pressureValid);
  appendReadingOrError(msg, F("Light: "), lux, 1, "lx", luxValid);
  sendTelegramMessage(msg);
}

// ============================================================
// HANDLE INCOMING MESSAGES
// Parses command and optional device target from message text.
// Commands with no target broadcast to all devices.
// Commands with a target only affect the matching device.
// Example: "/hold" affects all, "/hold library" affects only library.
// ============================================================
void handleMessages(int numMessages) {
  Serial.print("Got ");
  Serial.print(numMessages);
  Serial.println(" messages");

  for (int i = 0; i < numMessages; i++) {
    long updateId = bot.messages[i].update_id;

    // Avoid replaying already handled Telegram updates. This matters after
    // watchdog resets because bot.last_message_received normally lives only
    // in RAM.
    if (updateId <= lastProcessedUpdateId) {
      Serial.print("Skipping duplicate update: ");
      Serial.println(updateId);
      continue;
    }

    String text = bot.messages[i].text;
    text.trim();
    Serial.print("Message: ");
    Serial.println(text);

    // Split message into command and optional target device name
    // "/hold library" → command = "/hold", target = "library"
    // "/hold" → command = "/hold", target = ""
    String command = text;
    String target = "";
    int spaceIndex = text.indexOf(' ');
    if (spaceIndex != -1) {
      command = text.substring(0, spaceIndex);
      target = text.substring(spaceIndex + 1);
      target.trim();
      target.toLowerCase(); // Normalize to lowercase for comparison
    }
    command.toLowerCase();

    // isForMe is true if no target was specified (broadcast)
    // or if the target matches this device's DEVICE_NAME
    bool isForMe = (target == "" || target == DEVICE_NAME);

    // /status — return all sensor readings
    if (command == "/status" && isForMe) {
      rememberTelegramUpdate(updateId);
      sendStatus();
      continue;
    }

    // /hold — pause alerts for 1 hour then restart cycle
    if (command == "/hold" && isForMe) {
      waitingForAck = false;
      ackReceived = true;
      stopSuppressed = false;
      humidityWasLow = false;
      ackReceivedTime = millis(); // Start the 1 hour hold timer
      saveAlertState();
      rememberTelegramUpdate(updateId);
      String reply;
      reply.reserve(64);
      reply += DEVICE_NAME;
      reply += F(" humidity alerts paused for 1 hour.");
      sendTelegramMessage(reply);
      continue;
    }

    // /stop — fully suppress alerts until humidity naturally cycles
    // Humidity must drop below threshold AND rise above it again
    // before alerts will fire again
    if (command == "/stop" && isForMe) {
      stopSuppressed = true;
      waitingForAck = false;
      ackReceived = false;
      humidityWasLow = false; // Reset the natural cycle tracker
      saveAlertState();
      rememberTelegramUpdate(updateId);
      String reply;
      reply.reserve(96);
      reply += DEVICE_NAME;
      reply += F(" humidity alerts suppressed until humidity drops and rises again.");
      sendTelegramMessage(reply);
      continue;
    }

    // Unknown or non-targeted commands are still consumed so they do not
    // replay forever after a reset.
    rememberTelegramUpdate(updateId);
  }
}

// ============================================================
// HUMIDITY ALERT STATE MACHINE
// Called every loop cycle. Manages the full alert lifecycle:
// fresh alert → repeat every 5 min → /hold pauses 1 hour →
// restart after 1 hour → /stop suppresses until natural cycle.
// ============================================================
void handleHumidityLogic() {
  if (!dhtHumidityValid) {
    return;
  }

  if (humidity > HUMIDITY_THRESHOLD) {

    // /stop was sent — do nothing until humidity naturally cycles
    if (stopSuppressed) {
      return;
    }

    // No Telegram work while WiFi is unavailable. The device remains a local
    // monitor and will alert when WiFi returns if humidity is still high.
    if (!telegramAvailable()) {
      return;
    }

    // /hold was received — wait 1 hour then restart the alert cycle
    if (ackReceived) {
      if (millis() - ackReceivedTime >= ALERT_RESTART_INTERVAL) {
        ackReceived = false;    // Clear hold state
        saveAlertState();
        sendHumidityAlert();    // Restart the cycle with a fresh alert
      }
      return;
    }

    // Alert was sent, waiting for response — resend every 5 minutes
    if (waitingForAck) {
      if (millis() - lastAlertSent >= ALERT_REPEAT_INTERVAL) {
        sendHumidityAlert();
      }
      return;
    }

    // No active state — this is a fresh trigger, send the first alert
    sendHumidityAlert();

  } else {
    // Humidity dropped below threshold

    // If /stop is active, track that humidity went low
    // This is required before alerts can restart after /stop
    if (stopSuppressed && !humidityWasLow) {
      humidityWasLow = true;
      saveAlertState();
    }

    // Clear alert state — fresh start when humidity rises again
    waitingForAck = false;
    if (ackReceived) {
      ackReceived = false;
      saveAlertState();
    }
  }
}

// ============================================================
// SETUP
// Runs once on boot. Initializes all sensors, display, and WiFi.
// WiFi connects in the background — sensors and display start
// immediately without waiting for a connection.
// ============================================================
void setup() {
  Serial.begin(115200);

  Wire.begin();
  dht.begin();
  bmpValid = bmp.begin();
  bh1750Valid = lightMeter.begin();
  u8g2.begin();
  u8g2.setPowerSave(0);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.begin(ssid, password);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  client.setInsecure();
  client.setBufferSizes(512, 512); // Smaller BearSSL buffers reduce heap pressure during Telegram HTTPS calls
  client.setTimeout(6000);

  // Read RTC memory to check if online message was already sent
  // this power cycle. If magic value doesn't match, this is a
  // fresh power-on and we initialize the struct cleanly.
  system_rtc_mem_read(64, &rtcData, sizeof(rtcData));
  if (rtcData.magic != RTC_MAGIC) {
    rtcData.magic = RTC_MAGIC;
    rtcData.onlineSent = false;
    rtcData.lastUpdateId = 0;
    rtcData.holdActive = false;
    rtcData.stopSuppressed = false;
    rtcData.humidityWasLow = false;
  }
  botOnlineSent = rtcData.onlineSent;
  lastProcessedUpdateId = rtcData.lastUpdateId;
  bot.last_message_received = rtcData.lastUpdateId;
  ackReceived = rtcData.holdActive;
  if (ackReceived) {
    ackReceivedTime = millis(); // Conservative reset behavior: restart remaining hold window
  }
  stopSuppressed = rtcData.stopSuppressed;
  humidityWasLow = rtcData.humidityWasLow;

  screenTimeout = millis();
  lastSwitch = millis();
}

// ============================================================
// MAIN LOOP
// Runs continuously. Non-blocking — uses millis() for all timing.
// Order of operations each cycle:
// 1. Cycle display label every 4 seconds
// 2. Read all sensors
// 3. Check lux for screen wake (sampled once per second)
// 4. Check screen timeout
// 5. Check /stop natural cycle reset
// 6. Run humidity alert state machine
// 7. Send online message once WiFi connects
// 8. Poll Telegram for new messages every 10 seconds
// 9. Build and render display
// ============================================================
void loop() {

  // Step 1 — Advance to next sensor label every 4 seconds
  if (millis() - lastSwitch > 4000) {
    labelIndex = (labelIndex + 1) % 4; // Wraps 0→1→2→3→0
    lastSwitch = millis();
  }

  // Step 2 — Read DHT/BMP sensors on an interval
  // DHT11 is slow and should not be polled every loop. The display keeps
  // showing the most recent valid reading between samples.
  if (lastSensorRead == 0 || millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();

    float tempC = dht.readTemperature();
    float humidityReading = dht.readHumidity();

    if (!isnan(tempC)) {
      tempF = (tempC * 9.0 / 5.0) + 32.0; // Celsius to Fahrenheit
      dhtTempValid = true;
    } else {
      dhtTempValid = false;
    }

    if (!isnan(humidityReading)) {
      humidity = humidityReading;
      dhtHumidityValid = true;
    } else {
      dhtHumidityValid = false;
    }

    if (bmpValid) {
      pressure = bmp.readPressure() / 100.0; // Pa to hPa
      pressureValid = true;
    } else {
      pressureValid = false;
    }
  }

  // Step 3 — Lux sampling and screen wake logic
  // Sampled once per second to prevent sensor noise from
  // constantly triggering screen wakes and flickering. Consecutive
  // valid readings are compared directly.
  if (millis() - lastLuxSample > 1000) {
    if (bh1750Valid) {
      float currentLux = lightMeter.readLightLevel();

      if (!isnan(currentLux) && currentLux >= 0.0) {
        lux = currentLux; // Update global for display and status
        luxValid = true;

        if (luxSampleInitialized) {
          // Wake condition 1: significant lux change (lights on or off)
          bool bigChange = fabs(currentLux - lastLux) >= LUX_CHANGE_THRESHOLD;

          // Wake condition 2: deliberate cover gesture
          // Sensor drops from above 1 lux to below 1 lux
          // Works even in a dim room where absolute change would be small
          bool coverDrop = (currentLux < 1.0 && lastLux >= 1.0);

          if (bigChange || coverDrop) {
            u8g2.setPowerSave(0); // Wake display
            screenOn = true;
            lastDisplayDraw = 0; // Force redraw soon after wake
            screenTimeout = millis(); // Reset the 5 minute timer
          }
        } else {
          luxSampleInitialized = true;
        }

        lastLux = currentLux;
      } else {
        luxValid = false;
      }
    } else {
      luxValid = false;
    }

    lastLuxSample = millis();
  }

  // Step 4 — Turn screen off after 5 minutes of no wake trigger
  if (screenOn && millis() - screenTimeout >= SCREEN_DURATION) {
    u8g2.setPowerSave(1); // Sleep display
    screenOn = false;
  }

  // Step 5 — /stop natural cycle reset check
  // After /stop, alerts only restart when humidity has dropped
  // below threshold (humidityWasLow = true) AND risen above it again
  // This simulates: problem fixed → room dried out → problem returned
  if (stopSuppressed && humidityWasLow && dhtHumidityValid && humidity > HUMIDITY_THRESHOLD) {
    stopSuppressed = false;
    humidityWasLow = false;
    waitingForAck = false;
    ackReceived = false;
    saveAlertState();
    // Let the normal state machine send the fresh alert if WiFi is available.
  }

  // Step 6 — Run humidity alert state machine
  handleHumidityLogic();

  // Step 7 - Send online message once per power cycle using RTC memory
  // WiFi drops and watchdog resets will not trigger this again
  // Only an actual power loss clears RTC memory
  if (!botOnlineSent && telegramAvailable()) {
    String onlineMsg;
    onlineMsg.reserve(32);
    onlineMsg += DEVICE_NAME;
    onlineMsg += F(" online.");
    bool onlineSent = sendTelegramMessage(onlineMsg);
    if (onlineSent) {
      botOnlineSent = true;
      rtcData.onlineSent = true;
      saveRtcData();
    }
  }

  // Step 8 — Poll Telegram for new messages every 10 seconds
  // wdtDisable/Enable wraps the entire block because getUpdates
  // makes an SSL call that can exceed the hardware watchdog timeout
  if (telegramAvailable() && millis() - lastBotCheck > 10000) {
    ESP.wdtDisable();
    int numMessages = bot.getUpdates(lastProcessedUpdateId + 1);
    ESP.wdtEnable(0);

    // Drain all queued messages before moving on
    while (numMessages) {
      handleMessages(numMessages);

      ESP.wdtDisable();
      numMessages = bot.getUpdates(lastProcessedUpdateId + 1);
      ESP.wdtEnable(0);
    }

    lastBotCheck = millis();
  }

  // Step 9 — Build value string for current label
  char valueStr[24];
  switch (labelIndex) {
    case 0:
      if (dhtTempValid) { dtostrf(tempF, 4, 1, valueStr); strcat(valueStr, " F"); }
      else { strcpy(valueStr, "ERR F"); }
      break;
    case 1:
      if (dhtHumidityValid) { dtostrf(humidity, 4, 1, valueStr); strcat(valueStr, " %"); }
      else { strcpy(valueStr, "ERR %"); }
      break;
    case 2:
      if (luxValid) { dtostrf(lux, 4, 1, valueStr); strcat(valueStr, " lx"); }
      else { strcpy(valueStr, "ERR lx"); }
      break;
    case 3:
      if (pressureValid) { dtostrf(pressure, 5, 0, valueStr); strcat(valueStr, " hPa"); }
      else { strcpy(valueStr, "ERR hPa"); }
      break;
  }

  bool connected = (WiFi.status() == WL_CONNECTED);

  // Step 9 cont. — Render display only if screen is on
  // Top yellow strip (16px): WiFi icon + sensor label
  // Bottom blue area (48px): Large sensor value
  if (screenOn && (lastDisplayDraw == 0 || millis() - lastDisplayDraw >= DISPLAY_REFRESH_INTERVAL)) {
    lastDisplayDraw = millis();
    u8g2.clearBuffer();

    // WiFi icon — glyph 0x0051 = connected, 0x0050 = disconnected
    u8g2.setFont(u8g2_font_open_iconic_www_1x_t);
    u8g2.drawGlyph(0, 13, connected ? 0x0051 : 0x0050);

    // Sensor label in small font next to WiFi icon
    u8g2.setFont(u8g2_font_7x13_tr);
    u8g2.drawStr(14, 12, labels[labelIndex]);

    // Large sensor value in bottom blue area
    u8g2.setFont(u8g2_font_fub20_tr);
    u8g2.drawStr(0, 55, valueStr);

    u8g2.sendBuffer(); // Push buffer to display
  }

  delay(100); // Short yield — keeps loop responsive without blocking
}

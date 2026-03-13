// ============================================================
// AtmoSensor — ESP8266 Environment Monitor
// Tracks temperature, humidity, pressure, and light.
// Displays readings on onboard OLED, sends Telegram alerts.
// Multiple devices share one bot via DEVICE_NAME targeting.
// ============================================================

#include <Arduino.h>
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

// ============================================================
// RTC MEMORY — persists across watchdog resets and WiFi drops
// but clears on actual power loss. Used to track whether the
// online message has already been sent this power cycle.
// ============================================================
struct RTCData {
  uint32_t magic;
  bool onlineSent;
};
RTCData rtcData;
#define RTC_MAGIC 0xDEADBEEF

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

// ============================================================
// SENSOR READINGS
// Global floats updated each loop cycle
// ============================================================
float tempF = 0;
float humidity = 0;
float pressure = 0;
float lux = 0;

// Lux debounce variables — prevent screen flickering from sensor noise
// prevLux: reading from two samples ago (used for change comparison)
// lastLux: reading from one sample ago
// lastLuxSample: timestamp to enforce 1 second between samples
float lastLux = 0;
float prevLux = 0;
unsigned long lastLuxSample = 0;

// ============================================================
// SEND HUMIDITY ALERT
// Fires when humidity crosses threshold, repeats every 5 min
// until /hold or /stop is received
// ESP.wdtDisable/Enable wraps all Telegram calls because the
// SSL handshake can take longer than the hardware watchdog allows
// ============================================================
void sendHumidityAlert() {
  ESP.wdtDisable(); // Disable hardware watchdog — SSL takes too long otherwise
  String alert = "⚠️ " + String(DEVICE_NAME) + " humidity at " + String(humidity, 1) + "%\n\n";
  alert += "Reply /hold " + String(DEVICE_NAME) + " to pause alerts for 1 hour.\n";
  alert += "Reply /stop " + String(DEVICE_NAME) + " to suppress alerts until humidity drops and recovers naturally.\n";
  alert += "Reply /hold or /stop to suppress alerts ON ALL DEVICES until humidity drops and recovers naturally.";
  bot.sendMessage(CHAT_ID, alert, "");
  ESP.wdtEnable(0); // Re-enable hardware watchdog
  lastAlertSent = millis(); // Record when this alert was sent for repeat timer
  waitingForAck = true;     // Mark that we're waiting for a response
}

// ============================================================
// SEND STATUS
// Returns all four sensor readings plus WiFi state
// Triggered by /status or /status [device] command
// ============================================================
void sendStatus() {
  ESP.wdtDisable();
  String msg = String(DEVICE_NAME) + " status:\n";
  msg += "Temp: " + String(tempF, 1) + " F\n";
  msg += "Humidity: " + String(humidity, 1) + " %\n";
  msg += "Pressure: " + String(pressure, 0) + " hPa\n";
  msg += "Light: " + String(lux, 1) + " lx\n";
  msg += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
  bot.sendMessage(CHAT_ID, msg, "");
  ESP.wdtEnable(0);
}

// ============================================================
// HANDLE INCOMING MESSAGES
// Parses command and optional device target from message text
// Commands with no target broadcast to all devices
// Commands with a target only affect the matching device
// Example: "/hold" affects all, "/hold library" affects only library
// ============================================================
void handleMessages(int numMessages) {
  Serial.print("Got ");
  Serial.print(numMessages);
  Serial.println(" messages");

  for (int i = 0; i < numMessages; i++) {
    String text = bot.messages[i].text;
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
      target.toLowerCase(); // Normalize to lowercase for comparison
    }

    // isForMe is true if no target was specified (broadcast)
    // or if the target matches this device's DEVICE_NAME
    bool isForMe = (target == "" || target == DEVICE_NAME);

    // /status or /library — return all sensor readings
    if ((command == "/status" || command == "/library") && isForMe) {
      sendStatus();
    }

    // /hold — pause alerts for 1 hour then restart cycle
    if (command == "/hold" && isForMe) {
      waitingForAck = false;
      ackReceived = true;
      ackReceivedTime = millis(); // Start the 1 hour hold timer
      bot.sendMessage(CHAT_ID, String(DEVICE_NAME) + " humidity alerts paused for 1 hour.", "");
    }

    // /stop — fully suppress alerts until humidity naturally cycles
    // Humidity must drop below threshold AND rise above it again
    // before alerts will fire again
    if (command == "/stop" && isForMe) {
      stopSuppressed = true;
      waitingForAck = false;
      ackReceived = false;
      humidityWasLow = false; // Reset the natural cycle tracker
      bot.sendMessage(CHAT_ID, String(DEVICE_NAME) + " humidity alerts suppressed until humidity drops and rises again.", "");
      continue;
    }
  }
}

// ============================================================
// HUMIDITY ALERT STATE MACHINE
// Called every loop cycle. Manages the full alert lifecycle:
// fresh alert → repeat every 5 min → /hold pauses 1 hour →
// restart after 1 hour → /stop suppresses until natural cycle
// ============================================================
void handleHumidityLogic() {
  if (humidity > HUMIDITY_THRESHOLD) {

    // /stop was sent — do nothing until humidity naturally cycles
    if (stopSuppressed) {
      return;
    }

    // /hold was received — wait 1 hour then restart the alert cycle
    if (ackReceived) {
      if (millis() - ackReceivedTime >= ALERT_RESTART_INTERVAL) {
        ackReceived = false;    // Clear hold state
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
    if (stopSuppressed) {
      humidityWasLow = true;
    }

    // Clear alert state — fresh start when humidity rises again
    waitingForAck = false;
    ackReceived = false;
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
  bmp.begin();
  lightMeter.begin();
  u8g2.begin();
  u8g2.setPowerSave(0);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.begin(ssid, password);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  client.setInsecure();
  client.setTimeout(6000);

  // Read RTC memory to check if online message was already sent
  // this power cycle. If magic value doesn't match, this is a
  // fresh power-on and we initialize the struct cleanly.
  system_rtc_mem_read(64, &rtcData, sizeof(rtcData));
  if (rtcData.magic != RTC_MAGIC) {
    rtcData.magic = RTC_MAGIC;
    rtcData.onlineSent = false;
  }
  botOnlineSent = rtcData.onlineSent;

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

  // Step 2 — Read sensors
  tempF = (dht.readTemperature() * 9.0 / 5.0) + 32.0; // Celsius to Fahrenheit
  humidity = dht.readHumidity();
  pressure = bmp.readPressure() / 100.0; // Pa to hPa

  // Step 3 — Lux sampling and screen wake logic
  // Sampled once per second to prevent sensor noise from
  // constantly triggering screen wakes and flickering
  // Two consecutive readings are compared (prevLux vs currentLux)
  // to require a real sustained change rather than a momentary spike
  if (millis() - lastLuxSample > 1000) {
    float currentLux = lightMeter.readLightLevel();
    lux = currentLux; // Update global for display and status

    // Wake condition 1: significant lux change (lights on or off)
    bool bigChange = abs(currentLux - prevLux) >= LUX_CHANGE_THRESHOLD;

    // Wake condition 2: deliberate cover gesture
    // Sensor drops from above 1 lux to below 1 lux
    // Works even in a dim room where absolute change would be small
    bool coverDrop = (currentLux < 1.0 && prevLux >= 1.0);

    if (bigChange || coverDrop) {
      u8g2.setPowerSave(0); // Wake display
      screenOn = true;
      screenTimeout = millis(); // Reset the 5 minute timer
    }

    // Shift readings forward for next comparison
    prevLux = lastLux;
    lastLux = currentLux;
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
  if (stopSuppressed && humidityWasLow && humidity > HUMIDITY_THRESHOLD) {
    stopSuppressed = false;
    humidityWasLow = false;
    sendHumidityAlert(); // Fresh alert — new problem detected
    return;
  }

  // Step 6 — Run humidity alert state machine
  handleHumidityLogic();

  // Step 7 - Send online message once per power cycle using RTC memory
  // WiFi drops and watchdog resets will not trigger this again
  // Only an actual power loss clears RTC memory
  if (!botOnlineSent && WiFi.status() == WL_CONNECTED) {
    ESP.wdtDisable();
    bot.sendMessage(CHAT_ID, String(DEVICE_NAME) + " online.", "");
    ESP.wdtEnable(0);
    botOnlineSent = true;
    rtcData.onlineSent = true;
    system_rtc_mem_write(64, &rtcData, sizeof(rtcData));
    waitingForAck = false;
    ackReceived = false;
    lastAlertSent = 0;
  }

  // Step 8 — Poll Telegram for new messages every 10 seconds
  // wdtDisable/Enable wraps the entire block because getUpdates
  // makes an SSL call that can exceed the hardware watchdog timeout
  if (millis() - lastBotCheck > 10000) {
    ESP.wdtDisable();
    int numMessages = bot.getUpdates(bot.last_message_received + 1);
    // Drain all queued messages before moving on
    while (numMessages) {
      handleMessages(numMessages);
      numMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    ESP.wdtEnable(0);
    lastBotCheck = millis();
  }

  // Step 9 — Build value string for current label
  char valueStr[16];
  switch (labelIndex) {
    case 0: dtostrf(tempF, 4, 1, valueStr); strcat(valueStr, " F"); break;
    case 1: dtostrf(humidity, 4, 1, valueStr); strcat(valueStr, " %"); break;
    case 2: dtostrf(lux, 4, 1, valueStr); strcat(valueStr, " lx"); break;
    case 3: dtostrf(pressure, 5, 0, valueStr); strcat(valueStr, " hPa"); break;
  }

  bool connected = (WiFi.status() == WL_CONNECTED);

  // Step 9 cont. — Render display only if screen is on
  // Top yellow strip (16px): WiFi icon + sensor label
  // Bottom blue area (48px): Large sensor value
  if (screenOn) {
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
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Adafruit_BMP085.h>
#include <BH1750.h>
#include "secrets.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

#define DHTPIN D7
#define DHTTYPE DHT11
#define HUMIDITY_THRESHOLD 50.0
#define SCREEN_DURATION 300000
#define LUX_CHANGE_THRESHOLD 5.0
#define ALERT_REPEAT_INTERVAL 300000   // 5 minutes
#define ALERT_RESTART_INTERVAL 3600000 // 1 hour

DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP085 bmp;
BH1750 lightMeter;
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, 14, 12, U8X8_PIN_NONE);

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

const char* labels[] = {"TEMPERATURE", "HUMIDITY", "LUX", "BAROMETER"};
int labelIndex = 0;
unsigned long lastSwitch = 0;
unsigned long lastBotCheck = 0;
unsigned long screenTimeout = 0;
unsigned long lastAlertSent = 0;
unsigned long ackReceivedTime = 0;
bool screenOn = true;
bool waitingForAck = false;
bool ackReceived = false;
bool stopSuppressed = false;
bool humidityWasLow = false;

float tempF = 0;
float humidity = 0;
float pressure = 0;
float lux = 0;
float lastLux = 0;

void sendHumidityAlert() {
  String alert = "⚠️ " + String(DEVICE_NAME) + " humidity at " + String(humidity, 1) + "%\n\n";
  alert += "Reply with anything to acknowledge and pause alerts for 1 hour.\n";
  alert += "Reply /stop " + String(DEVICE_NAME) + " to suppress alerts until humidity drops and recovers naturally.";
  bot.sendMessage(CHAT_ID, alert, "");
  lastAlertSent = millis();
  waitingForAck = true;
}

void sendStatus() {
  String msg = String(DEVICE_NAME) + " status:\n";
  msg += "Temp: " + String(tempF, 1) + " F\n";
  msg += "Humidity: " + String(humidity, 1) + " %\n";
  msg += "Pressure: " + String(pressure, 0) + " hPa\n";
  msg += "Light: " + String(lux, 1) + " lx";
  bot.sendMessage(CHAT_ID, msg, "");
}

void handleMessages(int numMessages) {
  Serial.print("Got ");
  Serial.print(numMessages);
  Serial.println(" messages");
  for (int i = 0; i < numMessages; i++) {
    String text = bot.messages[i].text;
    Serial.print("Message: ");
    Serial.println(text);

    // Parse command and optional device target
    String command = text;
    String target = "";
    int spaceIndex = text.indexOf(' ');
    if (spaceIndex != -1) {
      command = text.substring(0, spaceIndex);
      target = text.substring(spaceIndex + 1);
      target.toLowerCase();
    }

    bool isForMe = (target == "" || target == DEVICE_NAME);

    if ((command == "/status" || command == "/library") && isForMe) {
      sendStatus();
    }

    if (command == "/stop" && isForMe) {
      stopSuppressed = true;
      waitingForAck = false;
      ackReceived = false;
      humidityWasLow = false;
      bot.sendMessage(CHAT_ID, "Humidity alerts suppressed until humidity drops and rises again.", "");
      continue;
    }

    // Any message counts as an ack if we're waiting for one
    if (waitingForAck) {
      waitingForAck = false;
      ackReceived = true;
      ackReceivedTime = millis();
    }
  }
}

void handleHumidityLogic() {
  if (humidity > HUMIDITY_THRESHOLD) {
    // /stop was sent — wait for a full natural cycle before alerting again
    if (stopSuppressed) {
      return;
    }

    // Ack received — wait 1 hour then restart cycle
    if (ackReceived) {
      if (millis() - ackReceivedTime >= ALERT_RESTART_INTERVAL) {
        ackReceived = false;
        sendHumidityAlert();
      }
      return;
    }

    // Waiting for ack — resend every 5 minutes
    if (waitingForAck) {
      if (millis() - lastAlertSent >= ALERT_REPEAT_INTERVAL) {
        sendHumidityAlert();
      }
      return;
    }

    // Fresh trigger — send first alert
    sendHumidityAlert();

  } else {
    // Humidity is below threshold
    if (stopSuppressed) {
      humidityWasLow = true;
    }
    waitingForAck = false;
    ackReceived = false;
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  dht.begin();
  bmp.begin();
  lightMeter.begin();
  u8g2.begin();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("DEVICE_NAME");
  WiFi.begin(ssid, password);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13_tr);
  u8g2.drawStr(0, 12, "Connecting...");
  u8g2.sendBuffer();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  client.setInsecure();
  client.setTimeout(15000);
  bot.sendMessage(CHAT_ID, String(DEVICE_NAME) + " online.", "");
  screenTimeout = millis();
  lastSwitch = millis();
}

void loop() {
  if (millis() - lastSwitch > 4000) {
    labelIndex = (labelIndex + 1) % 4;
    lastSwitch = millis();
  }

  tempF = (dht.readTemperature() * 9.0 / 5.0) + 32.0;
  humidity = dht.readHumidity();
  pressure = bmp.readPressure() / 100.0;
  lux = lightMeter.readLightLevel();

  if (abs(lux - lastLux) >= LUX_CHANGE_THRESHOLD || (lux < 1.0 && lastLux >= 1.0)) {
    u8g2.setPowerSave(0);
    screenOn = true;
    screenTimeout = millis();
  }
  lastLux = lux;

  if (screenOn && millis() - screenTimeout >= SCREEN_DURATION) {
    u8g2.setPowerSave(1);
    screenOn = false;
  }

  // Handle /stop suppression reset — humidity must have dipped below AND come back up
  if (stopSuppressed && humidityWasLow && humidity > HUMIDITY_THRESHOLD) {
    stopSuppressed = false;
    humidityWasLow = false;
    sendHumidityAlert();
    return;
  }

  handleHumidityLogic();

  if (millis() - lastBotCheck > 10000) {
    int numMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numMessages) {
      handleMessages(numMessages);
      numMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastBotCheck = millis();
  }

  char valueStr[16];
  switch (labelIndex) {
    case 0: dtostrf(tempF, 4, 1, valueStr); strcat(valueStr, " F"); break;
    case 1: dtostrf(humidity, 4, 1, valueStr); strcat(valueStr, " %"); break;
    case 2: dtostrf(lux, 4, 1, valueStr); strcat(valueStr, " lx"); break;
    case 3: dtostrf(pressure, 5, 0, valueStr); strcat(valueStr, " hPa"); break;
  }

  bool connected = (WiFi.status() == WL_CONNECTED);

  if (screenOn) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_open_iconic_www_1x_t);
    u8g2.drawGlyph(0, 13, connected ? 0x0051 : 0x0050);
    u8g2.setFont(u8g2_font_7x13_tr);
    u8g2.drawStr(14, 12, labels[labelIndex]);
    u8g2.setFont(u8g2_font_fub20_tr);
    u8g2.drawStr(0, 55, valueStr);
    u8g2.sendBuffer();
  }

  delay(100);
}
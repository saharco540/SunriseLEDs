#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <TimeAlarms.h>

#include <ArduinoOTA.h>
#include <PubSubClient.h>

#include "config.h"

#ifdef ENABLE_WEB_SERVER
#include <ESP8266WebServer.h>
ESP8266WebServer server(80);
#endif

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

#ifdef ENABLE_MQTT
WiFiClient espClient;
PubSubClient client(espClient);
#endif

#ifdef ENABLE_TELEGRAM_BOT
WiFiClientSecure secured_client;
UniversalTelegramBot bot(token, secured_client);
#endif


// Variables to store the set time
int targetHour = -1;
int targetMinute = -1;
int adjustedHour = -1;
int adjustedMinute = -1;
bool isTimeSet = false;
bool timerActive = false;
unsigned long lastBrightnessIncrease = 0;
unsigned long lastTimeBotRan;
int maxBrightness = MAX_BRIGHTNESS;
int brightnessDuration = SUNRISE_DURATION_MINUTES;
int currentBrightness = 0;
int messageCheckInterval = 20000; // Check for new messages every ... seconds
int lastUpdateId = 0;

// Forward declarations
void updateTimeOffset();
bool isDST(long epochTime);

void brightnessIncrease();
void handleNewMessages(int numNewMessages);
void startBrightnessIncrease();
void actByMessage(String message);
void sendMessageToTelegram(String message);
void sendMessageToMQQT(String message);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnect();
void sendMessage(String message, boolean isMQQT, boolean isTelegram);
String getMainHTML();
void handleSliderChange();
void handleSetSunrise();

AlarmId brightnessIncreaseRoutineAlarm;
AlarmId brightnessIncrementTimer;

void setup() {

  pinMode(LED_PIN, OUTPUT);
  analogWriteRange(1023); // Set PWM range to 1023
  Serial.begin(115200);

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi connected");
  // Print IP
  Serial.println(WiFi.localIP());

  #ifdef ENABLE_TELEGRAM_BOT
  secured_client.setInsecure(); 
  #endif

  // Update time from ntp, try again if failed
  while (!timeClient.update()) {
    Serial.println("Failed to update time from NTP server");
    delay(2000);
  }

 
  // ----------------- Time -----------------

  timeClient.begin();
  timeClient.update();
  updateTimeOffset(); // Initial time offset adjustment
  unsigned long epochTime = timeClient.getEpochTime();
  setTime(epochTime);
  
  // ----------------- General Init -----------------
  
  lastTimeBotRan = millis();
  Alarm.delay(0);
  
  // ----------------- OTA -----------------
  ArduinoOTA.setHostname("MorningLEDs"); // Set a hostname (optional)
  ArduinoOTA.onStart([]() {
    Serial.println("Start updating...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.println("OTA ready");

  // ----------------- MQTT -----------------

  // If ENABLE_MQTT is defined, connect to the MQTT broker
  #ifdef ENABLE_MQTT
  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);
  #endif
  
  // ----------------- Webpage ----------------
  
  #ifdef ENABLE_WEB_SERVER

  // TODO - Save time input in webpage
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getMainHTML());
  });
  server.on("/currentBrightness", HTTP_GET, handleSliderChange);
  server.on("/maxBrightness", HTTP_GET, handleSliderChange);
  server.on("/sunriseDuration", HTTP_GET, handleSliderChange);
  server.on("/setSunrise", HTTP_GET, handleSetSunrise);
  server.on("/reboot", HTTP_GET, []() {
    server.send(200, "text/plain", "Rebooting...");
    ESP.restart();
  });

  server.on("/getInitialValues", HTTP_GET, []() {
    StaticJsonDocument<200> doc;
    doc["currentBrightness"] = currentBrightness;
    doc["maxBrightness"] = maxBrightness;
    doc["sunriseDuration"] = brightnessDuration;
    
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
  });
  server.begin();
  #endif
  
  // ----------------- Init message -----------------

  // client.loop();
  sendMessage("MorningLEDs started", true, true);


}

#ifdef ENABLE_WEB_SERVER

void handleSliderChange() {
  String sliderId;
  if (server.hasArg("value")) {
    int value = server.arg("value").toInt();
    // Determine which slider sent the request
    if (server.uri() == "/currentBrightness") {
      // send message to actByMessage to change brightness
      actByMessage("/setbrightness " + String(value));
    } else if (server.uri() == "/maxBrightness") {
      // send message to actByMessage to change max brightness
      actByMessage("/setmaxbrightness " + String(value));

    } else if (server.uri() == "/sunriseDuration") {
      // send message to actByMessage to change sunrise duration
      actByMessage("/setduration " + String(value));
    }
  } else {
    server.send(400, "text/plain", "Missing value");
  }
}

void handleSetSunrise() {
    if (!server.hasArg("hour") || !server.hasArg("minute")) {
        server.send(400, "text/plain", "Hour or minute parameter missing");
        return;
    }
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    
    // Set the time by sending a message to actByMessage
    actByMessage("/settime " + String(hour) + ":" + String(minute));


    server.send(200, "text/plain", "Sunrise settings updated");
}

#endif

#ifdef ENABLE_MQTT
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    payload[length] = '\0'; // Null-terminate the payload
    String message = String((char*)payload);
    Serial.println(message);
    actByMessage(message);    
}


void reconnect() {
  while (!client.connected()) {
    if (client.connect("ESP8266Client")) {
      // client.subscribe("home/morningleds/alarmTime");
      client.subscribe(MQTT_TOPIC);
    }
    delay(2000);
  }
}
#endif


void loop() {
  
  #ifdef ENABLE_WEB_SERVER
  server.handleClient();
  #endif

  #ifdef ENABLE_MQTT
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  #endif

  // If ENABLE_TELEGRAM_BOT is defined, check for new messages from telegram
  #ifdef ENABLE_TELEGRAM_BOT

  if (millis() - lastTimeBotRan > messageCheckInterval) { // Check for new messages every 5 seconds
    Serial.println("Checking for new messages...");
    int numNewMessages = bot.getUpdates(lastUpdateId + 1);
    if (numNewMessages > 0) {
      Serial.println("Got message");
      handleNewMessages(numNewMessages);
    }
    lastTimeBotRan = millis();
  }

  #endif

  Alarm.delay(0);
  ArduinoOTA.handle();

}

void updateTimeOffset() {
  if (isDST(timeClient.getEpochTime())) {
    timeClient.setTimeOffset(10800); // UTC+3 for DST
    sendMessage("DST is active", true, false);
  } else {
    timeClient.setTimeOffset(7200); // UTC+2 for Standard Time
    sendMessage("DST is not active", true, false);
  }
}

bool isDST(long epochTime) {
  time_t rawtime = (time_t)epochTime;
  struct tm *timeinfo = localtime(&rawtime);

  int month = timeinfo->tm_mon + 1;
  int day = timeinfo->tm_mday;
  int wday = timeinfo->tm_wday;

  if (month < 3 || month > 10)  return false; // DST is between March and October
  if (month > 3 && month < 10)  return true;

  int previousSunday = day - wday;
  if (month == 3) return previousSunday >= 24; // DST starts last Sunday in March
  if (month == 10) return previousSunday < 24; // DST ends last Sunday in October

  return false; // Default to standard time
}

void brightnessIncrease() {
  Serial.println("Increasing brightness");
  // Calculate the brightness increase per step

  // Increase brightness by analogWrite (PWM) to the LED_PIN
  currentBrightness += 1;
  if (currentBrightness > maxBrightness) {
    currentBrightness = maxBrightness;
    analogWrite(LED_PIN, currentBrightness);

    // Disable the timer
    Alarm.free(brightnessIncrementTimer);
    Serial.println("Brightness increase finished");
    sendMessage("Brightness increase finished", true, false);
    return;
  }
  analogWrite(LED_PIN, currentBrightness);
  // bot.sendMessage(CHAT_ID, "Brightness increased to: " + String(currentBrightness));
  Serial.println("Brightness increased to: " + String(currentBrightness));
  sendMessage("Brightness increased to: " + String(currentBrightness), true, false);
}

void startBrightnessIncrease() {
  sendMessage("Brightness increase started", true, false);
  // Start the process of gradually increasing brightness from 0 to maxBrightness over brightnessDuration minutes
  // Do it using a Alarm.timerRepeat untill maxBrightness is reached and then disable the timer

  // Calculate seconds for each step
  int secondsPerStep = (brightnessDuration * 60) / maxBrightness;

  if (secondsPerStep < 1) {
    secondsPerStep = 1;
  }

  // Set the initial brightness to 0
  currentBrightness = 0;
  
  // Start the timer
  brightnessIncrementTimer = Alarm.timerRepeat(secondsPerStep, brightnessIncrease);
  sendMessage("Brightness increase started, secondsPerStep: " + String(secondsPerStep), true, false);

  // Cancel the alarm
  // Alarm.free(alarmId);
}

#ifdef ENABLE_TELEGRAM_BOT
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    int newUpdateId = bot.messages[i].update_id;
    lastUpdateId = newUpdateId;
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    Serial.println("Got message from telegram, chat id  " + chat_id + ": " + text);
    // Example command: /settime 14:30
    
  }
}
#endif

void actByMessage(String message){
  if (message.startsWith("/settime")) {
      int splitIndex = message.indexOf(' ');
      if (splitIndex != -1) {
        String timeText = message.substring(splitIndex + 1);
        int hour = timeText.substring(0, timeText.indexOf(':')).toInt();
        int minute = timeText.substring(timeText.indexOf(':') + 1).toInt();

        if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
          targetHour = hour;
          targetMinute = minute;

          int adjustedHour = targetHour;
          int adjustedMinute = targetMinute - brightnessDuration;

          // Adjust hour and minute if minute becomes negative
          if (adjustedMinute < 0) {
            int hourAdjustment = (abs(adjustedMinute) / 60) + 1;
            adjustedMinute += 60 * hourAdjustment;
            adjustedHour -= hourAdjustment;

            // Handle day wrap-around
            if (adjustedHour < 0) {
              adjustedHour += 24;
            }
          }

          // Alarm.free(); // Clear existing alarms
          brightnessIncreaseRoutineAlarm = Alarm.alarmOnce(adjustedHour, adjustedMinute, 0, startBrightnessIncrease);
          sendMessage("Time set to " + String(adjustedHour) + ":" + String(adjustedMinute), true, false);
          sendMessage("Minutes untill alarm: " + String((Alarm.getNextTrigger() - now())/60), true, false);
        } else {
          sendMessage("Invalid time format. Please use HH:MM format.", true, false);
        }
      } else {
        sendMessage("Please specify a time. Example: /settime 14:30", true, false);
      }
    }
  if (message == "/status") {
    // Print if alarm in enabled
    if (!Alarm.getNextTrigger(brightnessIncreaseRoutineAlarm)) {
      sendMessage("Alarm is disabled", true, false);
    }
    else{
      sendMessage("Alarm is enabled", true, false);
    }

    sendMessage("now: " + String(now()), true, false);
    sendMessage("next alarm: " + String(Alarm.getNextTrigger()), true, false);
    sendMessage("minutes untill alarm: " + String((Alarm.getNextTrigger() - now())/60), true, false);

  }
  if (message.startsWith("/setduration")) {
    brightnessDuration = message.substring(message.indexOf(' ') + 1).toInt();
    sendMessage("Brightness duration set to " + String(brightnessDuration) + " minutes", true, false);
  }
  if (message.startsWith("/setbrightness")) {
    int brightness = message.substring(message.indexOf(' ') + 1).toInt();
    if (brightness >= 0 && brightness <= 1023) {
      currentBrightness = brightness;
      analogWrite(LED_PIN, currentBrightness);
      sendMessage("Brightness set to " + String(currentBrightness), true, false);
    } else {
      sendMessage("Invalid brightness value. Please use a value between 0 and 1023", true, false);
    }
  }
  if (message.startsWith("/setmaxbrightness")) {
    int brightness = message.substring(message.indexOf(' ') + 1).toInt();
    if (brightness >= 0 && brightness <= 1023) {
      maxBrightness = brightness;
      sendMessage("Max brightness set to " + String(maxBrightness), true, false);
    } else {
      sendMessage("Invalid brightness value. Please use a value between 0 and 1023", true, false);
    }
  }
  if (message == "/reboot") {
    ESP.restart();
  }
}

void sendMessage(String message, boolean isMQQT, boolean isTelegram) {
  /*
    Send a message to either MQQT or Telegram or both, MQTT is default
  */
  #ifdef ENABLE_MQTT
  if (isMQQT) {
    sendMessageToMQQT(message);
  }
  #endif

  #ifdef ENABLE_TELEGRAM_BOT
  if (isTelegram) {
    sendMessageToTelegram(message);
  }
  #endif
}

#ifdef ENABLE_TELEGRAM_BOT
void sendMessageToTelegram(String message) {
  bot.sendMessage(CHAT_ID, message);
}
#endif

#ifdef ENABLE_MQTT
void sendMessageToMQQT(String message) {
  client.publish("home/morningleds/terminalOut", message.c_str());
}
#endif


String getMainHTML() {
    return R"rawliteral(

<!DOCTYPE html>
<html>
<head>
    <title>Sunrise Control Dashboard</title>
    <style>
        body { font-family: Arial, sans-serif; display: flex; flex-direction: column; align-items: center; justify-content: center; height: 100vh; margin: 0; background-color: #f0f0f0; }
        h1 { color: #333; }
        .slider-container { display: flex; flex-direction: column; width: 100%; max-width: 600px; padding: 0 20px; }
        .description { display: flex; align-items: center; margin: 10px 0; }
        .description label { flex-basis: 180px; text-align: right; margin-right: 20px; }
        .slider { -webkit-appearance: none; width: calc(100% - 200px); height: 15px; border-radius: 5px; background: #d3d3d3; outline: none; opacity: 0.7; transition: opacity .2s; }
        .slider:hover { opacity: 1; }
        .slider::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 25px; height: 25px; border-radius: 50%; background: #4CAF50; cursor: pointer; }
        .slider::-moz-range-thumb { width: 25px; height: 25px; border-radius: 50%; background: #4CAF50; cursor: pointer; }
        .value-display { font-weight: bold; margin-left: 10px; }
        .time-container {
            display: flex;
            flex-direction: column;
            align-items: center;
            width: 100%;
            max-width: 400px;
        }
        input[type="time"] {
            width: 70%;
            padding: 8px;
            border: 1px solid #ccc;
            border-radius: 4px;
            box-sizing: border-box;
        }
        button { border: none; color: white; padding: 15px 32px; text-align: center; text-decoration: none; display: inline-block; font-size: 16px; margin: 4px 2px; cursor: pointer; background-color: #4CAF50; border-radius: 12px; }
    </style>
</head>
<body>
    <h1>Sunrise Control Dashboard</h1>
    <div class="slider-container">
        <div class="description">
            <label for="currentBrightness">Set Current Brightness:</label>
            <input type="range" id="currentBrightness" class="slider" min="0" max="1023" />
            <span id="currentBrightnessValue" class="value-display"></span>
        </div>
        <div class="description">
            <label for="maxBrightness">Max Brightness:</label>
            <input type="range" id="maxBrightness" class="slider" min="1" max="1023" />
            <span id="maxBrightnessValue" class="value-display"></span>
        </div>
        <div class="description">
            <label for="sunriseDuration">Sunrise Duration (minutes):</label>
            <input type="range" id="sunriseDuration" class="slider" min="5" max="120" />
            <span id="sunriseDurationValue" class="value-display"></span>
        </div>
    </div>
    <div class="time-container">
        <div class="description">
            <label>Set Sunrise Time:</label>
            <input type="time" id="sunriseTime">
        </div>
    </div>
    <button onclick="setSunrise()">Set Sunrise</button>
    <button onclick="reboot()">Reboot</button>
    <script>
        document.addEventListener('DOMContentLoaded', function() {
            fetchInitialValues();
        });

        function fetchInitialValues() {
            fetch('/getInitialValues')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('currentBrightness').value = data.currentBrightness;
                    updateSliderDisplay('currentBrightness', data.currentBrightness);
                    
                    document.getElementById('maxBrightness').value = data.maxBrightness;
                    updateSliderDisplay('maxBrightness', data.maxBrightness);
                    
                    document.getElementById('sunriseDuration').value = data.sunriseDuration;
                    updateSliderDisplay('sunriseDuration', data.sunriseDuration);
                }).catch(error => console.error('Error fetching initial slider values:', error));
        }

        function updateSliderDisplay(id, value) {
            document.getElementById(id + "Value").innerText = value;
        }

        document.querySelectorAll('.slider').forEach(item => {
            item.addEventListener('change', event => {
                let id = event.target.id;
                let value = event.target.value;
                fetch(`/${id}?value=${value}`);
                updateSliderDisplay(id, value);
            });
        });

    </script>
    <script>
    function setSunrise() {
            const timeValue = document.getElementById('sunriseTime').value;
            const [hour, minute] = timeValue.split(':');
            const currentBrightness = document.getElementById('currentBrightness').value;
            const maxBrightness = document.getElementById('maxBrightness').value;
            const sunriseDuration = document.getElementById('sunriseDuration').value;

            fetch(`/setSunrise?hour=${hour}&minute=${minute}&currentBrightness=${currentBrightness}&maxBrightness=${maxBrightness}&sunriseDuration=${sunriseDuration}`)
                .then(response => {
                    if (!response.ok) throw new Error('Network response was not ok');
                    return response.text();
                })
                .then(text => console.log(text))
                .catch(error => console.error('Failed to set sunrise:', error));
        }
    function reboot() {
        fetch(`/reboot`)
            .then(response => {
                if (!response.ok) throw new Error('Network response was not ok');
                return response.text();
            })
            .then(text => console.log(text))
            .catch(error => console.error('Failed to reboot:', error));
    }
</script>
</body>
</html>

)rawliteral";
}

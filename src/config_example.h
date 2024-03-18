// Replace with your network credentials
const char* ssid = "SSID";
const char* password = "PASS";

#define MAX_BRIGHTNESS 255
#define SUNRISE_DURATION_MINUTES 45

#define LED_PIN 2

// Enable web server
#define ENABLE_WEB_SERVER


// Enable MQTT
// #define ENABLE_MQTT

#ifdef ENABLE_MQTT
// Replace with your MQTT Broker's IP address
const char* mqtt_server = "192.168.0.XXX";
#define MQTT_TOPIC "abc/xxx"

#endif

// Enable telegram bot
// #define ENABLE_TELEGRAM_BOT

#ifdef ENABLE_TELEGRAM_BOT
// Replace with your Telegram chat ID
#define CHAT_ID "-123123"
// Replace with your Telegram Bot token
const char* token = "123123:AAhhh";
#endif









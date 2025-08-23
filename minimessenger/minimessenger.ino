/*
2025-08
D1Mini Whatsapp
avec le freebroker HiveMQ

Sa console web : https://console.hivemq.cloud/clusters/8f76c91610f343c2b6795974c58861c7/web-g_mqttClient

TODO
- gérer les accents entrants
- utiliser WiFiManager (cf mistral) pour configurer le wifi - https://github.com/tzapu/WiFiManager, sinon déplacer la connection dans loop
*/


// ================================================================================
// Librairies
// ================================================================================

// Provided by Arduino IDE (with ESP8266 board plugins ?)
#include <ESP8266WiFi.h>

// Install from library manager: "PubSubClient" (2.8)
#include <PubSubClient.h>

// Provided by Arduino IDE (with ESP8266 board plugins ?)
#include <WiFiClientSecure.h>

// Install from library manager:
// - "Adafruit SSD1306 Wemos Mini OLED" (1.1.2)
// - "Adafruit ST7735 and ST7789" (1.11.0)
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>  // "Oled Shield" for D1Mini
#include <Adafruit_ST7789.h>   // 320x240

// To get current date & time for timestamping messages
#include <WiFiUdp.h>
// Install from library manager: "NTPClient" (3.2.1)
#include <NTPClient.h>


// ================================================================================
// Toggles
// ================================================================================

// Serial
#define FLAG_READ_SERIAL_INPUTS

#define WITH_LOGS


// ================================================================================
// User configuration
// ================================================================================

// WiFi credentials
const char* ssid = "SatelliteThree";  // Wifi SSID
const char* password = "xxxxxxx";  // WiFi Password
//const char* ssid = "AndroidPACPAC5";          // Wifi SSID
//const char* password = "apapapap";          // WiFi Password

// MQTT Broker details
const char* mqtt_server = "xxxxxx.s1.eu.hivemq.cloud";  // MQTT Broker's URL
const int mqtt_port = 8883;                                                       // TLS Port
const char* mqtt_user = "xxxxx";                                                  // Credential Username
const char* mqtt_password = "xxxxxxx";                                           // Credential Password

const char* g_mqttOutgoingTopicLogs = "admin/logs";
const char* g_mqttOutgoingTopicLive = "admin/live";
const char* g_mqttOutgoingTopicWill = "admin/dead";
const char* g_mqttIncomingTopicBroadcast = "msg/broadcast";
//                                         "msg/unicast/12"


#define NTP_UTC_OFFSET 7200  // UTC+2


// ================================================================================
// Configurable behaviour
// ================================================================================

#define LED_BLINK_FAST_DURATION 150
#define LED_BLINK_SLOW_DURATION 700

// Period between sending 2 "keepalive" messages
#define MQTT_KEEPALIVE_INTERVAL 30000
// Period between retring connection to MQTT broker
#define MQTT_CONNECT_RETRY_INTERVAL 15000


// ================================================================================
// Hardware configuration
// ================================================================================

// Pins configuration
// ------------------
// Pins OK:
// D0 GPIO16 Attention : Ne supporte pas les interruptions. Doit être à HIGH au démarrage pour éviter les problèmes.
// D5 GPIO14 Disponible.
// D6 GPIO12 Disponible.
// D7 GPIO13 Recommandée pour une LED.
// D8 GPIO15 Attention : Doit être à LOW au démarrage pour le mode flash.
// Pins Not OK:
// Évite les broches D3 (GPIO0) et D4 (GPIO2) si tu utilises le WiFi ou le bootloader.
// D1 GPIO5 : OLED shield pour la communication I2C (SCL)
// D2 GPIO4 : OLED shield pour la communication I2C (SDA)
// D3 GPIO0 Attention : Doit être à HIGH au démarrage (sinon, le D1 Mini passe en mode flash).
// D4 GPIO2 LED intégrée (inversée : LOW = allumée). Peut être utilisée, mais la LED bleue intégrée s'allumera aussi.


//Préfère les broches D1, D2, D5, D6, D7 ou D8 pour une LED.
#define LED_STATUS D8 //D5  // GPIO14
#define LED_FRIEND_1 D6
#define LED_FRIEND_2 D1 // D7


// OLED configuration
// ------------------
// SCL GPIO5
// SDA GPIO4
#define OLED_RESET 0  // GPIO0  TODO Correct?


#define TFT_CS D2   // TFT CS  pin is connected to NodeMCU pin D2
#define TFT_RST D3  // TFT RST pin is connected to NodeMCU pin D3
#define TFT_DC D4   // TFT DC  pin is connected to NodeMCU pin D4
// SCK (CLK) ---> NodeMCU pin D5 (GPIO14)
// MOSI(DIN) ---> NodeMCU pin D7 (GPIO13)



// ================================================================================
// Global variables
// ================================================================================

// WiFi
WiFiClientSecure g_wifiClient;

// MQTT
PubSubClient g_mqttClient(g_wifiClient);
int g_mqttConnectionId = -1;
unsigned int g_mqttOutputMsgId = 0;
bool g_mqttWasConnected = false;
unsigned long g_mqttLastReconnectTryTimestampMs = 0;
unsigned long g_mqttPreviousKeepAliveTimestampMs = 0;


// Size includes all standard fields plus user's payload
#define MSG_BUFFER_SIZE 500
char g_mqttOutgoingMsg[MSG_BUFFER_SIZE];

#define MQTT_TOPIC_SIZE 30
char g_mqttOutoingRecipientTopic[MQTT_TOPIC_SIZE];


// OLED Display
#define DISPLAY_TYPE_OLEDSHIELD 1
#define DISPLAY_TYPE_ST7789 2
int g_displayType = 0;

Adafruit_SSD1306 g_displayOledShield(OLED_RESET);
Adafruit_GFX* g_display2 = NULL;

static const unsigned char PROGMEM logo16_glcd_bmp[] = { B00000000, B11000000,
                                                         B00000001, B11000000,
                                                         B00000001, B11000000,
                                                         B00000011, B11100000,
                                                         B11110011, B11100000,
                                                         B11111110, B11111000,
                                                         B01111110, B11111111,
                                                         B00110011, B10011111,
                                                         B00011111, B11111100,
                                                         B00001101, B01110000,
                                                         B00011011, B10100000,
                                                         B00111111, B11100000,
                                                         B00111111, B11110000,
                                                         B01111100, B11110000,
                                                         B01110000, B01110000,
                                                         B00000000, B00110000 };


// NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", NTP_UTC_OFFSET);

// format "YYYY-MM-DD HH:MM:SS"
char g_ts[20];


// Serial
#ifdef FLAG_READ_SERIAL_INPUTS
#define MAX_SERIAL_MSG_LENGTH 100
char g_contentFromSerial[MAX_SERIAL_MSG_LENGTH + 1];
char g_inChar;
byte g_inNextCharIndex = 0;
#endif

// Messaging
byte g_deviceIdMe = -1;
byte g_deviceIdFriend1 = -1;
byte g_deviceIdFriend2 = -1;
char g_deviceIdChars[4];
char g_deviceName[40];
char g_userPseudo[40];


#define LED_STATE_NOT_CONFIGURED -1
#define LED_STATE_OFF 0
#define LED_STATE_ON 1
#define LED_STATE_BLINK_FAST 2
#define LED_STATE_BLINK_SLOW 3
#define LED_QTY 17
byte g_ledRequiredState[LED_QTY];
bool g_ledBlinkStateIsHigh[LED_QTY];
unsigned long g_ledBlinkLastTimestampMs[LED_QTY];


// ================================================================================
// Logging
// ================================================================================

// Impression d'une seule valeur
template<typename T>
void p_single(const T& val) {
  Serial.print(val);
}

// Impression de plusieurs valeurs
template<typename T, typename... Args>
void p_single(const T& first, const Args&... rest) {
  Serial.print(first);
  p_single(rest...);
}

template<typename... Args>
void hlog(const Args&... args) {
#ifdef WITH_LOGS
  Serial.print(g_deviceName);
  Serial.print(" - ");
  p_single(args...);  // Imprime tous les arguments
#endif WITH_LOGS
}

template<typename... Args>
void hlogn(const Args&... args) {
#ifdef WITH_LOGS
  Serial.print(g_deviceName);
  Serial.print(" - ");
  p_single(args...);  // Imprime tous les arguments
  Serial.println();
#endif WITH_LOGS
}

template<typename... Args>
void log(const Args&... args) {
#ifdef WITH_LOGS
  p_single(args...);  // Imprime tous les arguments
#endif WITH_LOGS
}

template<typename... Args>
void logn(const Args&... args) {
#ifdef WITH_LOGS
  p_single(args...);  // Imprime tous les arguments
  Serial.println();
#endif WITH_LOGS
}


// ================================================================================
// Time
// ================================================================================
void ntpConfigure() {
  timeClient.begin();
}

char* getUpdateDateTimeAsString() {
  timeClient.update();
  return getDateTimeString();
}

char* getDateTimeString() {
  time_t epochTime = timeClient.getEpochTime();

  // Convertir en structure tm
  struct tm* timeInfo = localtime(&epochTime);

  snprintf(g_ts, sizeof(g_ts), "%04d-%02d-%02d|%02d:%02d:%02d",
           timeInfo->tm_year + 1900,
           timeInfo->tm_mon + 1,
           timeInfo->tm_mday,
           timeInfo->tm_hour,
           timeInfo->tm_min,
           timeInfo->tm_sec);

  //return String(buffer);
  return g_ts;
}


// ================================================================================
// Connectivity
// ================================================================================

void identifyDevice() {
  String mac = WiFi.macAddress();  // Get MAC address as string
  hlogn("MAC Address: ", mac);

  int recipientId = 3;

  if (mac == "xx:xx:xx:xx:xx:xx") {
    strcpy(g_userPseudo, "Papa");
    g_deviceIdMe = 1;
    g_deviceIdFriend1 = 2;
    g_deviceIdFriend2 = 3;

    g_displayType = DISPLAY_TYPE_ST7789;
  } else if (mac == "xx:xx:xx:xx:xx:xx") {
    strcpy(g_userPseudo, "Maïa");
    g_deviceIdMe = 2;
    g_deviceIdFriend1 = 1;
    g_deviceIdFriend2 = 3;
  } else if (mac == "xx:xx:xx:xx:xx:xx") {
    strcpy(g_userPseudo, "Jolan");
    g_deviceIdMe = 3;
    g_deviceIdFriend1 = 1;
    g_deviceIdFriend2 = 2;
    recipientId = 2;

    g_displayType = DISPLAY_TYPE_OLEDSHIELD;

  } else {
    strcpy(g_userPseudo, "JohnDoe");
    g_deviceIdMe = random(100, 1000);
  }

  // Non formated g_deviceIdMe (pour Will topic)
  snprintf(g_deviceIdChars, 4, "%d", g_deviceIdMe);
  snprintf(g_deviceName, 8, "D1M_%03d", g_deviceIdMe);

  hlogn("Identified device: name=", g_deviceName, "id=", g_deviceIdMe, ", screenType:", g_displayType);

  WiFi.hostname(g_deviceName);

  setRecipient(recipientId);
}


// ================================================================================
// MQTT
// ================================================================================

#define MQTT_MSG_RETAINED true
#define MQTT_MSG_NOT_RETAINED false

#define MQTT_SESSION_VOLATILE true
#define MQTT_SESSION_PERSISTED false

#define MQTT_QOS_0 0
#define MQTT_QOS_1 1
#define MQTT_QOS_2 2

// Return true is reconnection is successfull
bool mqttReconnect() {
  hlog("MQTT: Attempting connection...");

  if (g_mqttClient.connect(
        g_deviceName,
        mqtt_user, mqtt_password,
        g_mqttOutgoingTopicWill, MQTT_QOS_0, MQTT_MSG_NOT_RETAINED, g_deviceIdChars,
        MQTT_SESSION_VOLATILE)) {
    logn("connected");
    hlogn("MQTT: MQTT_MAX_PACKET_SIZE=", MQTT_MAX_PACKET_SIZE);

    g_mqttClient.subscribe(g_mqttIncomingTopicBroadcast, MQTT_QOS_1);

    String myUnicastTopic = String("msg/unicast/") + g_deviceIdMe;
    g_mqttClient.subscribe(myUnicastTopic.c_str(), MQTT_QOS_1);
    g_mqttClient.subscribe(g_mqttOutgoingTopicLive, MQTT_QOS_0);
    g_mqttClient.subscribe(g_mqttOutgoingTopicWill, MQTT_QOS_0);

    g_mqttWasConnected = true,
    g_mqttConnectionId++;
    ledSetState(LED_STATUS, LED_STATE_ON);

    // Send public liveness
    mqttSendAlive((g_mqttConnectionId == 0 ? 0 : 1));

    return true;
  } else {
    logn("failed, rc=", g_mqttClient.state(), " trying again in ", MQTT_CONNECT_RETRY_INTERVAL, 's');
    //            delay(5000);
    return false;
  }
}

// 0: boot, 1:reco, 2:keepalive
void mqttSendAlive(int liveType) {
  char payload[MSG_BUFFER_SIZE];
  snprintf(payload, MSG_BUFFER_SIZE,
           "%d %s mac:%s ssid:%s ip:%s recoId:%d",
           g_deviceIdMe,
           (liveType == 0 ? "boot" : (liveType == 1 ? "reco" : "keep")),
           WiFi.macAddress().c_str(),
           ssid,
           WiFi.localIP().toString().c_str(),
           g_mqttConnectionId);
  mqttPushFormattedMessage(g_mqttOutgoingTopicLive, payload);
}


void mqttPushFormattedMessage(const char* topic, const char* payload) {
  snprintf(g_mqttOutgoingMsg, MSG_BUFFER_SIZE,
           "%s ### ts:%s deviceId:%d msgId:%d",
           payload,
           getUpdateDateTimeAsString(), g_deviceIdMe, g_mqttOutputMsgId);

  hlog("MQTT: Publishing message #", g_mqttOutputMsgId, " to topic [", topic, "] : [", g_mqttOutgoingMsg, ']');
  // Publishing. Only QoS 0 is possible at publish time with PubSubClient
  bool ok = g_mqttClient.publish(topic, g_mqttOutgoingMsg, MQTT_MSG_RETAINED);
  if (ok) {
    logn(". Done.");
  } else {
    logn(". Failed.");
  }

  g_mqttOutputMsgId++;
}


// ================================================================================
// LEDd
// ================================================================================

void ledSetState(int pin, int requiredState) {
  hlogn("Setting led state for pin #", pin, " to state ", requiredState);
  g_ledRequiredState[pin] = requiredState;

  if (requiredState == LED_STATE_OFF) {
    digitalWrite(pin, LOW);
  } else if (requiredState == LED_STATE_ON) {
    digitalWrite(pin, HIGH);
  } else {
    g_ledBlinkStateIsHigh[pin] = false;
    g_ledBlinkLastTimestampMs[pin] = millis();
    digitalWrite(pin, g_ledBlinkStateIsHigh[pin] ? HIGH : LOW);
    //hlogn("details pin #", pin, " g_ledBlinkStateIsHigh[pin]=", g_ledBlinkStateIsHigh[pin], " g_ledBlinkLastTimestampMs[pin]=", g_ledBlinkLastTimestampMs[pin]);
  }
}

void ledCommuteBlinkState(int pin) {
  g_ledBlinkStateIsHigh[pin] = !g_ledBlinkStateIsHigh[pin];
  g_ledBlinkLastTimestampMs[pin] = millis();
  digitalWrite(pin, g_ledBlinkStateIsHigh[pin] ? HIGH : LOW);
  //hlogn("Switching led state for blinking pin #", pin, " to new state ", g_ledBlinkStateIsHigh[pin]);
}

void setupLeds() {
  // Integrated blue led (inversed: LOW=On)
  pinMode(D4, OUTPUT);
  pinMode(LED_STATUS, OUTPUT);
  pinMode(LED_FRIEND_1, OUTPUT);
  pinMode(LED_FRIEND_2, OUTPUT);

  for (int i = 0; i < 4; i++) {
    digitalWrite(D4, LOW);
    digitalWrite(LED_STATUS, HIGH);
    digitalWrite(LED_FRIEND_1, HIGH);
    digitalWrite(LED_FRIEND_2, HIGH);
    delay(150);
    digitalWrite(D4, HIGH);
    digitalWrite(LED_STATUS, LOW);
    digitalWrite(LED_FRIEND_1, LOW);
    digitalWrite(LED_FRIEND_2, LOW);
    delay(150);
  }

  for (int pin = 0; pin < LED_QTY; pin++) {
    g_ledRequiredState[pin] = LED_STATE_NOT_CONFIGURED;
  }
  ledSetState(LED_STATUS, LED_STATE_BLINK_FAST);
  ledSetState(LED_FRIEND_1, LED_STATE_OFF);
  ledSetState(LED_FRIEND_2, LED_STATE_OFF);
}



// ================================================================================
// OLED
// ================================================================================

void setupDisplay() {
  if (g_displayType == DISPLAY_TYPE_OLEDSHIELD) {
    Adafruit_SSD1306* pDisp = new Adafruit_SSD1306(OLED_RESET);

    // by default, we'll generate the high voltage from the 3.3v line internally! (neat!)
    pDisp->begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3C (for the 64x48)

    g_display2 = pDisp;
  } else if (g_displayType == DISPLAY_TYPE_ST7789) {
    Adafruit_ST7789* pDisp = new Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

    pDisp->init(240, 320);

    g_display2 = pDisp;
  } else {
    hlogn("Display: nothing to configure");
  }
}


void showSplashScreen() {
  // Show image buffer on the display hardware.
  // Since the buffer is intialized with an Adafruit splashscreen internally, this will display the splashscreen.
  int duration = 1000;

  if (g_displayType == DISPLAY_TYPE_OLEDSHIELD) {
    Adafruit_SSD1306* pDisp = (Adafruit_SSD1306*)g_display2;

    pDisp->display();

  } else if (g_displayType == DISPLAY_TYPE_ST7789) {
    Adafruit_ST7789* pDisp = (Adafruit_ST7789*)g_display2;

    pDisp->fillScreen(ST77XX_BLACK);
    pDisp->fillCircle(50, 50, 30, ST77XX_MAGENTA);

    for (int x = 0; x < 320; x+=50) {
      pDisp->drawFastHLine(0, x, pDisp->width(), ST77XX_RED);
      pDisp->drawFastVLine(x, 0, pDisp->height(), ST77XX_YELLOW);
    }

  /*for (int16_t x=10; x < pDisp->width(); x+=20) {
    for (int16_t y=10; y < pDisp->height(); y+=20) {
      pDisp->fillCircle(x, y, 10, ST77XX_BLUE);
    }
  }*/

    duration = 1000;
  } else {
    hlogn("Display: no splash screen");
    duration = 0;
  }

  delay(duration);
}


// ================================================================================
// Messaging
// ================================================================================

void setRecipient(int recipientDeviceId) {
  snprintf(g_mqttOutoingRecipientTopic, MQTT_TOPIC_SIZE,
           "msg/unicast/%d",
           recipientDeviceId);
  hlogn("MQTT: Setting recipient topic to [", g_mqttOutoingRecipientTopic, ']');
}


// ================================================================================
// Entrypoints
// ================================================================================

void showUpdatedInfoScreen() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");

  if (g_displayType == DISPLAY_TYPE_OLEDSHIELD) {

    Adafruit_SSD1306* pDisp = (Adafruit_SSD1306*)g_display2;

    pDisp->clearDisplay();
    pDisp->setCursor(0, 0);

    pDisp->setTextSize(1);
    pDisp->setTextColor(WHITE);
    pDisp->print(g_deviceIdMe);
    pDisp->print(' ');

    pDisp->setTextColor(BLACK, WHITE);
    pDisp->print(g_deviceName);

    pDisp->setTextColor(WHITE);
    pDisp->print(' ');
    pDisp->print(mac);
    pDisp->print(' ');

    pDisp->setTextColor(BLACK, WHITE);
    pDisp->print(ssid);

    pDisp->setTextColor(WHITE);
    pDisp->print(' ');
    if (WiFi.status() != WL_CONNECTED) {
      pDisp->print("no-wifi");
    } else {
      pDisp->print(WiFi.localIP().toString());
    }
    pDisp->print(' ');

    pDisp->setTextColor(BLACK, WHITE);
    if (g_mqttClient.connected()) {
      pDisp->print("DATA");
    } else {
      pDisp->print("----");
    }

    pDisp->display();
  } else if (g_displayType == DISPLAY_TYPE_ST7789) {

    Adafruit_ST7789* pDisp = (Adafruit_ST7789*)g_display2;

    pDisp->fillScreen(ST77XX_BLACK);
    pDisp->setCursor(0, 0);
    pDisp->setTextSize(2);
    pDisp->setTextColor(ST77XX_WHITE);

    pDisp->print(g_deviceIdMe);
    pDisp->print(' ');

    pDisp->setTextColor(ST77XX_BLACK, ST77XX_WHITE);
    pDisp->print(g_deviceName);

    pDisp->setTextColor(ST77XX_WHITE);
    pDisp->print(' ');
    pDisp->print(mac);
    pDisp->print(' ');

    pDisp->setTextColor(ST77XX_BLACK, ST77XX_WHITE);
    pDisp->print(ssid);

    pDisp->setTextColor(ST77XX_WHITE);
    pDisp->print(' ');
    if (WiFi.status() != WL_CONNECTED) {
      pDisp->print("no-wifi");
    } else {
      pDisp->print(WiFi.localIP().toString());
    }
    pDisp->print(' ');

    pDisp->setTextColor(ST77XX_BLACK, ST77XX_WHITE);
    if (g_mqttClient.connected()) {
      pDisp->print("DATA");
    } else {
      pDisp->print("----");
    }
  } else {
    hlogn("Display : no display configured");
  }
}


void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP8266: setup()");

  // Initialise le générateur de nombres aléatoires
  // Utilise une broche non connectée pour varier la graine (seed)
  randomSeed(analogRead(A0));  // A0 est une broche non connectée (bruit analogique)

  identifyDevice();

  setupDisplay();
  setupLeds();

  showSplashScreen();
  showUpdatedInfoScreen();

  ntpConfigure();

  // Connect to WiFi
  hlog("WiFi: Connecting...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    log(".");
  }
  logn(" Connected. IP=", WiFi.localIP().toString());
  g_wifiClient.setInsecure();  // Use this if you don't have a certificate
  showUpdatedInfoScreen();


  // Set up MQTT with TLS
  g_mqttClient.setServer(mqtt_server, mqtt_port);
  g_mqttClient.setCallback(onMqttIncomingMessage);

  g_mqttWasConnected = true;
  // Test blinking
  //g_mqttLastReconnectTryTimestampMs = millis();

  resetSerialBuffer();

  Serial.println("ESP8266: setup() completed.");
}

void resetSerialBuffer() {
  // Reset g_inNextCharIndex and clean buffer (then no need to add '\0' at end of current msg)
  g_inNextCharIndex = 0;
  for (int i = 0; i <= MAX_SERIAL_MSG_LENGTH; i++) {
    g_contentFromSerial[i] = 0;
  }

  // Prefix with my name for next message
  snprintf(g_contentFromSerial, sizeof(g_contentFromSerial),
           "%s: ",
           g_userPseudo);
  g_inNextCharIndex = strlen(g_contentFromSerial);
}

bool g_firstLoop = true;

void loop() {
  unsigned long currentMillis = millis();  // Temps actuel en millisecondes depuis le démarrage

  if (!g_mqttClient.connected()) {
    if (g_mqttWasConnected) {
      g_mqttWasConnected = false;
      ledSetState(LED_STATUS, LED_STATE_BLINK_FAST);
    }

    // Time to try to reconnect ?
    if (g_firstLoop || currentMillis - g_mqttLastReconnectTryTimestampMs > MQTT_CONNECT_RETRY_INTERVAL) {
      // Reconnection attempt is successfull
      if (mqttReconnect()) {
        showUpdatedInfoScreen();
      }
    }
  } else {
    g_mqttClient.loop();

    // MQTT: Send keep-alive
    if (currentMillis - g_mqttPreviousKeepAliveTimestampMs >= MQTT_KEEPALIVE_INTERVAL) {
      // Met à jour le temps de la dernière exécution
      g_mqttPreviousKeepAliveTimestampMs = currentMillis;

      // Send public liveness
      mqttSendAlive(2);
    }
  }



#ifdef FLAG_READ_SERIAL_INPUTS

  // Add a new char to the buffer
  while (Serial.available() > 0) {
    g_inChar = Serial.read();

    // 'Enter key' : send message
    if (g_inChar == '\n') {
      if (g_inNextCharIndex > 0) {
        hlogn("Serial: Read msg id #", g_mqttOutputMsgId, " : ", g_contentFromSerial);
        mqttPushFormattedMessage(g_mqttOutoingRecipientTopic, g_contentFromSerial);

        resetSerialBuffer();
      } else {
        // Message is empty. Do nothing
        hlogn("Serial: Not publishing an empty message.");
      }
    } else {
      // Too long ?
      if (g_inNextCharIndex >= MAX_SERIAL_MSG_LENGTH) {
        // Force reset all
        hlogn("Serial: Msg too long. Dropping it");
        resetSerialBuffer();
      } else {
        g_contentFromSerial[g_inNextCharIndex] = g_inChar;
        g_inNextCharIndex++;
      }
    }
  }
#endif


  // Blinking leds management
  for (int pin = 0; pin < LED_QTY; pin++) {
    if (g_ledRequiredState[pin] == LED_STATE_BLINK_FAST) {
      if (currentMillis - g_ledBlinkLastTimestampMs[pin] > LED_BLINK_FAST_DURATION) {
        ledCommuteBlinkState(pin);
      }
    } else if (g_ledRequiredState[pin] == LED_STATE_BLINK_SLOW) {
      if (currentMillis - g_ledBlinkLastTimestampMs[pin] > LED_BLINK_SLOW_DURATION) {
        ledCommuteBlinkState(pin);
      }
    }
  }

  g_firstLoop = false;
}

void onMqttIncomingMessage(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  if (strcmp(topic, g_mqttOutgoingTopicLive) == 0) {
    int remoteDeviceId = atoi(message.c_str());
    onLiveness(remoteDeviceId, true);
  } else if (strcmp(topic, g_mqttOutgoingTopicWill) == 0) {
    int remoteDeviceId = atoi(message.c_str());
    onLiveness(remoteDeviceId, false);
  }
  // msg/unicast/<me> or msg/broadcast
  else if (topic[0] == 'm') {
    hlogn("MQTT: Incoming message [", message, ']');

    if (g_displayType == DISPLAY_TYPE_OLEDSHIELD) {
      Adafruit_SSD1306* pDisp = (Adafruit_SSD1306*)g_display2;
      pDisp->clearDisplay();
      pDisp->setTextSize(1);
      pDisp->setTextColor(WHITE);
      pDisp->setCursor(0, 0);
      //pDisp->print(topic);
      //pDisp->print(" : ");
      pDisp->print(message);
      pDisp->display();
    }
  } else {
    hlogn("MQTT: Message received in unknown topic [", topic, "] : [", message, ']');
  }
}

void onLiveness(int remoteDeviceId, bool isLive) {
  if (remoteDeviceId == g_deviceIdMe) {
    return;
  }

  hlogn("MQTT: Liveness device #", remoteDeviceId, " isLive:", isLive);
  if (remoteDeviceId == g_deviceIdFriend1) {
    ledSetState(LED_FRIEND_1, (isLive ? LED_STATE_ON : LED_STATE_OFF));
    //digitalWrite(LED_FRIEND_1, (isLive ? HIGH : LOW) );
  } else if (remoteDeviceId == g_deviceIdFriend2) {
    ledSetState(LED_FRIEND_2, (isLive ? LED_STATE_ON : LED_STATE_OFF));
    //digitalWrite(LED_FRIEND_2, (isLive ? HIGH : LOW) );
  }
}
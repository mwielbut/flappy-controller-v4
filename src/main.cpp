#include <Arduino.h>
#include <ArduinoBearSSL.h>
#include <ArduinoECCX08.h>
#include <utility/ECCX08JWS.h>
#include <ArduinoMqttClient.h>
#include <WiFiNINA.h>
#include <ArduinoLog.h>
#include <Arduino_JSON.h>
#include "SerialTransfer.h"

#include "logger.h" 
#include "arduino_secrets.h"

/*****************************/
/******* SET LOG LEVEL *******/
#define LOG_LEVEL LOG_LEVEL_VERBOSE
/*****************************/
/****** SET SCREEN SIZE ******/
#define SCREEN_SIZE 40
/*****************************/
/******* SET ALPHABET ********/
/**** Lowercase is color *****/
/********** g = 🟩 ************/
/********** r = 🟥 ************/
/********** y = 🟨 ************/
/********** p = 🟪 ************/
/********** w = ⬜ ************/
#define ALPHABET " ABCDEFGHIJKLMNOPQRSTUVWXYZg0123456789r.?-$'#yp,!@&w"

/*****************************/
/**** DON'T CHANGE BELOW *****/
#define SERIAL_RATE 115200
#define DEFAULT_CORRECTION 0
#define DEFAULT_CHARACTER ' '
#define AWS_MQTT_KEEPALIVE_INTERVAL 5 * 60 * 1000L
#define AWS_MQTT_CONNECTION_TIMEOUT 60 * 1000L
#define AWS_MQTT_HEALTH_CHECK_INTERVAL 60 * 1000L   // probe liveness more often than the keep-alive
#define AWS_MQTT_INACTIVITY_TIMEOUT 3 * 60 * 1000L  // force a reconnect if nothing's been heard this long
/*****************************/

SerialTransfer wireData;             // controller to worker communication
WiFiClient wifiClient;               // MQTT TCP
BearSSLClient sslClient(wifiClient); // MQTT SSL
MqttClient mqttClient(sslClient);    // MQTT client

// Tracks real MQTT traffic so we can detect a connection that's gone stale
// (e.g. router NAT silently dropping the idle TCP session) even though
// WiFi.status() and mqttClient.connected() both still claim everything's fine.
unsigned long lastMessageMillis = 0;
unsigned long lastHealthCheckMillis = 0;

typedef struct MQTTDesiredState
{
  char message[SCREEN_SIZE];
  char corrections[SCREEN_SIZE];
  bool valid = false;
} MQTTMessage;

char conformToValidMessage(char input, char fallback)
{
  if (input == '\0')
  {
    return fallback;
  }

  char c = input;
  String alphabet = ALPHABET;
  int i = alphabet.indexOf(c);
  if (i == -1)
  {
    return fallback;
  }

  return c;
}

uint8_t conformToValidCorrections(char input, uint8_t fallback)
{
  if (input == '\0')
  {
    return fallback;
  }

  int v = input - '0';
  if (v < 0 || v > 5)
  {
    return fallback;
  }

  return v;
}

void logInfoToLength(char *s, size_t l)
{
  Log.clearPrefix();
  Log.info("\"");
  for (size_t i = 0; i < l; i++)
  {
    Log.info("%c", s[i]);
  }
  Log.infoln("\"");
  Log.setPrefix(printPrefix);
}

void logInfoToLength(uint8_t *s, size_t l)
{
  Log.clearPrefix();
  Log.info("\"");
  for (size_t i = 0; i < l; i++)
  {
    Log.info("%d", s[i]);
  }
  Log.infoln("\"");
  Log.setPrefix(printPrefix);
}

void sendOverWire(struct MQTTDesiredState *desired)
{
  // prepare for wire, conform to valid wire corrections
  uint8_t corrections[SCREEN_SIZE];
  for (size_t i = 0; i < SCREEN_SIZE; i++)
  {
    char c = desired->corrections[i];
    corrections[i] = conformToValidCorrections(c, DEFAULT_CORRECTION);
  }

  // prepare for wire, conform to valid wire message
  char message[SCREEN_SIZE];
  for (size_t i = 0; i < SCREEN_SIZE; i++)
  {
    char c = desired->message[i];
    message[i] = conformToValidMessage(c, DEFAULT_CHARACTER);
  }

  uint16_t sendSize = 0;
  sendSize = wireData.txObj(corrections, sendSize);
  sendSize = wireData.txObj(message, sendSize);
  wireData.sendData(sendSize);

  Log.infoln("");
  Log.infoln("Sent over wire:");
  logInfoToLength(corrections, SCREEN_SIZE);
  logInfoToLength(message, SCREEN_SIZE);
  Log.infoln("");
}

struct MQTTDesiredState parseDesiredStateMessage(String message)
{
  Log.infoln("");
  Log.infoln("Parsing desired state message:");
  Log.infoln(message.c_str());

  JSONVar doc = JSON.parse(message);
  MQTTDesiredState desired;
  desired.valid = false; // invalid until valid

  if (JSON.typeof(message) == "undefined")
  {
    Log.errorln("Parsing message failed, invalid JSON!");
    return desired;
  }

  if (!doc.hasOwnProperty("state"))
  {
    Log.errorln("Parsing message failed, missing `state` property!");
    return desired;
  }

  JSONVar stateDoc = doc["state"];

  if (!stateDoc.hasOwnProperty("desired"))
  {
    Log.info("Message does not contain any `desired` payload, skipping.");
    return desired;
  }

  JSONVar desiredDoc = stateDoc["desired"];

  if (desiredDoc.hasOwnProperty("corrections"))
  {
    const char *value = (const char *)desiredDoc["corrections"];
    if (strlen(value) > 0)
    {
      strncpy(desired.corrections, value, SCREEN_SIZE);
      Log.infoln("Received corrections:");
      logInfoToLength(desired.corrections, SCREEN_SIZE);
    }
  }

  if (desiredDoc.hasOwnProperty("message"))
  {
    const char *value = (const char *)desiredDoc["message"];
    if (strlen(value) > 0)
    {
      strncpy(desired.message, value, SCREEN_SIZE);
      Log.infoln("Received message:");
      logInfoToLength(desired.message, SCREEN_SIZE);
    }
  }

  desired.valid = true;

  return desired;
}

void publishReportedStateMessage(struct MQTTDesiredState *desired)
{

  /*
  "state": {
    "reported": {
      "message": "abcdefghijklmnopqrstuvwxyz0123456789 abcdefghijklmnopqrstuvwxyz$",
      "corrections": "0000000000000000200000000000010000000000000000000000000000000000"
    },
  }
  */

  char message[SCREEN_SIZE + 1];     // add one for '\0'
  char corrections[SCREEN_SIZE + 1]; // add one for '\0'

  for (size_t i = 0; i < SCREEN_SIZE; i++)
  {
    char c = desired->message[i];
    message[i] = c;
  }
  message[SCREEN_SIZE] = '\0';

  for (size_t i = 0; i < SCREEN_SIZE; i++)
  {
    char c = desired->corrections[i];
    corrections[i] = c;
  }
  corrections[SCREEN_SIZE] = '\0';

  JSONVar doc;
  doc["state"]["reported"]["message"] = message;
  doc["state"]["reported"]["corrections"] = corrections;

  String docString = JSON.stringify(doc);
  mqttClient.beginMessage("$aws/things/" MQTT_AWS_NAME "/shadow/update");
  mqttClient.print(docString);
  mqttClient.endMessage();

  Log.infoln("Published reported state message:");
  Log.infoln(docString.c_str());
  Log.infoln("");
}

void onMQTTMessage(int messageSize)
{
  lastMessageMillis = millis();

  String topic = mqttClient.messageTopic();
  Log.infoln("Received message on topic \"%s\" with size %d bytes", topic.c_str(), messageSize);

  String message = mqttClient.readString();
  MQTTDesiredState desired = parseDesiredStateMessage(message);

  if (desired.valid)
  {
    sendOverWire(&desired);
    publishReportedStateMessage(&desired);
  }
}

void requestDesiredState()
{
  Log.infoln("Requesting initial desired state");
  mqttClient.beginMessage("$aws/things/" MQTT_AWS_NAME "/shadow/get");
  mqttClient.print(millis());
  mqttClient.endMessage();
}

void connectWiFi()
{
  Log.infoln("Attempting to connect to WiFi network: ");
  Log.infoln(WIFI_SECRET_SSID);

  Log.info("");
  Log.clearPrefix();
  while (WiFi.begin(WIFI_SECRET_SSID, WIFI_SECRET_PASS) != WL_CONNECTED)
  {
    // failed, retry
    Log.info(".");
    delay(5000);
  }
  Log.infoln(" ");
  Log.setPrefix(printPrefix);

  Log.infoln("Connected to the network");
}

void connectMQTT()
{
  Log.infoln("Attempting to connect to MQTT broker: ");
  Log.infoln(MQTT_AWS_BROKER);

  Log.info("");
  Log.clearPrefix();
  while (!mqttClient.connect(MQTT_AWS_BROKER, 8883))
  {
    // failed, retry
    Log.info(".");
    delay(5000);
  }
  Log.infoln(" ");
  Log.setPrefix(printPrefix);

  Log.infoln("You're connected to the MQTT broker");
  digitalWrite(LED_BUILTIN, LOW); // light OFF when connected

  mqttClient.setKeepAliveInterval(AWS_MQTT_KEEPALIVE_INTERVAL);
  mqttClient.setConnectionTimeout(AWS_MQTT_CONNECTION_TIMEOUT);
  mqttClient.onMessage(onMQTTMessage);

  mqttClient.subscribe("$aws/things/" MQTT_AWS_NAME "/shadow/get/accepted");
  delay(100);
  mqttClient.subscribe("$aws/things/" MQTT_AWS_NAME "/shadow/update/accepted");
  delay(100);

  requestDesiredState();

  lastMessageMillis = millis();
  lastHealthCheckMillis = millis();
}

unsigned long getTime()
{
  return WiFi.getTime();
}

void setupSSL()
{
  // ECCX08 contains the generated private key
  if (!ECCX08.begin())
  {
    Log.errorln("No ECCX08 present!");
    while (1)
      ;
  }

  // Set a callback to get the current time
  // used to validate the servers certificate
  ArduinoBearSSL.onGetTime(getTime);

  // Set the ECCX08 slot to use for the private key
  // and the accompanying public certificate for it
  sslClient.setEccSlot(0, MQTT_AWS_PUBLIC_CERTIFICATE);
}

void loop()
{

  if (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(LED_BUILTIN, HIGH); // light ON if NOT connected to WiFi
    connectWiFi();
  }

  // A dead/NAT-dropped MQTT session can leave mqttClient.connected() reporting
  // true forever, since nothing ever tells the socket the remote end is gone.
  // Don't trust it alone - force a reconnect if we've heard nothing in too long.
  bool mqttStale = mqttClient.connected() && (millis() - lastMessageMillis > AWS_MQTT_INACTIVITY_TIMEOUT);

  if (!mqttClient.connected() || mqttStale)
  {
    if (mqttStale)
    {
      Log.warningln("No MQTT activity in over %lu ms, forcing reconnect", (unsigned long)(millis() - lastMessageMillis));
      mqttClient.stop();
    }
    digitalWrite(LED_BUILTIN, HIGH); // light ON if NOT connected to MQTT
    connectMQTT();
  }

  // poll for new MQTT messages and send keep-alive
  mqttClient.poll();

  // Actively probe liveness far more often than the 5-minute keep-alive so a
  // stale connection gets caught in ~1 minute instead of potentially never.
  if (millis() - lastHealthCheckMillis > AWS_MQTT_HEALTH_CHECK_INTERVAL)
  {
    requestDesiredState();
    lastHealthCheckMillis = millis();
  }

  delay(500);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
}

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(SERIAL_RATE);
  Serial1.begin(SERIAL_RATE);
  wireData.begin(Serial1);
  setupLogger(LOG_LEVEL);
  setupSSL();

  Log.infoln("Setup done.");
}
#include <DNSServer.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include "ESPAsyncWebServer.h"
#include <Preferences.h>
#include <ESP32Servo.h>
#include "LittleFS.h"
#include <ArduinoJson.h>
#include <cstring>
#include <string>
#include <esp_task_wdt.h>
#include <smartButton.h>

// PIN Defintions
#define PIN_BUTTON_UP 12
#define PIN_BUTTON_DOWN 13
#define PIN_SHIFTER_SERVO 14
#define BUTTON_DEBOUNCE_MS 50
// Set PIN_NEUTRAL_LED to 0 if no LED
#define PIN_NEUTRAL_LED 15

#define NVM_NAME_SPACE "AUTOSHIFTER"
#define WIFI_SSID "AutoShifter"
#define WIFI_PASSWORD "123456789"

// Hardware watchdog timeout in seconds
#define WDT_TIMEOUT 5

// Servo Object
Servo myservo;

// Smart button object
smartButton up_button(PIN_BUTTON_UP,INPUT_PULLUP,50);
smartButton down_button(PIN_BUTTON_DOWN,INPUT_PULLUP,50);


// NVM / Preferences
Preferences preferences;

// Forward declerations
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len);
void wsOnEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);

const char* wifi_ssid = WIFI_SSID;
const char* wifi_password = WIFI_PASSWORD;
IPAddress ip_address;
DNSServer dnsServer;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
StaticJsonDocument<2048> json_tx;

struct shifterState_t {
  bool hasFormUpdated = false;
  bool hasFromDefaults = false;
  const uint8_t defaultUpDegrees = 200;
  const uint8_t defaultNeutralDegrees = 130;
  const uint8_t defaultMidPointDegrees = 95;
  const uint8_t defaultDownDegrees = 10;
  const uint16_t defaultHoldDelay = 200;

  uint8_t upDegrees;
  uint8_t neutralDegrees;
  uint8_t midPointDegrees;
  uint8_t downDegrees;
  uint16_t holdDelay;
  uint8_t currentGearPositonId = 2; // Gear positon #2 = neutral
};
shifterState_t volatile shifterState;


enum pongType {STARTUP=1,USER=2,KEEPALIVE=3};

String templateProcessor(const String& paramName){
  // Process template
  if(paramName == "valueServoUpDegrees"){
    return String(shifterState.upDegrees);
  }
  if(paramName == "valueServoNeutralDegrees"){
    return String(shifterState.neutralDegrees);
  }
  if(paramName == "valueHoldDelay"){
    return String(shifterState.holdDelay);
  }
  if(paramName == "wsGatewayAddr"){
    // normally ws://192.168.4.1/ws
    return String("ws://") + ip_address.toString() + String("/ws");
  }
  if(paramName == "valueServoMidPointDegrees"){
    return String(shifterState.midPointDegrees);
  }
  if(paramName == "valueServoDownDegrees"){
    return String(shifterState.downDegrees);
  }
  if( paramName == "hasFormUpdated" ){
    bool hasFormUpdated = shifterState.hasFormUpdated;
    shifterState.hasFormUpdated = false;
    // return  hasFormUpdated?"true":"false";
    return String( hasFormUpdated );
  }
  if( paramName == "hasFromDefaults" ){
    bool hasFromDefaults = shifterState.hasFromDefaults;
    shifterState.hasFromDefaults = false;
    return  hasFromDefaults?"true":"false";
  }
  // We didn't find anything return empty string
  return String();
}

void processFormParamater( const String& fieldName, const String& fieldValue ){
  Serial.printf("field: %s = %s\n",fieldName,fieldValue);
  uint16_t intFieldValue = (uint16_t) fieldValue.toInt();
  if( fieldName == "servoUpDegrees" ){
    shifterState.upDegrees = (uint8_t) intFieldValue;
  }
  if( fieldName == "servoNeutralDegrees" ){
    shifterState.neutralDegrees = (uint8_t) intFieldValue;
  }
  if( fieldName == "servoMidPointDegrees" ){
    shifterState.midPointDegrees = (uint8_t) intFieldValue;
  }
  if( fieldName == "servoDownDegrees" ){
    shifterState.downDegrees = (uint8_t) intFieldValue;
  }
  if( fieldName == "holdDelay" ){
    shifterState.holdDelay = intFieldValue;
  }
}

void nvsWrite(){
  preferences.putUChar("upDegrees", shifterState.upDegrees);
  preferences.putUChar("neutralDegrees", shifterState.neutralDegrees);
  preferences.putUChar("midPointDegrees", shifterState.midPointDegrees);
  preferences.putUChar("downDegrees", shifterState.downDegrees);
  preferences.putUChar("holdDelay", shifterState.holdDelay);
}

void nvsRead(){
  if( preferences.isKey("upDegrees") == false ){
    // Empty name space ?, push defaults
    shifterState.upDegrees = shifterState.defaultUpDegrees;
    shifterState.midPointDegrees = shifterState.defaultMidPointDegrees;
    shifterState.neutralDegrees = shifterState.defaultNeutralDegrees;
    shifterState.downDegrees = shifterState.defaultDownDegrees;
    shifterState.holdDelay = shifterState.defaultHoldDelay;
    shifterState.hasFromDefaults = true;
    nvsWrite();
    return;
  }
  shifterState.upDegrees = preferences.getUChar("upDegrees");
  shifterState.neutralDegrees = preferences.getUChar("neutralDegrees");
  shifterState.midPointDegrees = preferences.getUChar("midPointDegrees");
  shifterState.downDegrees = preferences.getUChar("downDegrees");
  shifterState.holdDelay = preferences.getUChar("holdDelay");
}

String getGearPosText( uint8_t gearPosId ){
  String gearPosText = String("1N234567").substring(gearPosId-1,gearPosId);
  return gearPosText;
}

void wsSendGearUpdate(uint16_t pressedTime){
  char buffer[1000];
  uint8_t gearPosId = shifterState.currentGearPositonId;
  String gearPosText = getGearPosText(gearPosId);
  json_tx.clear();
  json_tx["messageType"] = "gearPosition";
  json_tx["payload"]["currentGearPosition"] = gearPosText;
  json_tx["payload"]["gearPosId"] = gearPosId;
  json_tx["payload"]["pressedTime"] = pressedTime;
  size_t lenJson = serializeJson(json_tx, buffer);
  ws.textAll(buffer,lenJson);
  Serial.printf("WS TX-> JSON %s\n",buffer);
  if( PIN_NEUTRAL_LED>0 ) digitalWrite( PIN_NEUTRAL_LED, gearPosText=="N" );
}

void checkGearChange(){
  uint16_t upPressed = up_button.pressTime();
  uint16_t downPressed = down_button.pressTime();
  if( downPressed>0 ){
    // Serial.printf("Change counter:%d Down Presstime:%d\n", down_button.changeCount, downPressed );
    myservo.write(shifterState.downDegrees);
    delay(shifterState.holdDelay);
    myservo.write(shifterState.midPointDegrees);
    if( shifterState.currentGearPositonId > 1 ){
      shifterState.currentGearPositonId--;
    }
    if( shifterState.currentGearPositonId == 2){
      Serial.println("Shift from 2nd to 1st skip neutral");
      shifterState.currentGearPositonId--;
    }
    Serial.printf("Change down to %s button press time:%d servo pos:%d\n",getGearPosText(shifterState.currentGearPositonId),downPressed,shifterState.downDegrees);
    wsSendGearUpdate(downPressed);
    return;
  }

  // Check to see if we are in first and need to shift to neutral
  if( upPressed>1500 && shifterState.currentGearPositonId==1 ){
    myservo.write(shifterState.neutralDegrees);
    delay(shifterState.holdDelay);
    myservo.write(shifterState.midPointDegrees);
    shifterState.currentGearPositonId++;
    Serial.printf("Change from 1st to neutral, do half shift button press time:%d servo pos:%d\n",upPressed,shifterState.neutralDegrees);
    wsSendGearUpdate(upPressed);
    return;
  }

  if( upPressed>0 ){
    // Serial.printf("Change counter:%d UP Presstime:%d\n", up_button.changeCount, upPressed );
    myservo.write(shifterState.upDegrees);
    delay(shifterState.holdDelay);
    myservo.write(shifterState.midPointDegrees);
    if( shifterState.currentGearPositonId <= 6 ){
      if( shifterState.currentGearPositonId == 1 ){
        // We shift pass neutral
        shifterState.currentGearPositonId++;
        Serial.println("Change up, skip neutral");
      }
      shifterState.currentGearPositonId++;
    }
    Serial.printf("Change up to %s button press time:%d servo pos:%d\n",getGearPosText(shifterState.currentGearPositonId),upPressed,shifterState.upDegrees);
    wsSendGearUpdate(upPressed);
    return;
  }
}

void server_routes(){
  // https://github.com/me-no-dev/ESPAsyncWebServer#handlers-and-how-do-they-work
  server.onNotFound([](AsyncWebServerRequest *request){
    request->redirect("/index.html");
  });

  server.on("/index.html", HTTP_ANY, [](AsyncWebServerRequest * request){
    if( request->method() == HTTP_POST ){
      int paramCount = request->params();
      if( paramCount> 0 ) shifterState.hasFormUpdated = true;
      Serial.printf("Number of params %d\n",paramCount);
      for( int i=0; i<paramCount; i++ ){
        AsyncWebParameter * param = request->getParam(i);
        processFormParamater(param->name(), param->value());
      }
      nvsWrite();
    }
    // Send response page using index.html via templateProcessor
    Serial.printf("Send index.html page\n");
    request->send(LittleFS,"/html/index.html","text/html",false,templateProcessor);
  });

  server.on("/setdefaults", HTTP_GET, [](AsyncWebServerRequest * request){
    preferences.clear();
    nvsRead();
    request->redirect("/index.html");
  });

  // Serve static files
  server.serveStatic("/", LittleFS,"/html/");

    // Websockets init.
  ws.onEvent(wsOnEvent);
  server.addHandler(&ws);
}



void setup(){
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
  Serial.begin(115200);
  preferences.begin( NVM_NAME_SPACE, false);
  nvsRead();

  if( PIN_NEUTRAL_LED > 0 ) pinMode(PIN_NEUTRAL_LED,OUTPUT);

  // Start file system  
  if(!LittleFS.begin()){
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }

  // Allow allocation of all timers
	ESP32PWM::allocateTimer(0);
	ESP32PWM::allocateTimer(1);
	ESP32PWM::allocateTimer(2);
	ESP32PWM::allocateTimer(3);
	myservo.setPeriodHertz(50);    // standard 50 hz servo
	myservo.attach(PIN_SHIFTER_SERVO, 500, 2400); // 500 - 2400 for 9G SG90
  pinMode(PIN_BUTTON_UP,INPUT_PULLUP);
  up_button.begin();
  down_button.begin();
  myservo.write(shifterState.midPointDegrees);
  shifterState.currentGearPositonId = 2; // Assume neutral on start
  wsSendGearUpdate(0);
  
  WiFi.softAP(wifi_ssid,wifi_password);
  ip_address = WiFi.softAPIP();
  Serial.println(ip_address);
  dnsServer.start(53, "*", ip_address );
  server_routes();

  // Start web server
  server.begin();
  Serial.print("Webserver started on IP:");
  Serial.println(ip_address);
}

void loop(){
  esp_task_wdt_reset();
  dnsServer.processNextRequest();
  checkGearChange();
  delay(50);
}

void wsOnEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
 void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      wsSendGearUpdate(0);
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA:
      // handleWebSocketMessage(arg, data, len);
      break;
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}
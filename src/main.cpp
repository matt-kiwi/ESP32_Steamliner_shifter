// Example see https://www.hackster.io/davidefa/esp32-lora-mesh-1-the-basics-3a0920
// TGO V32 Pinout https://github.com/umbm/TTGO-LoRa32-V2.1-T3_V1.6

// The unit has an Up and Down button. 
// Up goes up, Down goes down and if you are in 1st a long press will go to neutral.

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

// PIN Defintions
#define PIN_BUTTON_UP 12
#define PIN_BUTTON_DOWN 13
#define PIN_SHIFTER_SERVO 14
#define BUTTON_DEBOUNCE_MS 50

#define NVM_NAME_SPACE "AUTOSHIFTER"
#define WIFI_SSID "AutoShifter"
#define WIFI_PASSWORD "123456789"


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
  uint8_t upDegrees;
  uint8_t neutralDegrees;
  uint8_t midPointDegrees;
  uint8_t downDegrees;
  char currentGearPosition = 'N';
};
shifterState_t shifterState;


enum pongType {STARTUP=1,USER=2,KEEPALIVE=3};

String templateProcessor(const String& paramName){
  // Process template
  if(paramName == "valueServoUpDegrees"){
    return String(shifterState.upDegrees);
  }
  if(paramName == "valueServoNeutralDegrees"){
    return String(shifterState.neutralDegrees);
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
    return  hasFormUpdated?"true":"false";
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
  u_int8_t intFieldValue = (u_int8_t) fieldValue.toInt();
  if( fieldName == "servoUpDegrees" ){
    shifterState.upDegrees = intFieldValue;
  }
  if( fieldName == "servoNeutralDegrees" ){
    shifterState.neutralDegrees = intFieldValue;
  }
  if( fieldName == "servoMidPointDegrees" ){
    shifterState.midPointDegrees = intFieldValue;
  }
  if( fieldName == "servoDownDegrees" ){
    shifterState.downDegrees = intFieldValue;
  }
}

void nvsWrite(){
  preferences.putUChar("upDegrees", shifterState.upDegrees);
  preferences.putUChar("neutralDegrees", shifterState.neutralDegrees);
  preferences.putUChar("midPointDegrees", shifterState.midPointDegrees);
  preferences.putUChar("downDegrees", shifterState.downDegrees);
}

void nvsRead(){
  if( preferences.isKey("upDegrees") == false ){
    // Empty name space ?, push defaults
    shifterState.upDegrees = shifterState.defaultUpDegrees;
    shifterState.midPointDegrees = shifterState.defaultMidPointDegrees;
    shifterState.neutralDegrees = shifterState.defaultNeutralDegrees;
    shifterState.downDegrees = shifterState.defaultDownDegrees;
    shifterState.hasFromDefaults = true;
    nvsWrite();
    return;
  }
  shifterState.upDegrees = preferences.getUChar("upDegrees");
  shifterState.neutralDegrees = preferences.getUChar("neutralDegrees");
  shifterState.midPointDegrees = preferences.getUChar("midPointDegrees");
  shifterState.downDegrees = preferences.getUChar("downDegrees");
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

void wsSendGearUpdate(){
  char buffer[1000];
  json_tx.clear();
  json_tx["messageType"] = "gearPosition";
  json_tx["payload"]["currentGearPosition"] = &shifterState.currentGearPosition;
  size_t lenJson = serializeJson(json_tx, buffer);
  ws.textAll(buffer,lenJson);
  // Serial.printf("WS TX-> JSON %s\n",buffer);
}

void setup(){
  Serial.begin(115200);
  preferences.begin( NVM_NAME_SPACE, false);
  nvsRead();

  // Start file system  
  if(!LittleFS.begin()){
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }
  
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
  dnsServer.processNextRequest();
  delay(50);
}

void wsOnEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
 void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
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
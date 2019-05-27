#include "config.h" // System specific configuration
#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoWebsockets.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
//#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP_EEPROM.h>

const int END_OF_DAY = 60 * 24 + 1;

#define timeOffset 0
#define NTP_UPDATE_INTERVAL 3600000
#define ONE_WIRE_BUS  D1
#define GPI_BOILER    D2
#define GPI_PUMP      D3
#define GPI_LED       D4
#define GPI_BUTTON    D5
#define MAX_TIMERS    10

enum eventType {
  EVENT_NONE  = 0,
  EVENT_WATER = 1,
  EVENT_HEAT  = 2
};

struct timerEvent {
  byte days = 0; //bitwise flag of days timer occurs (bit 0 = Sunday)
  unsigned int start = 0; //minutes since midnight that timer on event occurs
  unsigned int end = 0; //minutes since midnight that timer off event occurs
  byte type = EVENT_NONE; //Event type (see eventTypes)
};

bool bPump        = false;
bool bBoiler      = false;
bool bHeating     = false;
bool bButton      = false;
int  nAdc         = 0;
unsigned int nNextMinute  = 0;
long lNextHour    = 0;
float nWaterTemp  = 55;
float nRoomTemp   = 20;
timerEvent aTimers[MAX_TIMERS];
unsigned int nNextEventTimer = MAX_TIMERS;
unsigned int nNextEventTime = END_OF_DAY;
bool bNextEventStart = false;
byte nDayOfWeek = 0;
bool bTimezoneSet = false;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", timeOffset, NTP_UPDATE_INTERVAL);
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature DS18B20(&oneWire);

void setup(){
  Serial.begin(115200);
  Serial.println();
  Serial.println("riban HAVC starting up...");
  Serial.print("Configuring GPI... ");
  pinMode(GPI_BOILER, OUTPUT);
  pinMode(GPI_PUMP, OUTPUT);
  pinMode(GPI_BUTTON, INPUT_PULLUP);
  pinMode(GPI_LED, OUTPUT);
  digitalWrite(GPI_BOILER, bBoiler);
  digitalWrite(GPI_PUMP, bPump);
  digitalWrite(GPI_LED, !bHeating);
  Serial.println("SUCCESS");

  Serial.print("Enable EEPROM(");
  Serial.print(2 + sizeof(aTimers));
  Serial.print(")...");
  EEPROM.begin(2 + sizeof(aTimers));
  byte nTest = 0;
  EEPROM.get(0, nTest);
  if(nTest != 0x42) {
    initialiseEEPROM();
  } else {
    EEPROM.get(1, nTest);
    if(nTest != 0x57)
      initialiseEEPROM();
  }
  loadEEPROM();
  Serial.println("SUCCESS");

  Serial.print("Disable AP... ");
  WiFi.softAPdisconnect(true);
  Serial.println("SUCCESS");
  
  Serial.print("Connecting to WiFi... ");
  WiFi.begin(SSID, PASSCODE);
  WiFi.hostname(HOSTNAME);

  while (WiFi.status() != WL_CONNECTED) {
    delay (500);
    Serial.print (".");
  }
  Serial.println(" SUCCESS");
  
  Serial.print("Starting NTP client... ");
  timeClient.begin();
  MDNS.begin(HOSTNAME);
  Serial.println("SUCCESS");
  
  Serial.print("Starting HTTP server... ");
  httpUpdater.setup(&httpServer);
  httpServer.begin();
  httpServer.on("/", handleRoot);
  httpServer.on("/toggleHeat", handleToggleHeat);
  httpServer.on("/save", handleSave);
  httpServer.on("/config", handleConfig);
  httpServer.onNotFound(handleNotFound);
  Serial.println("SUCCESS");

  MDNS.addService("http", "tcp", 80);

  Serial.print("Starting DS18B20... ");
  DS18B20.begin(); 
  Serial.println("SUCCESS");
}

void loop() {
  timeClient.update();
  long lNow = timeClient.getEpochTime();
  if(lNow >= nNextMinute)
    handleMinute(lNow);
  httpServer.handleClient();
  MDNS.update();
  
  if(digitalRead(GPI_BUTTON) == bButton) {
    bButton = !bButton;
    //bButton is true when button is pressed
    if(bButton) {
      setHeating(!bHeating);
    }
    delay(50); //simple debounce
  }
  delay(10); //Avoid tight loop
}

void handleRoot() {
  String sHtml("<html><head><title>riban HAVC</title></head><body><h1>Heating Control</h1><p><form action=/toggleHeat method=post><input type=submit value='Toggle Heat'></form>Heating: ");
  sHtml += String(bHeating?"ON":"OFF");
  sHtml += String("</p><form action=/save method=post><table><tr><th>On</th><th>Off</th><th>S</th><th>M</th><th>T</th><th>W</th><th>T</th><th>F</th><th>S</th></tr>");
  String sChecked(" checked/>");
  for(unsigned int i = 0; i < MAX_TIMERS; ++i) {
    sHtml += String("<tr><td><input type=time name=St") + String(i) + String(" value='") + getTime(aTimers[i].start) + 
      String("'></td><td><input type=time name=En") + String(i) + String(" value='") + getTime(aTimers[i].end) +
      String("'></td><td><input type=checkbox name=Su") + String(i) + ((aTimers[i].days & 1)?sChecked : String("/>")) +
      String("</td><td><input type=checkbox name=Mo") + String(i) + ((aTimers[i].days & 2)?sChecked : String("/>")) +
      String("</td><td><input type=checkbox name=Tu") + String(i) + ((aTimers[i].days & 4)?sChecked : String("/>")) +
      String("</td><td><input type=checkbox name=We") + String(i) + ((aTimers[i].days & 8)?sChecked : String("/>")) +
      String("</td><td><input type=checkbox name=Th") + String(i) + ((aTimers[i].days & 16)?sChecked : String("/>")) +
      String("</td><td><input type=checkbox name=Fr") + String(i) + ((aTimers[i].days & 32)?sChecked : String("/>")) +
      String("</td><td><input type=checkbox name=Sa") + String(i) + ((aTimers[i].days & 64)?sChecked : String("/>")) +
      String("</td></tr>");
  }
  sHtml += String("</table><input type=submit value=Save></form><p>");
  if(nNextEventTime == END_OF_DAY)
    sHtml += String("No more events today");
  else {
    sHtml += String("Next event: Heating ") + (bNextEventStart?String("on at "):String("off at ")) + getTime(nNextEventTime);
  }
  sHtml += String("</p><p><a href=/update>Update</a> <a href=/config>Configuration</a></p></body></html>");
  httpServer.send(200, "text/html", sHtml.c_str());
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += httpServer.uri();
  message += "\nMethod: ";
  message += (httpServer.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += httpServer.args();
  message += "\n";
  for (uint8_t i = 0; i < httpServer.args(); i++) {
    message += " " + httpServer.argName(i) + ": " + httpServer.arg(i) + "\n";
  }
  httpServer.send(404, "text/plain", message);
}

void handleMinute(long lNow) {
  // Triggered each minute at hh:mm:00
  Serial.print("Minute event at ");
  Serial.println(timeClient.getFormattedTime());

  unsigned int nNow = (lNow / 60) % (60 * 24);
  Serial.printf("nNow: %s nNextEventTime: %s\n", getTime(nNow).c_str(), getTime(nNextEventTime).c_str());
  if(nNextEventTime == nNow)
    processEvents(nNow);

  updateNextEvent();
  nNextMinute = (lNow / 60 + 1) * 60;

  DS18B20.requestTemperatures();
  nAdc = analogRead(A0);
  Serial.printf("ADC: %d\n", nAdc);

  
  if(lNow >= lNextHour)
    handleHour();
  else if(!bTimezoneSet)
    getTimezone();
}

void handleHour() {
  //Triggered each hour at hh:00:00
  Serial.println("Hour event");
  getTimezone();
  lNextHour = ((timeClient.getEpochTime() / 3600) + 1) * 3600;
  nDayOfWeek = (timeClient.getEpochTime() / 86400L + 4) % 7;
}

void getTimezone() {
  Serial.println("getTimezone");
  WiFiClient client;
  HTTPClient http;
  String sApi(F("http://api.timezonedb.com/v2.1/get-time-zone?key="));
  sApi += String(TIMEZONEDB_KEY);
  sApi += String(F("&by=zone&zone=Europe/London&format=json"));
  if(http.begin(client, sApi.c_str())) {
    int httpCode = http.GET();
    if (httpCode > 0) {
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        Serial.print(F("api.timezonedb.com response: "));
        Serial.println(http.getString());

        DynamicJsonDocument doc(512);
        DeserializationError error = deserializeJson(doc, http.getString());
        if (error) {
          Serial.print(F("deserializeJson() failed: "));
          Serial.println(error.c_str());
          return;
        }
        // Extract values
        Serial.printf("timezonedb status: %s\n", doc["status"].as<String>().c_str());
        if(doc["status"].as<String>() == "OK") {
          timeClient.setTimeOffset(doc["gmtOffset"].as<int>());
          bTimezoneSet = true;
        }
      }
    }
    http.end();
  } else {
    Serial.println(F("ERROR: Failed to connect to http://api.timezonedb.com"));
  }
}

void handleToggleHeat() {
  setHeating(!bHeating);
  httpServer.sendHeader("Location","/");
  httpServer.send(303);
}

void handleSave() {
  for(int i = 0; i < MAX_TIMERS; ++i) {
    aTimers[i].days = 0;
  }
  for(int i=0; i<httpServer.args();++i) {
    String sStub = httpServer.argName(i).substring(0,2);
    int nTimer = httpServer.argName(i).substring(2).toInt();
    if(nTimer < MAX_TIMERS) {
      if(sStub == "Su") aTimers[nTimer].days |= 1;
      else if(sStub == "Mo") aTimers[nTimer].days |= 2;
      else if(sStub == "Tu") aTimers[nTimer].days |= 4;
      else if(sStub == "We") aTimers[nTimer].days |= 8;
      else if(sStub == "Th") aTimers[nTimer].days |= 16;
      else if(sStub == "Fr") aTimers[nTimer].days |= 32;
      else if(sStub == "Sa") aTimers[nTimer].days |= 64;
      else if(sStub == "St") aTimers[nTimer].start = httpServer.arg(i).substring(0,2).toInt() * 60 + httpServer.arg(i).substring(3).toInt();
      else if(sStub == "En") aTimers[nTimer].end = httpServer.arg(i).substring(0,2).toInt() * 60 + httpServer.arg(i).substring(3).toInt();
    }
  }
  saveEEPROM();
  updateNextEvent();
  httpServer.sendHeader("Location","/");
  httpServer.send(303);
}

void handleConfig() {
  byte nCount = DS18B20.getDeviceCount();
  byte nAddress;
  
  String sHtml("<html><head><title>riban HAVC - config</title></head><body><h1>Heating Control - Config</h1><p>");
  sHtml += String("</p><form action=/save method=post><table><tr><th>ID</th><th>Temp</th><th>Location</th><th>Min</th><th>Max</th><th>Enable</th></tr>");
  String sChecked(" checked/>");
  for(unsigned int i = 0; i < nCount; ++i) {
    DS18B20.getAddress(&nAddress, i);
    float fTemp = DS18B20.getTempCByIndex(i);
    sHtml += String("<tr><td>") + String(nAddress, HEX) + String("</td><td>") + String(fTemp) + String("</td><td><input type=text name=Lo") + String(i) + String(" value='") + String("???") + 
      String("<tr><td><input type=text name=Mi") + String(i) + String(" value='") + String("???") + 
      String("<tr><td><input type=text name=Ma") + String(i) + String(" value='") + String("???") +       
      String("'></td><td><input type=checkbox name=En") + String(i) + ((false)?String(" checked"): String("")) +
      String(" /></td></tr>");
  }
  sHtml += String("</table><input type=submit value=Save></form></body></html>");
  httpServer.send(200, "text/html", sHtml.c_str());
}

void initialiseEEPROM() {
  byte nByte = 0x42;
  EEPROM.put(0, nByte);
  nByte = 0x57;
  EEPROM.put(1, nByte);
  saveEEPROM();
}

void saveEEPROM() {
  for(int i = 0; i < MAX_TIMERS; ++i) {
    EEPROM.put(2 + i * sizeof(timerEvent), aTimers[i]);
  }
  EEPROM.commit();
}

void loadEEPROM() {
  for(int i = 0; i < MAX_TIMERS; ++i) {
    EEPROM.get(2 + i * sizeof(timerEvent), aTimers[i]);
  }
}

String getTime(unsigned int nTime) {
  int min = nTime % 60;
  int hr = nTime / 60;
  char sTime[6];
  snprintf(sTime, 6, "%02d:%02d\0", hr, min);
  return String(sTime);
}

void updateNextEvent() {
  unsigned int nNow = (timeClient.getEpochTime() / 60) % (60 * 24);
  nNextEventTime = END_OF_DAY;
  nNextEventTimer = MAX_TIMERS;

  byte nDoWflag = 1 << nDayOfWeek;
  for(int i = 0; i < MAX_TIMERS; ++i) {
    if((nDoWflag & aTimers[i].days) && (aTimers[i].end > nNow) && (aTimers[i].end < nNextEventTime)) {
      nNextEventTime = aTimers[i].end;
      nNextEventTimer = i;
      bNextEventStart = false;
    }
    if((nDoWflag & aTimers[i].days) && (aTimers[i].start > nNow) && (aTimers[i].start < nNextEventTime)) {
      nNextEventTime = aTimers[i].start;
      nNextEventTimer = i;
      bNextEventStart = true;
    }
  }
}

void processEvents(unsigned int nMinute) {
  Serial.println("processEvents");
  for(unsigned int i = 0; i < MAX_TIMERS; ++i) {
    if(aTimers[i].end == nMinute)
      setHeating(false);
    else if(aTimers[i].start == nMinute)
      setHeating(true);
  }
}

void setHeating(bool bEnable) {
  bHeating = bEnable;
  digitalWrite(GPI_LED, !bHeating);
}

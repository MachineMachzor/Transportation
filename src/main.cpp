#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <driver/ledc.h>
#include <algorithm>  // for std::find
#include <iterator>   // for std::begin/​end
#include <Wire.h>
#include <EEPROM.h>
// #include <esp32cam.h>
#include <SPI.h>
#include <vector>
#include <Preferences.h>
#include "camera_index.h"          // pulls in your gzipped HTML


Preferences prefs;
const bool TESTING_NEXTION = true;

Stream *dbgSerial = nullptr;     // for debug output to PC
Stream *nextionSerial = nullptr; // for Nextion commands


const uint32_t USB_BAUD = 115200; //115200;   // PC debug
const uint32_t NEXTION_BAUD = 9600; // Nextion


// bool VERBOSE = true;


// #include <HTTPClient.h>

// For testing
char* ssidTest     = "NETGEAR26";
char* passwordTest = "melodicpanda708";

struct WifiCredentials {
  String ssid;
  String pass;
  bool ok = false;
};

struct keys {
  String ssid = "SSID";
  String pass = "PASS";
  String startAddr = "START";
  String endAddr = "END";
  String walkTime = "WALKTIME";
};

int port = 80;
WebServer server(port); // serve on port
const keys CONST_KEYS;

const char* EMPTY_VALUE = "";

const unsigned long CONNECT_TIMEOUT = 10000; // ms

// Save setting
void saveSetting(const char* settingName, const char* settingValue) {
  prefs.begin("cfg", false);            // namespace "cfg", RW, specified by false
  prefs.putString(settingName, settingValue);
  prefs.end();
}

// Load (with defaults)
String loadStringSetting(const char* key, const char* defaultVal = "") {
  prefs.begin("cfg", true);   // read-only, specified by True
  String v = prefs.getString(key, defaultVal);
  prefs.end();
  return v;
}


String logBuffer = "";
static std::vector<String> logLines;  
static int holdMessageCount = 8;  // holds up to N messages

void logMessage(const String& msg) {
  logBuffer = "";
  logLines.push_back(msg);
  if (logLines.size() > holdMessageCount) {
    logLines.erase(logLines.begin());
  }

  // logBuffer += msg + "\n";
  for (auto &line : logLines) {
    logBuffer += line + "\n";
  }

  dbgSerial->println(msg);
  if (logBuffer.length() > 1024) logBuffer = logBuffer.substring(512);  // keep it trimmed
}


void handleIndex() {
  // inform browser this is gzipped HTML
  server.sendHeader("Content-Encoding", "gzip");
  // note: send_P lets us pass a pointer+len to flash data
  server.send_P(200, "text/html",
                (const char*)index_ov2640_html_gz,
                index_ov2640_html_gz_len);
}

void handleLogs() {
  server.send(200, "text/plain", logBuffer);
}


/*
Go to NoWifi home page and select a wifi IF
1. No wifi prior in credentials
2. Prior wifi in credentials is NOT on the list of wifi networks

If prior credentials found, and in wifi network, try to login with that.
1. If successful, AND prior Homepage info found, go to InfoPage
2. If successful, AND no homepage info found, go to homepage
3. If failed, go to NoWifi Page

Homepage should have start location (optional, would use current if blank), end location (it'll generate the map from here internally), then give the option to select a mode of transport.
1. If Bus for ex, select ones choice of the first bus to go to
Have walk time/time to get to first transport as optional as well, it'll use maps default if none shown. Calculate time to leave accordingly

*/

void sendCommand(const String &cmd) {
  String c = cmd;
  c.trim();                          // remove stray CR/LF
  dbgSerial->println("TX->Nextion: " + c); // debug to USB Serial
  // Serial1.write((const uint8_t*)c.c_str(), c.length());
  // Serial1.write(0xFF); Serial1.write(0xFF); Serial1.write(0xFF);
  // Serial1.flush();                   // wait for TX buffer to drain
  // Serial1 for prod, Serial for testing with USB Serial monitor
  nextionSerial->write((const uint8_t*)c.c_str(), c.length());
  nextionSerial->write(0xFF); nextionSerial->write(0xFF); nextionSerial->write(0xFF);
  nextionSerial->flush();                   // wait for TX buffer to drain
  delay(50);                         // let Nextion process



  // nextionSerial->print(cmd);
  // nextionSerial->write(0xFF); nextionSerial->write(0xFF); nextionSerial->write(0xFF);
  // delay(10);
}


void debugHex(const String &s) {
  dbgSerial->print("HEX: ");
  for (size_t i = 0; i < s.length(); ++i) {
    dbgSerial->printf("%02X ", (uint8_t)s[i]);
  }
  dbgSerial->println(" FF FF FF");
}


bool tryWifi(const char* ssid, const char* pass) {
  WiFi.begin(ssid, pass);
  bool connected = false;
  for (int i = 0; i < 15; i++) {
    connected = WiFi.status() == WL_CONNECTED;
    if (connected) {
      break;
    } 
    delay(200);
    

  }
  return connected;

}

void handleNextionPacket(uint8_t *p, int len) {
  if (len <= 0) return;
  uint8_t type = p[0];
  if (type == 0x65 && len >= 4) {           // touch event
    uint8_t page = p[1];
    uint8_t comp = p[2];
    uint8_t event = p[3];                   // 0=press,1=release
    nextionSerial->printf("Touch page=%d comp=%d ev=%d\n", page, comp, event);
  } else if (type == 0x70) {                // string response
    String s = String((char*)&p[1]);
    nextionSerial->println("Nextion string: " + s);
  } else if (type == 0x71 && len >= 5) {    // number response
    long val = (p[1]<<24) | (p[2]<<16) | (p[3]<<8) | p[4];
    nextionSerial->printf("Nextion number: %ld\n", val);
  } else {
    nextionSerial->println("Unknown packet");
  }
}

std::vector<String> connectionSequence() {
  int n = WiFi.scanNetworks();
  WifiCredentials result;
  // String wifiList = "";
  std::vector<String> wifiList;
    
  for (int i = 0; i < min(n,5); ++i) {               // send first 5 rows min(n,5)
    String s = WiFi.SSID(i);
    // sendCommand("t" + String(i) + ".txt=\"" + s + "\""); // assumes t0..t4 text fields on Nextion, this is why we may want to limit it to 5 only
    dbgSerial->println(s);
    wifiList.push_back(s);
    // wifiList += s;
  }
  // Realistically sleep wait until Nextion calls back with a submission attempt for this, and also send creds to Nextion
  // WiFi.begin(ssidTest, passTest);
  
  return wifiList;
}

String joinWithNewline(const std::vector<String>& v) {
  String out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += '\n';   // add newline before every element except the first
    out += v[i];
  }
  return out;
}



void setup() {
  // put your setup code here, to run once:
  Serial.begin(USB_BAUD);
  // Serial.println("hello");

  WiFi.mode(WIFI_STA); //Client/station mode.
  // WiFi.begin(ssid, password);


  // example: RX=16, TX=17
  Serial1.begin(NEXTION_BAUD, SERIAL_8N1, 16, 17); // ESP32 hardware UART
  // send a Nextion command (must end with 0xFF 0xFF 0xFF)
  // Serial1.print("t0.txt=\"Hello\"\xFF\xFF\xFF");
  bool connected = false;

  if (TESTING_NEXTION) {
    // Editor on PC listens to USB serial, so send Nextion commands to Serial.
    nextionSerial = &Serial;
    // keep debug off the USB to avoid polluting the Editor; send debug to Serial1 (not connected)
    dbgSerial = &Serial1;
  } else {
    // production: Nextion is on Serial1 pins, debug goes to USB Serial
    nextionSerial = &Serial1;
    dbgSerial = &Serial;
  }

  // Clear console of prior messages
  sendCommand("");


  
  WiFi.disconnect(); 
  delay(100);   //Would remove prior connections, it stores it by default, could check to see if it's connected off the bat
  WifiCredentials creds;
  creds.ssid = loadStringSetting(CONST_KEYS.ssid.c_str());
  creds.pass = loadStringSetting(CONST_KEYS.pass.c_str());
  // Wifi may not have a password
  if (creds.ssid.length() == 0) {
    logMessage("No prior SSID");
  } else {
    logMessage("Prior SSID, try to connect to wifi");
    connected = tryWifi(creds.ssid.c_str(), creds.pass.c_str());
    creds.ok = connected;
    // Serial.printf("Did prior login save allow connection to wifi? creds.ok: %s\n", creds.ok ? "true" : "false");
  }

  if (creds.ok) {
    
  } else {
    logMessage("No prior wifi");
  }

  // Serial.println(creds);
  
  // WiFi.begin();
  // WiFi.reconnect(); //Try to store credentials?
  
  
  // Send command to Nextion to show wifi networks

  // connected = false;
  if (!connected) {
    logMessage("Finding WIFI as new");
    std::vector<String> wifiList = connectionSequence();
    logMessage("Scanned wifi networks:\n" + joinWithNewline(wifiList));
    saveSetting(CONST_KEYS.ssid.c_str(), ssidTest);
    saveSetting(CONST_KEYS.pass.c_str(), passwordTest);
    bool connected = tryWifi(ssidTest, passwordTest);
    creds.ok = connected;
    creds.ssid = ssidTest;
    creds.pass = passwordTest;
    connected = creds.ok;
    String msg;
    msg = "Found wifi and logged in, did it work? creds.ok: ";
    msg += creds.ok ? "true" : "false";
    logMessage(msg);

  }
  // Realistically we'll want to wait for a command to select to wifi then try again
  // connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    logMessage("Connected to Wi-Fi");
    // sendCommand("page HomePage");  // go to main page
    // sendCommand("page HomePage");  
    // sendCommand("page HomePage");  
    // sendCommand("page HomePage");  
    sendCommand("page HomePage"); 
    
    // sendCommand("page 1");
    // debugHex("page HomePage");
  }
  
  server.on("/",         HTTP_GET, handleIndex);
  server.on("/logs", HTTP_GET, handleLogs);
  server.begin();
  dbgSerial->printf("Connected MAIN SERVER, IP = %s\n", WiFi.localIP().toString().c_str());
  logMessage("HTTP server running, ready for commands.");
}

void loop() {
  // put your main code here, to run repeatedly:

  // while (Serial1.available()) {
  //   uint8_t c = Serial1.read();
  //   buf[idx++] = c;
  //   // check for terminator 0xFF 0xFF 0xFF
  //   if (idx >= 3 && buf[idx-1]==0xFF && buf[idx-2]==0xFF && buf[idx-3]==0xFF) {
  //     int len = idx - 3;       // payload length
  //     handleNextionPacket(buf, len);
  //     idx = 0;
  //   }
  // }
  server.handleClient(); 


}


// Run script in monitor
// cd C:\Users\ringk\OneDrive\Documents\PlatformIO\Projects\Transportation_IO
// pio device monitor -p COM4 -b 115200 --filter esp32_exception_decoder

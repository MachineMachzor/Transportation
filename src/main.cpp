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
#include <map>
#include <string>


Preferences prefs;
const bool TESTING_NEXTION = true; //If false, should be production nextion
const bool FAKE_NO_WIFI = true; //If true, always go to no wifi page for testing

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

struct buttonText {
  int compId;
  String text;
};

struct keys {
  String ssid = "SSID";
  String pass = "PASS";
  String startAddr = "START";
  String endAddr = "END";
  String walkTime = "WALKTIME";
};

struct login_errors {
  String no_wifi = "No Wifi";
  String no_pass = "No Password";
  String bad_login = "Bad Login";
};

login_errors login_errors_const;


std::map<int, String> NO_WIFI_PAGE_MAP = {
    {2, "b0"},
    {3, "b1"},
    {4, "b2"},
    {5, "b3"},
    {6, "b4"},
    {7, "b5"}
};

// std::map<int, String> WIFI_INPUT_MAP = {
//     {2, "b0"},
//     {3, "b1"},
//     {4, "b2"},
//     {5, "b3"},
//     {6, "b4"},
//     {7, "b5"}
// };




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
  // logMessage("TX->Nextion: " + c); // debug to USB Serial
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

std::vector<uint8_t> readNextionPacket(Stream &s, unsigned long timeoutMs = 3000) {
  std::vector<uint8_t> buf;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (s.available()) {
      // server.handleClient(); 
      uint8_t b = s.read();
      buf.push_back(b);
      size_t n = buf.size();
      if (n >= 3 && buf[n-1]==0xFF && buf[n-2]==0xFF && buf[n-3]==0xFF) {
        // strip terminators for payload convenience
        buf.resize(n-3);
        return buf;
      }
      
    }
    delay(1);
    // server.handleClient(); // keep server responsive while waiting
  }
  // server.handleClient(); 
  return {}; // empty = timeout/no packet
}

// map Nextion component IDs to your button indices (fill with your IDs)
const int compIdToButtonIndex[] = { -1, -1, /* index by component id */ };


int waitForButtonPress(Stream &nx, unsigned long timeoutMs = 10000) {
  auto pkt = readNextionPacket(nx, timeoutMs);
  if (pkt.empty()) return -1;
  // Nextion touch events begin with 0x65; payload layout: 0x65, eventType, componentId...
  // Confirm header then return component id (pkt[2] if present)
  if (pkt.size() >= 3 && pkt[0] == 0x65) {
    return (int)pkt[2]; // component id
  }
  return -1;
}


String getButtonText(Stream &nx, unsigned long timeoutMs = 10000) {
  
  auto pkt = readNextionPacket(nx, timeoutMs);
  String text;
  if (!pkt.empty() && pkt[0] == 0x70) {
      for (size_t i = 1; i < pkt.size(); i++) {
          if (pkt[i] == 0xFF) break;
          text += (char)pkt[i];
      }
  }
  return text;
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


void sendComponentTxt(int btnCount, int txtTruncateLength, std::vector<String> txtList, String componentType="b") {
  // b = btn, t = txt
  for (size_t i = 0; i < min(btnCount, int(txtList.size())); ++i) {
    String txt = txtList[i];
    if (txt.length() > txtTruncateLength) {
      txt = txt.substring(0, txtTruncateLength); // truncate if too long
    }
    sendCommand(componentType + String(i) + ".txt=\"" + txt + "\"");
  }
}

String joinWithNewline(const std::vector<String>& v) {
  String out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += '\n';   // add newline before every element except the first
    out += v[i];
  }
  return out;
}



// ROUTE: DecisionPage --> NoWifiPage --> WifiInput --> HomePage
buttonText SelectWifi() {
  buttonText bt;
  sendCommand("page NoWifiPage"); 
  logMessage("Finding WIFI as new");
  // delay(1000); // let Nextion switch pages
  std::vector<String> wifiList = connectionSequence();
  sendComponentTxt(5, 20, wifiList, "b"); // send first 5 networks to buttons, truncate to 20 chars
  logMessage("Scanned wifi networks:\n" + joinWithNewline(wifiList));
  int compId = -1;
  String text = "";
  while (compId == -1 || text.length() == 0 || text == "Loading...") {
    logMessage("Waiting for button press during wifi selection...");
    compId = waitForButtonPress(*nextionSerial, 10000);
    logMessage("Button pressed, compId: " + String(compId));
    // sendCommand("get b" + String(compId) + ".txt"); // get text of button pressed
    sendCommand("get " + NO_WIFI_PAGE_MAP[compId] + ".txt");
    text = getButtonText(*nextionSerial); // flush any prior response
    // auto pkt = readNextionPacket(*nextionSerial, 10000);
    // String text;
    // if (!pkt.empty() && pkt[0] == 0x70) {
    //     for (size_t i = 1; i < pkt.size(); i++) {
    //         if (pkt[i] == 0xFF) break;
    //         text += (char)pkt[i];
    //     }
    // }

    if (text.length() == 0) {
      logMessage("No text received from Nextion for selected wifi.");
    } else {
      logMessage("Selected wifi SSID: " + text);
    }
  }

  logMessage("Final SSID selected: " + text);
  bt.compId = compId;
  bt.text = text;
  return bt;
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
  saveSetting(CONST_KEYS.ssid.c_str(), ssidTest);
  saveSetting(CONST_KEYS.pass.c_str(), passwordTest);
  delay(100);   //Would remove prior connections, it stores it by default, could check to see if it's connected off the bat
  WifiCredentials creds;
  if (FAKE_NO_WIFI) { // This is for testing really
    creds.ssid = loadStringSetting(CONST_KEYS.ssid.c_str());
    creds.pass = loadStringSetting(CONST_KEYS.pass.c_str());
  }
  
  // Wifi may not have a password
  
  if (creds.ssid.length() == 0) { //|| !FAKE_NO_WIFI
    logMessage("No prior SSID");
  } else {
    logMessage("Prior SSID, try to connect to wifi");
    connected = tryWifi(creds.ssid.c_str(), creds.pass.c_str());
    creds.ok = connected;
    
    // Serial.printf("Did prior login save allow connection to wifi? creds.ok: %s\n", creds.ok ? "true" : "false");
  }
  



  // Serial.println(creds);
  
  // WiFi.begin();
  // WiFi.reconnect(); //Try to store credentials?
  
  
  // Send command to Nextion to show wifi networks

  connected = false;
  if (!connected) {
    buttonText bt = SelectWifi();
    String text = bt.text;
    int compId = bt.compId;
    
    // After selecting wifi, go to the next page
    sendCommand("page WifiInput");
    
    if (text != "Unlisted") {
      // ssidTest = (char*)text.c_str();
      sendCommand("t2.txt=\"" + text + "\""); // set SSID field
    }


    compId = -1;
    String username;
    String password;
    creds = WifiCredentials();

    
    while (compId == -1){ //|| username.length() == 0 || password.length() == 0 || !creds.ok) {
      creds = WifiCredentials();
      logMessage("Waiting for button press during wifi login...");
      compId = waitForButtonPress(*nextionSerial, 10000);
    }
    logMessage("Login Button pressed, compId: " + String(compId));
    

    sendCommand("get t2.txt");
    username = getButtonText(*nextionSerial); // flush any prior response
    sendCommand("get t4.txt");
    password = getButtonText(*nextionSerial);

    logMessage("Received wifi credentials from Nextion: SSID: " + username + ", Password: " + password);
    bool hasError = false;
    if (username.length() == 0) {
      logMessage("No SSID entered, cannot connect to wifi.");
      sendCommand("errorTxt.txt=\"" + login_errors_const.no_wifi + "\"");
      hasError = true;
    } else if (password.length() == 0) {
      logMessage("No Password entered, cannot connect to wifi.");
      sendCommand("errorTxt.txt=\""+login_errors_const.no_pass + "\"");
      hasError = true;
    } 

    if (!hasError) {
      bool connected = tryWifi(username.c_str(), password.c_str());
      creds.ok = connected;
      creds.ssid = username;
      creds.pass = password;
      if (!connected) {
        logMessage("Failed to connect to wifi with provided credentials.");
        sendCommand("errorTxt.txt=\""+login_errors_const.bad_login + "\"");
      } else {
        logMessage("Connected to wifi successfully!");
        saveSetting(CONST_KEYS.ssid.c_str(), username.c_str());
        saveSetting(CONST_KEYS.pass.c_str(), password.c_str());
      }

      
      
      
      String msg;
      msg = "Found wifi and logged in, did it work? creds.ok: ";
      msg += creds.ok ? "true" : "false";
      logMessage(msg);
    }
    

   

    

    
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
  
  // if (!FAKE_NO_WIFI) {
  //   WiFi.disconnect(); 
  //   connected = tryWifi(ssidTest, passwordTest); // TEMP for testing so we can have our server
  // }
  
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

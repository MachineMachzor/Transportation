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
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

Preferences prefs;
const bool TESTING_NEXTION = false; //If false, should be production nextion
const bool FAKE_NO_WIFI = true; //If true, always go to no wifi page for testing

Stream *dbgSerial = nullptr;     // for debug output to PC
Stream *nextionSerial = nullptr; // for Nextion commands


const uint32_t USB_BAUD = 115200; //115200;   // PC debug
const uint32_t NEXTION_BAUD = 9600; // Nextion


const int MAX_RESULTS = 10; // top N suggestions to keep


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
  String ssid = "";
  String pass = "";
  bool connected = false;
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


struct pco {
  String red="53248";
  String green="608";
};

pco PCO_COLORS;

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

String httpGetStream(const String &url) {
  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure();
  HTTPClient https;
  https.begin(*client, url);
  https.addHeader("User-Agent", "ESP32/1.0");
  int httpCode = https.GET();
  String result = "";
  if (httpCode > 0 && httpCode == HTTP_CODE_OK) {
    WiFiClient *stream = https.getStreamPtr();
    const size_t bufSize = 512;
    uint8_t buf[bufSize];
    while (https.connected() && stream->available()) {
      size_t len = stream->readBytes(buf, bufSize);
      result += String((char*)buf, len); // careful: still grows memory
      // Better: parse `buf` chunk-by-chunk instead of appending to `result`
    }
  }
  https.end();
  delete client;
  return result;
}

struct Place {
  String name;
  double lat;
  double lon;
};

// --- Heuristic helpers to detect coordinates and names ---
bool isValidLatLon(double a, double b) {
  return (a >= -90.0 && a <= 90.0 && b >= -180.0 && b <= 180.0);
}


bool looksLikeName(const String &s) {
  if (s.length() < 3) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    if (isAlpha(s.charAt(i))) return true;
  }
  return false;
}

// --- Recursive extraction: find first name and first lat/lon pair in a JsonVariant ---
void extractNameAndCoords(JsonVariant v, String &outName, double &outLat, double &outLon, bool &gotName, bool &gotCoords) {
  if (gotName && gotCoords) return;

  if (v.is<const char*>()) {
    if (!gotName) {
      String s = String((const char*)v);
      if (looksLikeName(s)) {
        outName = s;
        gotName = true;
        if (gotName && gotCoords) return;
      }
    }
    return;
  }

  if (v.is<JsonArray>()) {
    JsonArray arr = v.as<JsonArray>();
    // First, scan for consecutive numeric pairs inside this array
    for (size_t i = 0; i + 1 < arr.size(); ++i) {
      if (!gotCoords && arr[i].is<double>() && arr[i+1].is<double>()) {
        double a = arr[i].as<double>();
        double b = arr[i+1].as<double>();
        if (isValidLatLon(a, b)) {
          outLat = a;
          outLon = b;
          gotCoords = true;
          if (gotName && gotCoords) return;
        }
      }
    }
    // Recurse into children
    for (JsonVariant item : arr) {
      extractNameAndCoords(item, outName, outLat, outLon, gotName, gotCoords);
      if (gotName && gotCoords) return;
    }
    return;
  }

  if (v.is<JsonObject>()) {
    for (JsonPair kv : v.as<JsonObject>()) {
      extractNameAndCoords(kv.value(), outName, outLat, outLon, gotName, gotCoords);
      if (gotName && gotCoords) return;
    }
    return;
  }
}



// --- Heuristic to find the suggestions array in the parsed JSON ---
JsonVariant findSuggestionsRoot(JsonVariant root) {
  // If root is an array, check if it looks like a suggestions list:
  if (root.is<JsonArray>()) {
    JsonArray arr = root.as<JsonArray>();
    int candidateCount = 0;
    for (JsonVariant child : arr) {
      if (child.is<JsonArray>()) {
        // if child contains at least one string, count it as a suggestion-like entry
        bool hasString = false;
        for (JsonVariant sub : child.as<JsonArray>()) {
          if (sub.is<const char*>()) { hasString = true; break; }
        }
        if (hasString) candidateCount++;
      }
    }
    // heuristics: if many children look like suggestion entries, return this array
    if (candidateCount >= 2) return root;
    // otherwise recurse into children
    for (JsonVariant child : arr) {
      JsonVariant found = findSuggestionsRoot(child);
      if (!found.isNull()) return found;
    }
  } else if (root.is<JsonObject>()) {
    for (JsonPair kv : root.as<JsonObject>()) {
      JsonVariant found = findSuggestionsRoot(kv.value());
      if (!found.isNull()) return found;
    }
  }
  return JsonVariant(); // null
}


// Call this before deserializeJson
String extractJsonArray(const String &raw) {
  // find first '[' or '{'
  int start = raw.indexOf('[');
  int startObj = raw.indexOf('{');
  if (startObj >= 0 && (startObj < start || start == -1)) start = startObj;
  if (start == -1) return String(); // no JSON start found

  // find last matching ']' or '}' by searching from the end
  int end = raw.lastIndexOf(']');
  int endObj = raw.lastIndexOf('}');
  if (endObj > end) end = endObj;
  if (end == -1 || end < start) return String(); // no JSON end found

  // return substring inclusive of start..end
  return raw.substring(start, end + 1);
}

// --- Main parser: given body string, fill places[] and return count ---
int parsePlacesFromBody(const String &body, Place places[], int maxPlaces) {
  String jsonPart = extractJsonArray(body);
  if (jsonPart.length() == 0) {
    dbgSerial->println("No JSON found in body");
    return 0;
  }

  // tune capacity to expected JSON size; increase if deserializeJson returns NoMemory
  const size_t capacity = 30000;
  JsonDocument doc;

  DeserializationError err = deserializeJson(doc, jsonPart);
  if (err) {
    dbgSerial->print("JSON parse failed: ");
    dbgSerial->println(err.c_str());
    // Optional: print a short snippet to debug
    dbgSerial->print("JSON snippet: ");
    dbgSerial->println(jsonPart);//jsonPart.substring(0, min(200, int(jsonPart.length()))));
    return 0;
  }


  JsonVariant root = doc.as<JsonVariant>();
  JsonVariant suggestions = findSuggestionsRoot(root);
  if (suggestions.isNull()) {
    // fallback: maybe root itself is the suggestions array
    if (root.is<JsonArray>()) suggestions = root;
    else {
      dbgSerial->println("Could not find suggestions array heuristically.");
      return 0;
    }
  }

  int found = 0;
  for (JsonVariant entry : suggestions.as<JsonArray>()) {
    if (found >= maxPlaces) break;
    String name = "";
    double lat = 0.0, lon = 0.0;
    bool gotName = false, gotCoords = false;

    extractNameAndCoords(entry, name, lat, lon, gotName, gotCoords);

    if (gotName && gotCoords) {
      // store result
      places[found].name = name;
      places[found].lat = lat;
      places[found].lon = lon;
      found++;
    }
  }
  return found;
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


bool tryWifi(const char* ssid, const char* pass, unsigned long timeout_ms = 20000) {
  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    if (WiFi.status() == WL_CONNECTED) {
      return true;
    }
    // do small delay to yield CPU; keep it short so loop is responsive
    delay(200);
  }
  return false; // timed out
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

// WifiLoginPage
// sendCommand("page WifiInput");
buttonText WifiLogin() {
  int compId = -1;
  String username;
  String password;
  WifiCredentials creds;
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
    sendCommand("errorTxt.pco="+PCO_COLORS.green); //Make it green before connecting
    sendCommand("errorTxt.txt=\"Connecting...\"");
    bool connected = tryWifi(username.c_str(), password.c_str());
    sendCommand("errorTxt.pco="+PCO_COLORS.red);
    sendCommand("errorTxt.txt=\"\""); //Empty it out
    creds.ok = connected;
    creds.ssid = username;
    creds.pass = password;
    if (!creds.ok) {
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
  buttonText bt;
  bt.compId = compId;
  bt.text = username;
  bt.ssid = username;
  bt.pass = password;
  bt.connected = creds.ok;
  return bt;
}




void setup() {
  // put your setup code here, to run once:
  Serial.begin(USB_BAUD);
  // Serial.println("hello");

  WiFi.mode(WIFI_STA); //Client/station mode.
  // WiFi.begin(ssid, password);



  String startAddr = "START";
  String endAddr = "Pittsburgh, PA 15222";


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
  }
  



  // Serial.println(creds);
  
  // WiFi.begin();
  // WiFi.reconnect(); //Try to store credentials?
  
  
  // Send command to Nextion to show wifi networks

  // connected = false;
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
    buttonText wl;

    while (!creds.ok) {
      wl = WifiLogin();
      creds.ok = wl.connected;
    }

    username = wl.ssid;
    password = wl.pass;
    creds.ok = wl.connected;

    logMessage("Connected to Wi-Fi");
    // sendCommand("page HomePage");  // go to main page
    // sendCommand("page HomePage");  
    // sendCommand("page HomePage");  
    // sendCommand("page HomePage");  
    sendCommand("page HomePage"); 
    
  }
  
  // Realistically we'll want to wait for a command to select to wifi then try again
  // connected = WiFi.status() == WL_CONNECTED;
  // Would have to do further checks here than this, as they may have prior stored addresses
  if (creds.ok) {
    
    logMessage("Skipped wifi selection");
    // sendCommand("page 1");
    // debugHex("page HomePage");
    sendCommand("page HomePage"); 

    dbgSerial->println("\nWiFi connected");

    String url = "https://www.google.com/s?tbm=map&gs_ri=maps&suggest=p&authuser=0&hl=en&gl=us&psi=Avghab7tBdbV5NoP9PqxgQ0.1763833866758.1&q=Tw&ech=7&pb=!2i2!4m12!1m3!1d14611.795576010498!2d-79.93046255!3d40.44832804999999!2m3!1f0!2f0!3f0!3m2!1i1298!2i924!4f13.1!7i20!10b1!12m25!1m5!18b1!30b1!31m1!1b1!34e1!2m4!5m1!6e2!20e3!39b1!10b1!12b1!13b1!16b1!17m1!3e1!20m3!5e2!6b1!14b1!46m1!1b0!96b1!99b1!19m4!2m3!1i360!2i120!4i8!20m57!2m2!1i203!2i100!3m2!2i4!5b1!6m6!1m2!1i86!2i86!1m2!1i408!2i240!7m33!1m3!1e1!2b0!3e3!1m3!1e2!2b1!3e2!1m3!1e2!2b0!3e3!1m3!1e8!2b0!3e3!1m3!1e10!2b0!3e3!1m3!1e10!2b1!3e2!1m3!1e10!2b0!3e4!1m3!1e9!2b1!3e2!2b1!9b0!15m8!1m7!1m2!1m1!1e2!2m2!1i195!2i195!3i20!22m3!1sAvghab7tBdbV5NoP9PqxgQ0!7e81!17sAvghab7tBdbV5NoP9PqxgQ0%3A83!23m2!4b1!10b1!24m109!1m30!13m9!2b1!3b1!4b1!6i1!8b1!9b1!14b1!20b1!25b1!18m19!3b1!4b1!5b1!6b1!9b1!13b1!14b1!17b1!20b1!21b1!22b1!27m1!1b0!28b0!32b1!33m1!1b1!34b1!36e2!10m1!8e3!11m1!3e1!14m1!3b0!17b1!20m2!1e3!1e6!24b1!25b1!26b1!27b1!29b1!30m1!2b1!36b1!37b1!39m3!2m2!2i1!3i1!43b1!52b1!54m1!1b1!55b1!56m1!1b1!61m2!1m1!1e1!65m5!3m4!1m3!1m2!1i224!2i298!72m22!1m8!2b1!5b1!7b1!12m4!1b1!2b1!4m1!1e1!4b1!8m10!1m6!4m1!1e1!4m1!1e3!4m1!1e4!3sother_user_google_review_posts__and__hotel_and_vr_partner_review_posts!6m1!1e1!9b1!89b1!98m3!1b1!2b1!3b1!103b1!113b1!114m3!1b1!2m1!1b1!117b1!122m1!1b1!126b1!127b1!26m4!2m3!1i80!2i92!4i8!34m19!2b1!3b1!4b1!6b1!8m6!1b1!3b1!4b1!5b1!6b1!7b1!9b1!12b1!14b1!20b1!23b1!25b1!26b1!31b1!37m1!1e81!47m0!49m10!3b1!6m2!1b1!2b1!7m2!1e3!2b1!8b1!9b1!10e2!61b1!67m5!7b1!10b1!14b1!15m1!1b0!69i759"; // example (fragile)
    String body = httpGetStream(url);
    logMessage("Response length: " + String(body.length()));
    // dbgSerial->println(body); // or parse it
    
    Place places[MAX_RESULTS];
    int count = parsePlacesFromBody(body, places, MAX_RESULTS);

    dbgSerial->printf("Found %d places:\n", count);
    for (int i = 0; i < count; ++i) {
      dbgSerial->printf("%d) %s -> %f, %f\n", i+1, places[i].name.c_str(), places[i].lat, places[i].lon);
    }
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


/*
So, using main.cpp with an ESP32cam, lets say I have two variables:
String startAddr = "START"; String endAddr = "Pittsburgh, PA 15222";
- Given a current string of someone trying to search for an address, I want it to autofill the top 5 relevant search addresses below, like in google maps
- If startAddr is START, it should be replaced with the current location of the device. If this is not possible with an ESP32cam alone, it's fine to skip this
- Like in google maps, it should generate routes (car, transit, walking, cyclist) that is parsable, like for transit, there's X amount of first buses to take etc.
Is this all possible?


*/


// Run script in monitor
// cd C:\Users\ringk\OneDrive\Documents\PlatformIO\Projects\Transportation_IO
// pio device monitor -p COM4 -b 115200 --filter esp32_exception_decoder

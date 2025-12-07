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
#include <regex>
#include <time.h>
#include <ctype.h>



Preferences prefs;
const bool TESTING_NEXTION = true;//If false, should be production nextion
const bool FAKE_WIFI = true; //If true, just load in our test wifi credentials instead of selecting one

// This is for the use case of if wifi just does not work randomly, but still want to develop code
const bool OVERRIDE_WIFI = false; //Last resort, usually wifi should be connecting
#define DEBUG_MINUTES false



// Set all of these to false in production to not skip the code
const bool SKIP_WIFI_SELECTION = true;
const bool SKIP_WIFI_LOGIN = true;
const bool SKIP_ADDR_CHOOSE = true;
const bool SKIP_FIRST_BUS_SELECT = true;
const bool SKIP_WALKTIME_SET = true;
const bool SKIP_INFO_PAGE = false;

// Set all of these to false in production to not reset saved settings
const bool RESET_SAVED_ADDRS = true;
const bool RESET_SAVED_WALKTIME = true;




/*
// FULL TESTING OF NEXTION
const bool TESTING_NEXTION = true; //If false, should be production nextion
const bool FAKE_WIFI = false; //If true, always go to no wifi page for testing

*/

Stream *dbgSerial = nullptr;     // for debug output to PC
Stream *nextionSerial = nullptr; // for Nextion commands


const uint32_t USB_BAUD = 115200; //115200;   // PC debug
const uint32_t NEXTION_BAUD = 9600; // Nextion


const int MAX_RESULTS = 10; // top N suggestions to keep
const int MAX_TRANSIT_RESULTS = 2; // top N transit route suggestions to keep

String pageTracker;



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

struct BoardingInfo {
  String busLabel;
  String walkDistance;
  String walkTime;
  String stationName;
  String nextBusTime;
  // int nextBusInMinutes; //Dist from utcNow() to the nextBusTime
};


struct keys {
  String ssid = "SSID";
  String pass = "PASS";
  String startAddr = "START";
  String endAddr = "END";
  String walkTime = "WALKTIME"; //Walk time has to be 0 or greater, prohibit anything less than 0
  String milesCompute = "MILES"; //This is the miles specified for their walktime at the time, could be default or not, going to use this as relative if the first bus changes
  String bus = "BUS"; //Caches the bus. Actual string bus
  String firstBusOnly = "FIRSTBUSONLY"; //Overrides saved transit bus and just takes first one. This is a boolean not the actul string bus
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
    {6, "b4"}, //End of wifi options
    {7, "b5"}, //Unlisted
    {8, "b6"} //Refresh button
};


std::map<int, String> ADDR_PAGES = {
    {6, "b1"},
    {7, "b2"},
    {8, "b3"},
};

std::map<int, String> TRANSIT_BUSES = {
    {3, "b0"},
    {4, "b1"}
};

int ADDR_CONTINUE_ID = 3;



const String HOME_PAGE_START_TXT = "t2"; //User inputted start
const String HOME_PAGE_START_SELECTED_ID = "t5"; //Selected start location
const String  HOME_PAGE_END_TXT_ID = "t4";   //User inputted end
const String  HOME_PAGE_END_SELECTED_ID = "t6"; //Selected end location


static constexpr float KM_TO_MILES = 0.621371f;
static constexpr float FT_PER_MILE = 5280.0f;

// Extract the first numeric token (handles optional leading sign and decimal).
// Returns true and sets outVal if a number was found.
bool extractNumber(const String &s, float &outVal) {
  String num;
  bool seenDigit = false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if ((c >= '0' && c <= '9') || c == '.' || (c == '-' && num.length() == 0)) {
      num += c;
      seenDigit = true;
    } else if (seenDigit) {
      break; // stop at first non-number after we've started
    }
  }
  if (num.length() == 0) return false;
  outVal = num.toFloat();
  return true;
}

// Convert strings like "0.1 mi", "300 ft", "1 km" to miles (float).
// If unit not recognized, assumes the number is already in miles.
float distanceStringToMiles(const String &raw) {
  String s = raw;
  s.toLowerCase();
  s.trim();

  float value = 0.0f;
  if (!extractNumber(s, value)) return 0.0f;

  // detect units (allow "mi", " mi", "ft", " ft", "km", " km")
  if (s.indexOf("mi") >= 0 && s.indexOf("min") < 0) { // avoid matching "min"
    return value;
  }
  if (s.indexOf("ft") >= 0) {
    return value / FT_PER_MILE;
  }
  if (s.indexOf("km") >= 0) {
    return value * KM_TO_MILES;
  }

  // fallback: assume miles
  return value;
}

// Parse minutes from strings like "6 min" or "12.5min" and return float minutes.
// Returns 0.0f if no number or no "min" token present.
float minutesStringToFloat(const String &raw) {
  String s = raw;
  s.toLowerCase();
  s.trim();
  if (s.indexOf("min") < 0) return 0.0f;

  float value = 0.0f;
  if (!extractNumber(s, value)) return 0.0f;
  return value;
}





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

const float WALKTIME_NONE = -999;
const float WALKTIME_DEFAULT = -333; // signal to use default walk time from Maps

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
  WiFiClientSecure *client = new WiFiClientSecure; //Allocates TLS (transport layer security, wifi based communication between server and web ) on the heap so we can delete later
  client->setInsecure();// disable certificate verification (SSL). For production, use root CA or fingerprint
  HTTPClient https; //For performing the request
  https.begin(*client, url); // initialize HTTPS connection with TLS and url
  https.addHeader("User-Agent", "ESP32/1.0"); 
  int httpCode = https.GET(); //blocking call (waits until we finish) for GET request. Already supplied enough info prior to make this call
  String result = ""; //Prepares response
  if (httpCode > 0 && httpCode == HTTP_CODE_OK) { //Good response
    WiFiClient *stream = https.getStreamPtr(); // get stream pointer to read response body (TCP, transmission control protocol)
    const size_t bufSize = 512; //Chunked reads
    uint8_t buf[bufSize];
    while (https.connected() && stream->available()) { //connection is open and data is available
      size_t len = stream->readBytes(buf, bufSize); //Reads bufSize bytes into buf, returns number of bytes read
      result += String((char*)buf, len); // careful: still grows memory --> Append to result
      // Better: parse `buf` chunk-by-chunk instead of appending to `result`
    }
  } else {
    dbgSerial->printf("HTTP GET failed, error: %s\n", https.errorToString(httpCode).c_str());
    result = "Failed";
  }
  https.end(); //End session and free resources
  delete client;
  return result;
}

struct currentLocation {
  double lat;
  double lon;
};



String logBuffer = "";
static std::vector<String> logLines;  
static int holdMessageCount = 100; //8;  // holds up to N messages

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


StaticJsonDocument<512> returnUserData() {
  currentLocation c;
  String url = "https://ipinfo.io/json";
  String response = httpGetStream(url);
  // logMessage(response);
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    logMessage("Failed to parse location JSON");
    // return c;
  }
  return doc;
}

currentLocation getCurrentLocation() {
  currentLocation c;
  StaticJsonDocument<512> doc = returnUserData();
  String loc_str = doc["loc"].as<String>();
  std::string loc = loc_str.c_str();
  std::regex locRegex(R"(([-+]?[0-9]*\.?[0-9]+),([-+]?[0-9]*\.?[0-9]+))");
  std::smatch matches;
  if (std::regex_search(loc, matches, locRegex) && matches.size() == 3) {
    c.lat = static_cast<float>(std::stof(matches[1].str())); //double
    c.lon = static_cast<float>(std::stof(matches[2].str()));
  } else {
    logMessage("Failed to extract lat/lon from location string");
  }
  return c;
  // c.lat = String(doc["lat"].as<double>(), 6);
}

String getUserTimezone() {
  StaticJsonDocument<512> doc = returnUserData();
  String tzArduino = doc["timezone"].as<String>();
  std::string tz = std::string(tzArduino.c_str());

  // Trim
  auto l = tz.find_first_not_of(" \t\r\n");
  if (l == std::string::npos) return String("");
  auto r = tz.find_last_not_of(" \t\r\n");
  tz = tz.substr(l, r - l + 1);

  // IANA regex: Region/City or multi-part (e.g., America/Indiana/Knox)
  // Allows letters, digits, underscore, plus, minus; requires at least one '/'
  static const std::regex ianaRegex(R"(^[A-Za-z_+\-]+(?:\/[A-Za-z0-9_+\-]+)+$)");
  if (std::regex_match(tz, ianaRegex)) {
    return String(tz.c_str());
  }

  // Not a valid IANA name
  return String("");
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


// Trim whitespace
static String trimStr(const String &s) {
  int a = 0;
  while (a < s.length() && isspace((unsigned char)s[a])) ++a;
  int b = s.length() - 1;
  while (b >= a && isspace((unsigned char)s[b])) --b;
  if (b < a) return String("");
  return s.substring(a, b + 1);
}

// Extract first contiguous digit sequence (useful if CR/LF or hidden chars present)
bool extractFirstDigits(const String &s, String &outDigits) {
  outDigits = "";
  bool seen = false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c >= '0' && c <= '9') {
      outDigits += c;
      seen = true;
    } else if (seen) {
      break; // stop after first run of digits
    }
  }
  return seen;
}

// Parse "6:45 PM", "06:45pm", "18:45", "6:45"
bool parseTimeString(const String &in, int &outHour, int &outMin) {
  String s = trimStr(in);
  if (s.length() == 0) return false;
  String low = s;
  low.toLowerCase();
  bool hasAM = (low.indexOf("am") >= 0);
  bool hasPM = (low.indexOf("pm") >= 0);
  if (hasAM) low.replace("am", "");
  if (hasPM) low.replace("pm", "");
  low = trimStr(low);

  int colon = low.indexOf(':');
  if (colon < 0) return false;
  String hstr = trimStr(low.substring(0, colon));
  String mstr = trimStr(low.substring(colon + 1));
  if (hstr.length() == 0 || mstr.length() == 0) return false;

  int h = hstr.toInt();
  int m = mstr.toInt();
  if (m < 0 || m > 59) return false;

  if (hasAM || hasPM) {
    if (h < 1 || h > 12) return false;
    if (hasAM) {
      if (h == 12) h = 0;
    } else { // PM
      if (h != 12) h += 12;
    }
  } else {
    if (h < 0 || h > 23) return false;
  }

  outHour = h;
  outMin = m;
  return true;
}


// store this across calls (initialize to -1)
static long prev_epoch_min = -1;

// Call this whenever you want to check. Returns true exactly once when minute changes.
bool minute_changed_now(void) {
  time_t now = time(NULL);
  if (now == (time_t)-1) return false; // clock error

  long epoch_min = (long)(now / 60);   // integer minute since epoch
  if (epoch_min != prev_epoch_min) {
    prev_epoch_min = epoch_min;
    return true; // minute boundary crossed since last check (or first call)
  }
  return false;
}

// int seconds_until_next_minute(void) {
//     time_t now = time(NULL);
//     if (now == (time_t)-1) return -1; /* errno set by time() */
//     int sec = (int)(now % 60);
//     int wait = 60 - sec;
//     if (wait == 0) wait = 60;
//     return wait;
// }




// Main: epochMsStr may contain hidden chars; nextBusTimeStr examples: "6:45 PM", "18:45"
int minutesDifferenceFromEpochMs(const String &epochMsStr, const String &nextBusTimeStr) {
  // 1) extract digits from epoch string
  String digits;
  if (!extractFirstDigits(epochMsStr, digits)) {
    if (DEBUG_MINUTES) Serial.println("ERR: no digits in epoch string");
    return INT_MIN;
  }

  // optional: ensure we have at least 10 digits (seconds) or 13 digits (ms)
  if (digits.length() < 10) {
    if (DEBUG_MINUTES) {
      Serial.print("ERR: epoch digits too short: "); Serial.println(digits);
    }
    return INT_MIN;
  }

  // parse to unsigned long long (ms)
  unsigned long long ms = 0ULL;
  for (size_t i = 0; i < digits.length(); ++i) {
    ms = ms * 10ULL + (unsigned long long)(digits[i] - '0');
  }

  // if digits length looks like seconds (10 digits), convert to ms
  if (digits.length() == 10) ms *= 1000ULL;

  time_t now_t = (time_t)(ms / 1000ULL);

  // 2) parse next bus time
  int targetHour = 0, targetMin = 0;
  if (!parseTimeString(nextBusTimeStr, targetHour, targetMin)) {
    if (DEBUG_MINUTES) {
      Serial.print("ERR: cannot parse nextBusTimeStr: '"); Serial.print(nextBusTimeStr); Serial.println("'");
    }
    return INT_MIN;
  }

  // 3) build tm for now (localtime if TZ set)
  struct tm now_tm;
#if defined(ARDUINO_ARCH_ESP32) || defined(__unix__) || defined(__APPLE__)
  localtime_r(&now_t, &now_tm);
  

#else
  struct tm *tmp = localtime(&now_t);
  if (!tmp) return INT_MIN;
  now_tm = *tmp;
#endif

  // 4) build target tm for same date as now
  struct tm target_tm = now_tm;
  target_tm.tm_hour = targetHour;
  target_tm.tm_min  = targetMin;
  target_tm.tm_sec  = 0;

  time_t target_t = mktime(&target_tm);
  if (target_t == (time_t)-1) {
    if (DEBUG_MINUTES) Serial.println("ERR: mktime failed");
    return INT_MIN;
  }

  long diffSeconds = static_cast<long>(difftime(target_t, now_t));
  int diffMinutes; //= static_cast<int>(diffSeconds / 60); // trunc toward zero
  if (diffSeconds >= 0) {
      diffMinutes = static_cast<int>((diffSeconds + 30) / 60); // round half up
  } else {
    diffMinutes = static_cast<int>((diffSeconds - 30) / 60); // round half down for negatives
  }


  if (DEBUG_MINUTES) {
    Serial.print("DEBUG epoch digits: "); Serial.println(digits);
    Serial.print("DEBUG ms: "); Serial.println((unsigned long long)ms);
    Serial.print("DEBUG now_t (s): "); Serial.println((long)now_t);
    Serial.print("DEBUG now date: ");
    Serial.print(now_tm.tm_year + 1900); Serial.print('-');
    Serial.print(now_tm.tm_mon + 1); Serial.print('-');
    Serial.print(now_tm.tm_mday); Serial.print(' ');
    Serial.print(now_tm.tm_hour); Serial.print(':'); Serial.print(now_tm.tm_min); Serial.print(':'); Serial.println(now_tm.tm_sec);
    Serial.print("DEBUG target h:m: "); Serial.print(targetHour); Serial.print(':'); Serial.println(targetMin);
    Serial.print("DEBUG diffMinutes: "); Serial.println(diffMinutes);
  }

  return diffMinutes;
}


std::vector<String> connectionSequence(bool verbose=false) {
  int n = WiFi.scanNetworks();
  WifiCredentials result;
  // String wifiList = "";
  std::vector<String> wifiList;
  if (verbose) dbgSerial->println("Wifi List\n-----------");
  for (int i = 0; i < min(n,5); ++i) {               // send first 5 rows min(n,5)
    String s = WiFi.SSID(i);
    // sendCommand("t" + String(i) + ".txt=\"" + s + "\""); // assumes t0..t4 text fields on Nextion, this is why we may want to limit it to 5 only
    // dbgSerial->println(s);
    wifiList.push_back(s);
    if (verbose) {
      dbgSerial->println(s);
    }
    // wifiList += s;
  }
  // Realistically sleep wait until Nextion calls back with a submission attempt for this, and also send creds to Nextion
  // WiFi.begin(ssidTest, passTest);
  
  return wifiList;
}

void sendComponentTxt(int btnCount, int txtTruncateLength, const std::vector<String>& txtList,
                      String componentType = "b", bool loading = false, int startOffset = 0) {
  // i is the component index on the screen; j is the index into txtList
  for (int i = startOffset; i < min(btnCount + startOffset, int(startOffset + btnCount)); ++i) {
    int j = i - startOffset;               // index into txtList
    String txt = "";

    if (loading) {
      txt = "Loading...";
    } else {
      if (j >= 0 && j < int(txtList.size())) {
        txt = txtList[j];
      } else {
        // No text available for this slot: skip updating it to avoid clearing previous text
        continue;
      }
    }

    if (txt.length() > txtTruncateLength) {
      txt = txt.substring(0, txtTruncateLength);
    }
    // if (txt.length() == 0) {
    //   // txt = " "; // avoid empty text which Nextion may mishandle
    //   continue;
    // }
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


void safeSetPage(String page) {
  if (pageTracker != page) {
    sendCommand("");
    String cmd = "page " + page;
    sendCommand(cmd);
    pageTracker = page;
    delay(500); // let Nextion switch pages
  }
}



// ROUTE: DecisionPage --> NoWifiPage --> WifiInput --> StartAddr/End
buttonText SelectWifi() {
  buttonText bt;
  // sendCommand("page NoWifiPage");
  // pageTracker = "NoWifiPage";
  safeSetPage("NoWifiPage");
  logMessage("Finding WIFI as new");
  // delay(1000); // let Nextion switch pages
  std::vector<String> wifiList = connectionSequence();
  sendComponentTxt(5, 20, wifiList, "b"); // send first 5 networks to buttons, truncate to 20 chars
  logMessage("Scanned wifi networks:\n" + joinWithNewline(wifiList));
  int compId = -1;
  String text = "";
  while (compId == -1 || compId == 8 || text.length() == 0 || text == "Loading...") {
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
    if (compId == 8) {
      logMessage("Refresh button pressed, rescanning wifi networks...");
      sendComponentTxt(5, 20, wifiList, "b", true);
      wifiList = connectionSequence();
      sendComponentTxt(5, 20, wifiList, "b"); // send first 5 networks to buttons, truncate to 20 chars
      logMessage("Rescanned wifi networks:\n" + joinWithNewline(wifiList));
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


struct Place {
  String name;
  double lat;
  double lon;
};
Place placesStart[MAX_RESULTS];
Place placesEnd[MAX_RESULTS];

int placesCount; 
String searchQuery;


String directionsSearch;



// Simple match record
struct Match {
  unsigned long pos; // position in body
  String val;        // captured human string
};

struct Station {
  unsigned long pos = 0;
  String name;
  String humanTime;        // chosen next time (human readable)
  unsigned long epoch = 0; // chosen next epoch
  // new fields:
  String scheduledHuman;        // earliest scheduled human time (if present)
  unsigned long scheduledEpoch = 0;
  bool delayed = false;        // true if chosen epoch > scheduledEpoch
};

// --- Helpers (unchanged semantics, small additions) ---

static int skipSpaces(const String &s, int i) {
  int n = s.length();
  while (i < n) {
    char c = s.charAt(i);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i;
    else break;
  }
  return i;
}

// Parse an unsigned integer starting at i; advances i; returns -1 if none
static long parseNumber(const String &s, int &i) {
  i = skipSpaces(s, i);
  int n = s.length();
  long val = 0;
  bool found = false;
  while (i < n) {
    char c = s.charAt(i);
    if (c >= '0' && c <= '9') {
      found = true;
      val = val * 10 + (c - '0');
      ++i;
    } else break;
  }
  return found ? val : -1;
}

// Return true if the label looks like a transit route (contains at least one digit).
static bool isTransitRouteLabel(const String &label) {
  for (int k = 0; k < label.length(); ++k) {
    char c = label.charAt(k);
    if (c >= '0' && c <= '9') return true;
  }
  return false;
}

// Normalize a label: trim, collapse whitespace, filter obvious garbage (svg urls, //maps, etc.)
static String normalizeLabel(const String &label) {
  String out = label;
  out.trim();
  out.replace("\u202F", " ");
  out.replace("\u00A0", " ");
  // collapse multiple spaces
  String tmp;
  bool lastSpace = false;
  for (int i = 0; i < out.length(); ++i) {
    char c = out.charAt(i);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!lastSpace) { tmp += ' '; lastSpace = true; }
    } else { tmp += c; lastSpace = false; }
  }
  tmp.trim();
  String low = tmp;
  low.toLowerCase();
  if (low.startsWith("http://") || low.startsWith("https://") || low.startsWith("//") ||
      low.indexOf("maps.gstatic.com") >= 0 || low.indexOf(".svg") >= 0) return String();
  // require at least one alphanumeric character
  bool hasAlnum = false;
  for (int i = 0; i < tmp.length(); ++i) {
    char c = tmp.charAt(i);
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { hasAlnum = true; break; }
  }
  if (!hasAlnum) return String();
  return tmp;
}

// Extract a quoted string starting at index i (i points at opening quote).
// Advances i to the character after the closing quote. Returns empty String if no closing quote.
static String extractQuoted(const String &s, int &i) {
  int n = s.length();
  i = skipSpaces(s, i);
  if (i >= n || s.charAt(i) != '"') return String();
  ++i; // skip opening quote
  String out;
  bool escape = false;
  while (i < n) {
    char c = s.charAt(i++);
    if (escape) {
      if (c == 'n') out += '\n';
      else if (c == 'r') out += '\r';
      else if (c == 't') out += '\t';
      else out += c;
      escape = false;
    } else {
      if (c == '\\') escape = true;
      else if (c == '"') return out;
      else out += c;
    }
  }
  // no closing quote found
  return String();
}

// --- tryStationBlock (robust, consumes entire station block and collects time arrays) ---
// Station pattern expected: ["Stop Name", "12345", [epoch, "TZ", "human time", ...], ...]
static int tryStationBlock(const String &body, int i, std::vector<Station> &stations) {
  int n = body.length();
  int orig = i;
  i = skipSpaces(body, i);
  if (i >= n || body.charAt(i) != '[') return orig;

  int p = i + 1;
  p = skipSpaces(body, p);
  if (p >= n || body.charAt(p) != '"') return orig;

  // extract stop name
  int q = p;
  String stopName = extractQuoted(body, q);
  if (stopName.length() == 0) return orig;
  p = skipSpaces(body, q);
  if (p >= n) return orig;

  // advance past stop id if present (we don't require it)
  if (body.charAt(p) == ',') {
    p = skipSpaces(body, p + 1);
    if (p < n && body.charAt(p) == '"') {
      int q2 = p;
      (void)extractQuoted(body, q2); // ignore id content, just advance
      p = skipSpaces(body, q2);
    }
  }

  // scan the station block and collect all nested time arrays [ epoch, "TZ", "human time", ... ]
  unsigned long minEpoch = 0; // earliest (scheduled)
  unsigned long maxEpoch = 0; // latest (real-time / chosen)
  String minHuman;
  String maxHuman;

  int idx = p;
  while (idx < n) {
    idx = skipSpaces(body, idx);
    if (idx >= n) break;
    char c = body.charAt(idx);
    if (c == '[') {
      // attempt to parse nested time array starting here
      int t = idx + 1;
      t = skipSpaces(body, t);
      long maybeEpoch = parseNumber(body, t);
      if (maybeEpoch >= 0) {
        // look for comma then quoted tz then comma then quoted human time
        int t1 = skipSpaces(body, t);
        if (t1 < n && body.charAt(t1) == ',') {
          int t2 = skipSpaces(body, t1 + 1);
          if (t2 < n && body.charAt(t2) == '"') {
            int qtz = t2;
            String tz = extractQuoted(body, qtz);
            if (tz.length() > 0) {
              int t3 = skipSpaces(body, qtz);
              if (t3 < n && body.charAt(t3) == ',') {
                int t4 = skipSpaces(body, t3 + 1);
                if (t4 < n && body.charAt(t4) == '"') {
                  int qhuman = t4;
                  String human = extractQuoted(body, qhuman);
                  if (human.length() > 0) {
                    unsigned long e = (unsigned long)maybeEpoch;
                    // update min/max
                    if (minEpoch == 0 || e < minEpoch) {
                      minEpoch = e;
                      minHuman = human;
                    }
                    if (e > maxEpoch) {
                      maxEpoch = e;
                      maxHuman = human;
                    }
                    // advance idx to after this nested array's closing bracket
                    idx = qhuman;
                    while (idx < n && body.charAt(idx) != ']') ++idx;
                    if (idx < n && body.charAt(idx) == ']') ++idx;
                    continue;
                  }
                }
              }
            }
          }
        }
      }

      // not a recognized time array: skip nested array (balance brackets)
      int depthNested = 1;
      int j = idx + 1;
      while (j < n && depthNested > 0) {
        if (body.charAt(j) == '[') ++depthNested;
        else if (body.charAt(j) == ']') --depthNested;
        ++j;
      }
      idx = j;
      continue;
    } else if (c == ']') {
      // end of station block
      ++idx;
      Station s;
      s.pos = (unsigned long)i;
      s.name = stopName;
      // choose values: prefer maxEpoch as the "next" time, but keep scheduled (minEpoch) if present
      if (maxEpoch > 0) {
        s.epoch = maxEpoch;
        s.humanTime = maxHuman;
      }
      if (minEpoch > 0) {
        s.scheduledEpoch = minEpoch;
        s.scheduledHuman = minHuman;
      }
      if (s.epoch > 0 && s.scheduledEpoch > 0 && s.epoch > s.scheduledEpoch) {
        s.delayed = true;
      } else {
        s.delayed = false;
      }
      stations.push_back(s);
      return idx;
    } else {
      ++idx;
    }
  }

  // fallback if no closing bracket found
  Station s;
  s.pos = (unsigned long)i;
  s.name = stopName;
  if (maxEpoch > 0) { s.epoch = maxEpoch; s.humanTime = maxHuman; }
  if (minEpoch > 0) { s.scheduledEpoch = minEpoch; s.scheduledHuman = minHuman; }
  if (s.epoch > 0 && s.scheduledEpoch > 0 && s.epoch > s.scheduledEpoch) s.delayed = true;
  stations.push_back(s);
  return n;
}

// --- scanBodyManual (uses tryStationBlock and filters route labels) ---
static void scanBodyManual(const String &body,
                           std::vector<Match> &routes,
                           std::vector<Match> &distances,
                           std::vector<Match> &walkTimes,
                           std::vector<Station> &stations,
                           std::vector<Match> &departures,
                           Stream *dbgSerial) {
  int n = body.length();
  int i = 0;
  while (i < n) {
    // attempt station block first (starts with '[' then '"')
    if (body.charAt(i) == '[') {
      int newPos = tryStationBlock(body, i, stations);
      if (newPos != i) { i = newPos; continue; }
    }

    // look for numeric-coded arrays: [ <code> , ...
    if (body.charAt(i) == '[') {
      int p = i;
      int j = i + 1;
      j = skipSpaces(body, j);
      long code = parseNumber(body, j);
      if (code >= 0) {
        int afterNum = skipSpaces(body, j);
        if (afterNum < n && body.charAt(afterNum) == ',') {
          int afterComma = skipSpaces(body, afterNum + 1);
          // nested array with quoted label: [ code , [ "Label" ...
          if (afterComma < n && body.charAt(afterComma) == '[') {
            int inner = afterComma + 1;
            inner = skipSpaces(body, inner);
            if (inner < n && body.charAt(inner) == '"') {
              int q = inner;
              String label = extractQuoted(body, q);
              if (label.length() > 0) {
                // Only treat this quoted label as a route if it looks like a transit route (contains a digit).
                if (isTransitRouteLabel(label)) {
                  // normalize label before pushing
                  String norm = normalizeLabel(label);
                  if (norm.length() > 0) {
                    Match m; m.pos = (unsigned long)p; m.val = norm;
                    routes.push_back(m);
                  }
                } else {
                  // not a transit route label; ignore as route
                }
                i = q;
                ++i;
                continue;
              }
            }
          }
          // simple code,value pair: [ code , "value" ...
          if (afterComma < n && body.charAt(afterComma) == '"') {
            int q = afterComma;
            String value = extractQuoted(body, q);
            if (value.length() > 0) {
              String low = value;
              low.toLowerCase();
              // heuristics for distance
              if (low.endsWith("mi") || low.endsWith("ft") || low.indexOf("km") >= 0 ||
                  (low.indexOf(" m") >= 0 && low.indexOf("min") == -1)) {
                Match m; m.pos = (unsigned long)p; m.val = value; distances.push_back(m);
              } else if (low.indexOf("min") >= 0) {
                Match m; m.pos = (unsigned long)p; m.val = value; walkTimes.push_back(m);
              } else {
                // possible departure-like quoted time (e.g., "5:08 AM" or "5:08 AM")
                if (value.indexOf(':') >= 0 && (value.indexOf("AM") >= 0 || value.indexOf("PM") >= 0 ||
                                               value.indexOf("am") >= 0 || value.indexOf("pm") >= 0 ||
                                               value.indexOf("\u202FAM") >= 0 || value.indexOf("\u202FPM") >= 0)) {
                  Match m; m.pos = (unsigned long)p; m.val = value; departures.push_back(m);
                }
              }
              i = q;
              ++i;
              continue;
            }
          }
        }
      }
    }

    // detect inline "departure_time" key and try to capture human time inside its array
    if (body.startsWith("\"departure_time\"", i) || body.startsWith("departure_time", i)) {
      int k = i;
      // find next '['
      while (k < n && body.charAt(k) != '[') ++k;
      if (k < n && body.charAt(k) == '[') {
        int q = k + 1;
        long epoch = parseNumber(body, q);
        if (epoch >= 0) {
          q = skipSpaces(body, q);
          if (q < n && body.charAt(q) == ',') {
            q = skipSpaces(body, q + 1);
            if (q < n && body.charAt(q) == '"') {
              int q2 = q;
              String tz = extractQuoted(body, q2);
              q2 = skipSpaces(body, q2);
              if (q2 < n && body.charAt(q2) == ',') {
                q2 = skipSpaces(body, q2 + 1);
                if (q2 < n && body.charAt(q2) == '"') {
                  int q3 = q2;
                  String human = extractQuoted(body, q3);
                  if (human.length() > 0) {
                    Match m; m.pos = (unsigned long)i; m.val = human; departures.push_back(m);
                    i = q3;
                    ++i;
                    continue;
                  }
                }
              }
            }
          }
        }
      }
    }

    ++i;
  }
}
// --- dedupeRoutes: keep only the earliest occurrence for each route label (first step) ---
static std::vector<Match> dedupeRoutes(const std::vector<Match> &routes) {
  // Map label -> earliest pos
  std::vector<Match> out;
  out.reserve(routes.size());

  for (const auto &r : routes) {
    String norm = normalizeLabel(r.val);
    if (norm.length() == 0) continue;

    // find existing entry for this label
    bool found = false;
    for (auto &existing : out) {
      if (existing.val == norm) {
        found = true;
        // keep the earliest (smallest) pos
        if (r.pos < existing.pos) existing.pos = r.pos;
        break;
      }
    }
    if (!found) {
      Match m; m.pos = r.pos; m.val = norm;
      out.push_back(m);
    }
  }

  // Sort by position so results are in document order (earliest first)
  std::sort(out.begin(), out.end(), [](const Match &a, const Match &b) {
    return a.pos < b.pos;
  });

  return out;
}
// --- groupBoardingInfos: use deduped routes (first step per label) and skip routes with no following station ---
// --- groupBoardingInfos (updated) ---
// Uses dedupeRoutes (unchanged) and then:
// 1) picks the station with the smallest positive (s.pos - rm.pos) distance, preferring stations with humanTime
// 2) after building the list, removes entries that duplicate stationName+nextBusTime (keep first)
static std::vector<BoardingInfo> groupBoardingInfos(const std::vector<Match> &routes_in,
                                                    const std::vector<Match> &distances,
                                                    const std::vector<Match> &walkTimes,
                                                    const std::vector<Station> &stations,
                                                    const std::vector<Match> &departures,
                                                    size_t MAX_RESULTS) {
  std::vector<BoardingInfo> out;

  // normalize and dedupe routes (keep earliest occurrence per label)
  std::vector<Match> routes = dedupeRoutes(routes_in);

  out.reserve(min((size_t)routes.size(), MAX_RESULTS));

  // helper to find first departure after a given pos
  auto findDepartureAfter = [&](unsigned long pos)->String {
    for (const auto &dep : departures) {
      if (dep.pos > pos) return dep.val;
    }
    return String();
  };

  for (size_t r = 0; r < routes.size() && out.size() < MAX_RESULTS; ++r) {
    const Match &rm = routes[r];
    BoardingInfo bi;
    bi.busLabel = rm.val;

    // first distance after route
    for (const auto &d : distances) {
      if (d.pos > rm.pos) { bi.walkDistance = d.val; break; }
    }
    // first walk time after route
    for (const auto &w : walkTimes) {
      if (w.pos > rm.pos) { bi.walkTime = w.val; break; }
    }

    // --- Improved station selection: choose station with smallest positive delta (s.pos - rm.pos)
    const Station *bestStation = nullptr;
    unsigned long bestDelta = 0; // smallest positive delta
    const Station *fallbackFirstAfter = nullptr;
    unsigned long firstAfterDelta = 0;

    for (const auto &s : stations) {
      if (s.pos <= rm.pos) continue;
      unsigned long delta = s.pos - rm.pos;
      if (!fallbackFirstAfter) { fallbackFirstAfter = &s; firstAfterDelta = delta; }
      // prefer stations that have humanTime; among those pick smallest delta
      if (s.humanTime.length() > 0 || s.epoch > 0) {
        if (!bestStation || delta < bestDelta) {
          bestStation = &s;
          bestDelta = delta;
        }
      } else {
        // if no bestStation yet, consider this as candidate only if it's closer than fallback
        if (!bestStation && fallbackFirstAfter && delta < firstAfterDelta) {
          // update fallbackFirstAfter to this closer station without humanTime
          fallbackFirstAfter = &s;
          firstAfterDelta = delta;
        }
      }
    }

    if (!bestStation) bestStation = fallbackFirstAfter;

    if (bestStation) {
      bi.stationName = bestStation->name;
      if (bestStation->humanTime.length() > 0) {
        bi.nextBusTime = bestStation->humanTime;
      } else {
        // fallback: find first departure after station pos
        String dep = findDepartureAfter(bestStation->pos);
        if (dep.length() > 0) bi.nextBusTime = dep;
        else bi.nextBusTime = String();
      }
      // append delay note if present
      if (bestStation->delayed && bestStation->scheduledHuman.length() > 0) {
        bi.nextBusTime += " (delayed from ";
        bi.nextBusTime += bestStation->scheduledHuman;
        bi.nextBusTime += ")";
      }
    } else {
      // fallback: first departure after route
      for (const auto &dep : departures) {
        if (dep.pos > rm.pos) { bi.nextBusTime = dep.val; break; }
      }
    }

    out.push_back(bi);
  }

  // --- Post-filter: remove entries that duplicate stationName + nextBusTime (keep first)
  std::vector<BoardingInfo> filtered;
  filtered.reserve(out.size());
  for (const auto &b : out) {
    bool dup = false;
    for (const auto &f : filtered) {
      if (f.stationName == b.stationName && f.nextBusTime == b.nextBusTime) { dup = true; break; }
    }
    if (!dup) filtered.push_back(b);
  }

  // limit to MAX_RESULTS (in case filtering changed count)
  if (filtered.size() > MAX_RESULTS) filtered.resize(MAX_RESULTS);
  return filtered;
}


// --- Public API: extract up to MAX_RESULTS BoardingInfo entries from body and optionally print debug to dbgSerial. ---
std::vector<BoardingInfo> extractBoardingInfosManual(const String &body, Stream *dbgSerial, size_t MAX_RESULTS) {
  std::vector<Match> routes;
  std::vector<Match> distances;
  std::vector<Match> walkTimes;
  std::vector<Station> stations;
  std::vector<Match> departures;

  if (dbgSerial) {
    dbgSerial->print("Starting manual scan, body length=");
    dbgSerial->println((int)body.length());
  }

  scanBodyManual(body, routes, distances, walkTimes, stations, departures, dbgSerial);

  // if (dbgSerial) {
  //   dbgSerial->print("Found routes: "); dbgSerial->println((int)routes.size());
  //   dbgSerial->print("Found distances: "); dbgSerial->println((int)distances.size());
  //   dbgSerial->print("Found walkTimes: "); dbgSerial->println((int)walkTimes.size());
  //   dbgSerial->print("Found stations: "); dbgSerial->println((int)stations.size());
  //   dbgSerial->print("Found departures: "); dbgSerial->println((int)departures.size());
  // }

  auto infos = groupBoardingInfos(routes, distances, walkTimes, stations, departures, MAX_TRANSIT_RESULTS);

  // if (dbgSerial) {
  //   dbgSerial->print("Returning "); dbgSerial->print((int)infos.size()); dbgSerial->println(" BoardingInfo entries:");
  //   for (size_t i = 0; i < infos.size(); ++i) {
  //     const BoardingInfo &b = infos[i];
  //     dbgSerial->print("Option "); dbgSerial->print((int)i); dbgSerial->print(": ");
  //     dbgSerial->print("bus="); dbgSerial->print(b.busLabel);
  //     dbgSerial->print(" dist="); dbgSerial->print(b.walkDistance);
  //     dbgSerial->print(" walkTime="); dbgSerial->print(b.walkTime);
  //     dbgSerial->print(" station="); dbgSerial->print(b.stationName);
  //     dbgSerial->print(" next="); dbgSerial->println(b.nextBusTime);
  //   }
  // }

  return infos;
}



// Public API: parse JSON body and fill an array of BoardingInfo
std::vector<BoardingInfo> parseBoardingInfos(const String &body, BoardingInfo results[], int maxResults) {
  // String jsonPart = extractJsonPartSafe(body); //extractJsonPartChunked(body); //extractJsonPartDirections(body);
  // int start = 11; /* index of first '{' or '[' */
  // int endIndex = findJsonEnd(body, start);
  //   Stream *dbgSerial = &Serial; // or &Serial1
//   size_t MAX_RESULTS = 10;
  std::vector<BoardingInfo> infos = extractBoardingInfosManual(body, dbgSerial, MAX_RESULTS);
  return infos;
}

// --- Heuristic helpers to detect coordinates and names ---
bool isValidLatLon(double a, double b) {
  return (a >= -90.0 && a <= 90.0 && b >= -180.0 && b <= 180.0); //latitude, longitude ranges
}
// --- Pattern matcher: only accept arrays shaped like:
// [ "name-string", "id-string", [ null, null, <lat>, <lon> ] ]
bool matchPatternAndStore(JsonArray arr, Place &out) {
  // Need at least 3 elements: name, id, coords-array
  if (arr.size() < 3) return false;

  // first element must be a string (name)
  if (!arr[0].is<const char*>()) return false;

  // third element must be an array
  if (!arr[2].is<JsonArray>()) return false;

  JsonArray coords = arr[2].as<JsonArray>();
  // coords must have at least 4 elements and positions 2 and 3 must be numeric
  if (coords.size() < 4) return false;
  if (!coords[2].is<double>() || !coords[3].is<double>()) return false;

  // All checks passed: extract values
  out.name = String((const char*)arr[0].as<const char*>());
  out.lat = coords[2].as<double>();
  out.lon = coords[3].as<double>();
  return true;
}

// --- Recursive search that only collects matching patterns ---
void searchForPatterns(JsonVariant v, Place places[], int &count, int maxPlaces) {
  if (count >= maxPlaces) return;

  if (v.is<JsonArray>()) {
    JsonArray arr = v.as<JsonArray>();

    // If this array itself matches the pattern, store it
    if (matchPatternAndStore(arr, places[count])) {
      count++;
      if (count >= maxPlaces) return;
    }

    // Otherwise, recurse into children
    for (JsonVariant item : arr) {
      searchForPatterns(item, places, count, maxPlaces);
      if (count >= maxPlaces) return;
    }
    return;
  }

  if (v.is<JsonObject>()) {
    for (JsonPair kv : v.as<JsonObject>()) {
      searchForPatterns(kv.value(), places, count, maxPlaces);
      if (count >= maxPlaces) return;
    }
    return;
  }

  // primitives: nothing to do
}

String extractJsonPart(const String &raw) {
  int startObj = raw.indexOf('{');
  int startArr = raw.indexOf('[');
  int start = -1;
  if (startObj >= 0 && (startObj < startArr || startArr == -1)) start = startObj;
  else start = startArr;
  if (start == -1) return String();

  int endObj = raw.lastIndexOf('}');
  int endArr = raw.lastIndexOf(']');
  int end = max(endObj, endArr);
  if (end == -1 || end < start) return String();
  return raw.substring(start, end + 1);
}



// --- Parse function: extract JSON, parse, search, return count ---
int parsePlacesFromBody(const String &body, Place places[], int maxPlaces) {
  String jsonPart = extractJsonPart(body);
  if (jsonPart.length() == 0) {
    Serial.println("No JSON found in body");
    return 0;
  }

  // Tune capacity to your payload. Increase if deserializeJson returns NoMemory.
  const size_t capacity = 40000;
  // DynamicJsonDocument doc(capacity);
  JsonDocument doc;

  DeserializationError err = deserializeJson(doc, jsonPart);
  if (err) {
    Serial.print("JSON parse failed: ");
    Serial.println(err.c_str());
    // Print a short snippet for debugging
    Serial.print("Snippet: ");
    Serial.println(jsonPart.substring(0, min(200, int(jsonPart.length()))));
    return 0;
  }

  JsonVariant root = doc.as<JsonVariant>();
  int found = 0;
  searchForPatterns(root, places, found, maxPlaces);
  return found;
}

String setPbCenter(String url, double newLat, double newLon) {
  String token2d = "!2d";
  String token3d = "!3d";
  int i2 = url.indexOf(token2d);
  int i3 = url.indexOf(token3d);
  if (i2 < 0 || i3 < 0 || i3 < i2) return url; // nothing to replace or unexpected order

  // find end of the numeric lon after !2d
  int j = i2 + token2d.length();
  while (j < url.length()) {
    char c = url.charAt(j);
    if (!( (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')) break;
    ++j;
  }
  // find end of numeric lat after !3d
  int k = i3 + token3d.length();
  while (k < url.length()) {
    char c = url.charAt(k);
    if (!( (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')) break;
    ++k;
  }

  // build replacement substring
  String newSub = token2d + String(newLon, 6) + token3d + String(newLat, 6);
  // replace the slice from i2 .. k-1 with newSub
  String out = url.substring(0, i2) + newSub + url.substring(k);
  return out;
}



std::vector<String> getPlaces(String searchQuery, Place places[], int maxPlaces, double newLat, double newLong, bool verbose=false) {
  searchQuery.replace(" ", "+");
  searchQuery.replace(",", "%2C");
  String url = "https://www.google.com/s?tbm=map&gs_ri=maps&suggest=p&authuser=0&hl=en&gl=us&psi=Avghab7tBdbV5NoP9PqxgQ0.1763833866758.1&q=" + searchQuery + "&ech=3&pb=!2i13!4m12!1m3!1d14611.795576010498!2d-79.93046255!3d40.44832804999999!2m3!1f0!2f0!3f0!3m2!1i815!2i924!4f13.1!7i20!10b1!12m25!1m5!18b1!30b1!31m1!1b1!34e1!2m4!5m1!6e2!20e3!39b1!10b1!12b1!13b1!16b1!17m1!3e1!20m3!5e2!6b1!14b1!46m1!1b0!96b1!99b1!19m4!2m3!1i360!2i120!4i8!20m57!2m2!1i203!2i100!3m2!2i4!5b1!6m6!1m2!1i86!2i86!1m2!1i408!2i240!7m33!1m3!1e1!2b0!3e3!1m3!1e2!2b1!3e2!1m3!1e2!2b0!3e3!1m3!1e8!2b0!3e3!1m3!1e10!2b0!3e3!1m3!1e10!2b1!3e2!1m3!1e10!2b0!3e4!1m3!1e9!2b1!3e2!2b1!9b0!15m8!1m7!1m2!1m1!1e2!2m2!1i195!2i195!3i20!22m3!1sURMlaa7DOpPe5NoP99fnkQY!7e81!17sURMlaa7DOpPe5NoP99fnkQY%3A63!23m2!4b1!10b1!24m109!1m30!13m9!2b1!3b1!4b1!6i1!8b1!9b1!14b1!20b1!25b1!18m19!3b1!4b1!5b1!6b1!9b1!13b1!14b1!17b1!20b1!21b1!22b1!27m1!1b0!28b0!32b1!33m1!1b1!34b1!36e2!10m1!8e3!11m1!3e1!14m1!3b0!17b1!20m2!1e3!1e6!24b1!25b1!26b1!27b1!29b1!30m1!2b1!36b1!37b1!39m3!2m2!2i1!3i1!43b1!52b1!54m1!1b1!55b1!56m1!1b1!61m2!1m1!1e1!65m5!3m4!1m3!1m2!1i224!2i298!72m22!1m8!2b1!5b1!7b1!12m4!1b1!2b1!4m1!1e1!4b1!8m10!1m6!4m1!1e1!4m1!1e3!4m1!1e4!3sother_user_google_review_posts__and__hotel_and_vr_partner_review_posts!6m1!1e1!9b1!89b1!98m3!1b1!2b1!3b1!103b1!113b1!114m3!1b1!2m1!1b1!117b1!122m1!1b1!126b1!127b1!26m4!2m3!1i80!2i92!4i8!34m19!2b1!3b1!4b1!6b1!8m6!1b1!3b1!4b1!5b1!6b1!7b1!9b1!12b1!14b1!20b1!23b1!25b1!26b1!31b1!37m1!1e81!47m0!49m10!3b1!6m2!1b1!2b1!7m2!1e3!2b1!8b1!9b1!10e2!61b1!67m5!7b1!10b1!14b1!15m1!1b0!69i760";
  // String url = "https://www.google.com/s?tbm=map&gs_ri=maps&suggest=p&authuser=0&hl=en&psi=Avghab7tBdbV5NoP9PqxgQ0.1763833866758.1&q=" + searchQuery + "&ech=3";
  url = setPbCenter(url, newLat, newLong); //NYC coords for testing

  String body = httpGetStream(url);
  logMessage("Response length: " + String(body.length()));
  // dbgSerial->println(body); // or parse it
  std::vector<String> placeRetStrings;
  
  int count = parsePlacesFromBody(body, places, MAX_RESULTS);

  dbgSerial->printf("Found %d places:\n", count);
  for (int i = 0; i < count; ++i) {
    // dbgSerial->printf("%d) %s -> %f, %f\n", i+1, places[i].name.c_str(), places[i].lat, places[i].lon);
    placeRetStrings.push_back(places[i].name.c_str());
    if (verbose) {
      dbgSerial->printf("%s\n", places[i].name.c_str());
    }
  }
  return placeRetStrings;
}

BoardingInfo infos[MAX_RESULTS];



std::vector<BoardingInfo> getDirections(String start, String end, double newLat, double newLong, bool verbose=false) {
  start.replace(" ", "+");
  start.replace(",", "%2C");
  end.replace(" ", "+");
  end.replace(",", "%2C");
  String url = "https://www.google.com/maps/preview/directions?authuser=0&hl=en&gl=us&pb=!1m7!1s" + start + "!2s0x8834f20bad463bcb%3A0x4104e286b57ee3d5!3m2!3d40.4546065!4d-79.92213079999999!6e0!19sChIJyztGrQvyNIgR1eN-tYbiBEE!1m5!1s" + end + "!2s0x8834ee236ec7350f%3A0x73fa84093902b486!3m2!3d40.4120663!4d-79.90993689999999!3m15!1m3!1d3652.469221595039!2d-79.92302947339881!3d40.45715232143318!2m3!1f0!2f0!3f0!3m2!1i413!2i924!4f13.1!6m2!1f0!2f0!6m48!1m5!18b1!30b1!31m1!1b1!34e1!2m4!5m1!6e2!20e3!39b1!6m18!49b1!66b1!74i150000!85b1!91b1!114b1!149b1!178b1!206b1!212b1!213b1!223b1!227b1!232b1!233b1!244b1!246b1!250b1!10b1!12b1!13b1!14b1!16b1!17m2!3e1!3e1!20m5!1e3!2e3!5e2!6b1!14b1!46m1!1b0!96b1!99b1!15m4!1s4comaYyYFdie5NoP6PLzmAQ!4m1!2i10147!7e81!20m0!27b1!28m0!40i760!47m2!8b1!10e2!50sAMAbHIJ9Z-8tJDm9cAYXtpsZjRf8BsO2uA%3A1764149883184";
  url = setPbCenter(url, newLat, newLong); // set to user's location
  // dbgSerial->println("DirectionsURL: " + url);
  // return 0;
  String body = httpGetStream(url);
  logMessage("Directions response length: " + String(body.length()));
  // dbgSerial->println("Directions response: " + body.substring(0, min(200, int(body.length())))); // print first 200 chars

  std::vector<BoardingInfo> n = parseBoardingInfos(body, infos, MAX_RESULTS);
  // return;

  logMessage("Found " + String(n.size()) + " boarding entries.");
  // dbgSerial->print(n);
  // dbgSerial->println(" boarding entries:");
  if (verbose) {
    for (int i = 0; i < n.size(); ++i) {
      dbgSerial->println("---");
      dbgSerial->print("Bus: "); dbgSerial->println(n[i].busLabel);
      dbgSerial->print("Walk distance: "); dbgSerial->println(n[i].walkDistance);
      dbgSerial->print("Walk time: "); dbgSerial->println(n[i].walkTime);
      dbgSerial->print("Station: "); dbgSerial->println(n[i].stationName);
      dbgSerial->print("Next bus: "); dbgSerial->println(n[i].nextBusTime);
    }
  }
  


  // dbgSerial->println(body); // or parse it
  
  
  // int count = parsePlacesFromBody(body, places, MAX_RESULTS);

  // dbgSerial->printf("Found %d places:\n", count);
  // for (int i = 0; i < count; ++i) {
  //   dbgSerial->printf("%d) %s -> %f, %f\n", i+1, places[i].name.c_str(), places[i].lat, places[i].lon);
  // }
  return n;
}


currentLocation c;
String appendChosen = "Chosen: ";


float newWalkTime(float oldWalkTime, float oldMiles, float newMiles) {
  if (oldMiles <= 0 || newMiles <= 0) {
    return WALKTIME_NONE; // invalid input
  }
  float pace = oldWalkTime / oldMiles; // minutes per mile
  float newWalkTime = pace * newMiles;
  return newWalkTime;
}

int chooseWalkTime() {
  float walkTime = WALKTIME_NONE;
  String walkText;
  String lastMsg = "";
  String newMsg = "";
  bool goodWalkTime = false;
  while(true) {
    goodWalkTime = false;
    sendCommand("get t2.txt");
    walkText = getButtonText(*nextionSerial);
    sendCommand("t4.txt=\"Entered: " + walkText + " Length " + String(walkText.length()) + "\""); //Echo back for testing
    if (walkText.length() == 0) {
      newMsg = "Valid - Empty WalkTime";
      goodWalkTime = true;
      if(lastMsg != newMsg) {
        lastMsg = newMsg;
        sendCommand("t3.txt=\"" + newMsg + "\"");
        sendCommand("t3.pco="+PCO_COLORS.green); //Make it green before connecting
      }
    } 
    else {
      // newMsg = "Error - Numbers Only";
      // newMsg = "WalkTime: " + walkText + " min - Length of walktext is " + String(walkText.length());
      // if(lastMsg != newMsg) {
      //   lastMsg = newMsg;
      //   sendCommand("t3.txt=\"" + newMsg + "\"");
      //   sendCommand("t3.pco="+PCO_COLORS.red); //Make it green before connecting
      // }
      char *endptr;
      long val = strtol(walkText.c_str(), &endptr, 10); // base 10

      if (endptr == walkText.c_str()) {
        // no conversion performed -> parse error
        newMsg = "Error - Numbers Only";
        if(lastMsg != newMsg) {
          lastMsg = newMsg;
          sendCommand("t3.txt=\"" + newMsg + "\"");
          sendCommand("t3.pco="+PCO_COLORS.red); //Make it green before connecting
        }
      } else {
        // val holds parsed number; endptr points to first non-digit
        walkTime = (int)val;
        newMsg = "Valid - " + String(walkTime) + " min";
        goodWalkTime = true;
        if(lastMsg != newMsg) {
          lastMsg = newMsg;
          sendCommand("t3.txt=\"" + newMsg + "\"");
          sendCommand("t3.pco="+PCO_COLORS.green); //Make it green before connecting
        }
      }

    }
    if (goodWalkTime) {
      int compId = waitForButtonPress(*nextionSerial, 100);
      if (compId == 4) {
        String forLog = "Chosen WalkTime: " + String(walkTime) + " min";
        logMessage(forLog);
        // sendCommand("t5.txt=\"" + forLog + "\"");
        break;
        }
    
      }
  }
  return walkTime;
  // 
  // sendCommand("get " + HOME_PAGE_START_TXT + ".txt");
}

String chooseAddress() {
  int PLACE_MAX = 3;
  std::vector<String> startPlacesSearch;
  
  String priorStartText;
  String startText;
  String chosenStart;
  bool continueSuccessful = false;
  while (!continueSuccessful) {
    // buttonText btStart = GetStartAddress();
    sendCommand("get " + HOME_PAGE_START_TXT + ".txt");
    startText = getButtonText(*nextionSerial);

    
    if (startText != priorStartText && startText.length() > 0) {
      logMessage("Start text: " + startText);
      priorStartText = startText;
      startPlacesSearch = getPlaces(startText, placesStart, PLACE_MAX, c.lat, c.lon);
      // startPlacesSearch = getPlaces("new york", placesStart, PLACE_MAX, c.lat, c.lon);
      if (!startPlacesSearch.empty()) {
        sendComponentTxt(PLACE_MAX, 50, startPlacesSearch, "b", false, 1);
      }
      
      
            
    }

    int compId = waitForButtonPress(*nextionSerial, 100);
    if (ADDR_PAGES.count(compId)) {
      logMessage("Button pressed, compId: " + String(compId));
      // sendCommand("get b" + String(compId) + ".txt"); // get text of button pressed
      sendCommand("get " + ADDR_PAGES[compId] + ".txt");
      String text = getButtonText(*nextionSerial); // flush any prior response
        if (text.length() > 0) {
          chosenStart = text;
          String forLog = appendChosen + chosenStart;
          logMessage(forLog);
          // safeSetPage("EndAddr");
          // break;
          sendCommand("t5.txt=\"" + forLog + "\"");
          

        }
      } 
      else if (compId == ADDR_CONTINUE_ID) { //Continue button 
        if (chosenStart.length() > 0) {
          sendCommand("t1.txt=\"\""); // clear any prior error
          // safeSetPage("EndAddr");
          // break;
          continueSuccessful = true;
        } else {
          // logMessage("No start address chosen yet");
          sendCommand("t1.txt=\"Error, select address\"");
        }
      }
  }
  return chosenStart;
}

int indexOf(const std::vector<String>& v, const String& target) {
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == target) return int(i);
  }
  return -1; // not found
}

bool parseUnsignedLongLong(const String &s, unsigned long long &out) {
  out = 0ULL;
  bool seen = false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c >= '0' && c <= '9') {
      seen = true;
      out = out * 10ULL + (unsigned long long)(c - '0');
    } else {
      // stop at first non-digit after we've started
      if (seen) break;
      // if not started and non-digit, skip
    }
  }
  return seen;
}

String secondToken(const String &s) {
  int n = s.length();
  int i = 0;
  // skip leading whitespace
  while (i < n && isspace((unsigned char)s[i])) ++i;
  // skip first token
  while (i < n && !isspace((unsigned char)s[i])) ++i;
  // skip whitespace between tokens
  while (i < n && isspace((unsigned char)s[i])) ++i;
  if (i >= n) return String("");
  int start = i;
  while (i < n && !isspace((unsigned char)s[i])) ++i;
  return s.substring(start, i);
}





String getTimeNowUTC(bool verbose=false) {
  // "https://time.is/" --> Derived from this time, the parameters here are not open source, need to just have the user visit the site and we need to scrape it? Or just try this one
  String getUrlForTime = "https://time.is/t1/?en.0.10.955.0P.-300.161.176473717963.176497716389..N";
  String body = httpGetStream(getUrlForTime);
  
  /*
  1c2
  1764975381962
  0
  161_(UTC,_UTC+00:00)
  176473717963
  */
  // Get 1764975381962, being the first line, from the example above

  String epochMillis = secondToken(body);
  if (verbose) {
    logMessage("Time body: " + body);
    dbgSerial->println("Time Response: " + body);
    
  }
  logMessage("UTCNow Time found " + String(epochMillis));
  return epochMillis;
  // int epochSeconds = int(epochMillis / 1000);
  // logMessage("Parsed epoch seconds: " + String(epochSeconds));
  


}



const char* iana_to_posix(const char* iana) {
  if (strcmp(iana, "America/New_York") == 0) return "EST5EDT,M3.2.0/2,M11.1.0/2";
  if (strcmp(iana, "Europe/Helsinki") == 0) return "EET-2EEST,M3.5.0/3,M10.5.0/4";
  // add other zones you need
  return "UTC0";
}


String startAddr = "";
String endAddr = "";


void block_chooseAddress() {
  safeSetPage("StartAddr");
  startAddr = chooseAddress();
  saveSetting(CONST_KEYS.startAddr.c_str(), startAddr.c_str());
  
  safeSetPage("EndAddr");
  endAddr = chooseAddress();
  saveSetting(CONST_KEYS.endAddr.c_str(), endAddr.c_str());
}


void block_chooseWifi() {
  int compId = -1;
  if (!SKIP_WIFI_SELECTION) {
    buttonText bt = SelectWifi();
    String text = bt.text;
    compId = bt.compId;
    
    // After selecting wifi, go to the next page
    safeSetPage("WifiInput");
    
    if (text != "Unlisted") {
      // ssidTest = (char*)text.c_str();
      sendCommand("t2.txt=\"" + text + "\""); // set SSID field
    }

  }
}

WifiCredentials block_wifiLogin() {
  int compId = -1;
  String username;
  String password;
  WifiCredentials creds = WifiCredentials();
  buttonText wl;

  if (!SKIP_WIFI_LOGIN) {
    while (!creds.ok) {
      wl = WifiLogin();
      creds.ok = wl.connected;
    }

    username = wl.ssid;
    password = wl.pass;
    creds.ok = wl.connected;

    
  } 
  return creds;
}

float walkTime = WALKTIME_NONE;
float milesComputeSaved = WALKTIME_NONE;
String bus = "";
String firstBusOnlySaved = "false";
int reloadAttempt = 0;

void block_walkTimeBus() {
  std::vector<String> transitList;
  std::vector<String> walkTimeList;
  std::vector<String> walkDistList;

  if (walkTime == WALKTIME_NONE || milesComputeSaved == WALKTIME_NONE || bus.length() == 0 || startAddr.length() == 0 || endAddr.length() == 0) {
    String dist;
    String walk; //Local computed from first bus initially
    // String bus;
    if(!SKIP_FIRST_BUS_SELECT) {
      // From page FirstTransit, they need to select the bus so I get the distance and walktime, so I can set it relatively from there.
      // Get miles/dist, save it, then go to walk page so use knows what they're specifying
      // Both this and the SKIP_WALKTIME_SET really both have to be ran
      safeSetPage("FirstTransit");

      // locals.startAddr = 
      // locals.endAddr = 
      std::vector<BoardingInfo> n = getDirections(startAddr, endAddr, c.lat, c.lon);

      
      for (int i = 0; i < n.size(); ++i) {
        transitList.push_back(n[i].busLabel);
        // dbgSerial->println("---");
        dbgSerial->print("Bus: "); dbgSerial->println(n[i].busLabel);
        walkTimeList.push_back(n[i].walkTime);
        walkDistList.push_back(n[i].walkDistance);
        // dbgSerial->print("Walk distance: "); dbgSerial->println(infos[i].walkDistance);
        // dbgSerial->print("Walk time: "); dbgSerial->println(infos[i].walkTime);
        // dbgSerial->print("Station: "); dbgSerial->println(infos[i].stationName);
        // dbgSerial->print("Next bus: "); dbgSerial->println(infos[i].nextBusTime);
      }
      int remaining = MAX_TRANSIT_RESULTS - int(n.size());
      // logMessage("Any remaining blanks " + String((MAX_TRANSIT_RESULTS - int(n.size()))));
      for (int i = 0; i < remaining; i++) {
        transitList.push_back(""); // pad out to max
      }

      sendComponentTxt(MAX_TRANSIT_RESULTS, 10, transitList, "b", false, 0);

      while(true) {
        int compId = waitForButtonPress(*nextionSerial, 10000);
        if (TRANSIT_BUSES.count(compId)) {
          logMessage("Button pressed, compId: " + String(compId));
          // sendCommand("get b" + String(compId) + ".txt"); // get text of button pressed
          sendCommand("get " + TRANSIT_BUSES[compId] + ".txt");
          String busIdx = getButtonText(*nextionSerial); // flush any prior response
            if (busIdx.length() > 0 && busIdx != "Loading...") {
              String forLog = appendChosen + busIdx;
              // Get index of text in transitList
              int idx = indexOf(transitList, busIdx);
              dist = walkDistList[idx];
              walk = walkTimeList[idx];
              bus = busIdx;
              // logMessage(forLog);
              sendCommand("t2.txt=\"" + forLog + "\"");
              
              logMessage("Selected bus with walk distance: " + dist + ", walk time: " + walk + ", bus: " + bus);
              // break;
            }
        }
        if (compId == 5) {
          //Continue button 
          if (dist.length() > 0 && walk.length() > 0) {
            walkTime = minutesStringToFloat(walk);//.toInt();
            milesComputeSaved = distanceStringToMiles(dist);
            String forLog = "Using Walk Distance: " + String(milesComputeSaved) + ", Walk Time: " + String(walkTime) + ", Bus: " + bus;
            logMessage(forLog);
            // sendCommand("t1.txt=\"" + forLog + "\""); // show what we're using
            
            sendCommand("t3.txt=\"\"");
            break;
          } else {
            sendCommand("t3.txt=\"Error, select transit option\"");
          }
        }
      }

    } else {
      // Testing
      // walkTime = 10;
      bus = "64";
      
      // safeSetPage("Walk");
    }

    if (!SKIP_WALKTIME_SET) {
      safeSetPage("Walk");

      float walkTimeFound = chooseWalkTime();

      logMessage("User specified walk time: " + String(walkTimeFound) + " min " + "(was " + String(walkTime) + " min)");
      String equalityCheck = (walkTimeFound == walkTime) ? "true" : "false";
      // logMessage("User specified walk time equals prior walk time? " + equalityCheck);

      if (int(walkTimeFound) == int(WALKTIME_NONE)) {
        // walkTimeFound = WALKTIME_DEFAULT; //We specified none prior
        walkTimeFound = walkTime; //Use prior loaded walktime
      }
      

      logMessage("Final Walk Time chosen: " + String(walkTimeFound) + " min");
      saveSetting(CONST_KEYS.walkTime.c_str(), String(walkTimeFound).c_str());
      saveSetting(CONST_KEYS.milesCompute.c_str(), String(milesComputeSaved).c_str());
      saveSetting(CONST_KEYS.bus.c_str(), bus.c_str());
      safeSetPage("InfoPage");
    } else {
      walkTime = 10;
      milesComputeSaved = 0.5;
    }
  }
}


void block_infoPage() {
  safeSetPage("InfoPage");

  if (minute_changed_now()) {
    std::vector<String> transitList;
    std::vector<String> walkTimeList;
    std::vector<String> walkDistList;

    std::vector<BoardingInfo> n = getDirections(startAddr, endAddr, c.lat, c.lon);

    
    for (int i = 0; i < n.size(); ++i) {
      transitList.push_back(n[i].busLabel);
      // dbgSerial->println("---");
      dbgSerial->print("Bus: "); dbgSerial->println(n[i].busLabel);
      walkTimeList.push_back(n[i].walkTime);
      walkDistList.push_back(n[i].walkDistance);
      // dbgSerial->print("Walk distance: "); dbgSerial->println(infos[i].walkDistance);
      // dbgSerial->print("Walk time: "); dbgSerial->println(infos[i].walkTime);
      // dbgSerial->print("Station: "); dbgSerial->println(infos[i].stationName);
      // dbgSerial->print("Next bus: "); dbgSerial->println(infos[i].nextBusTime);
    }
    int remaining = MAX_TRANSIT_RESULTS - int(n.size());
    // logMessage("Any remaining blanks " + String((MAX_TRANSIT_RESULTS - int(n.size()))));
    for (int i = 0; i < remaining; i++) {
      transitList.push_back(""); // pad out to max
    }
    sendCommand("get c0.val");
    String useBestBusStr = getButtonText(*nextionSerial); // flush any prior response
    bool useBestBus = (useBestBusStr == "1");
    // startAddr = "434 Shady Ave, Pittsburgh, PA 15206";
    // endAddr = "400 E Waterfront Dr, Homestead, PA 15120";
    // std::vector<BoardingInfo> n = getDirections(startAddr, endAddr, c.lat, c.lon);

    // Check if bus is in the list
    if (bus.length() > 0) {
      logMessage("Checking for saved bus: " + bus);
      int busIdx = indexOf(transitList, bus);
      if (busIdx >= 0) {
        logMessage("Saved bus found in transit list: " + bus + ", index: " + String(busIdx));
        // Filter n to only have this bus
        std::vector<BoardingInfo> nFiltered;
        nFiltered.push_back(n[busIdx]);
        n = nFiltered;
      } else {
        logMessage("Saved bus not found in transit list: " + bus + ", using first available bus");
        // If not found, and useBestBus is false, just use first bus
        if (!useBestBus && n.size() > 1) {
          std::vector<BoardingInfo> nFiltered;
          nFiltered.push_back(n[0]);
          n = nFiltered;
        }
      }
    } else {
      logMessage("No saved bus specified, using first available bus");
      // If no bus specified, and useBestBus is false, just use first bus
      if (!useBestBus && n.size() > 1) {
        std::vector<BoardingInfo> nFiltered;
        nFiltered.push_back(n[0]);
        n = nFiltered;
      }
    }

    if (n.size() > 0) {
      String utcTimeNow = getTimeNowUTC();
      String nextBusTimeStr = n[0].nextBusTime;
      float walkDistNew = distanceStringToMiles(n[0].walkDistance);
      int diffTimeMin = minutesDifferenceFromEpochMs(utcTimeNow, nextBusTimeStr.c_str());
      String busUsed = n[0].busLabel;
      String firstLocation = n[0].stationName;

      float relativeNewWalkTime = newWalkTime(walkTime, milesComputeSaved, walkDistNew);

      // int nextBusTime = minutesDifferenceFromEpochMs(utcTimeNow, n[i].nextBusTime.c_str());
      // for (int i = 0; i < n.size(); ++i) {
        // dbgSerial->print("Bus: "); dbgSerial->println(n[i].busLabel);
        // dbgSerial->print("Walk distance: "); dbgSerial->println(n[i].walkDistance);
        // dbgSerial->print("Walk time: "); dbgSerial->println(n[i].walkTime);
        // dbgSerial->print("Station: "); dbgSerial->println(n[i].stationName);
        // dbgSerial->print("Next bus: "); dbgSerial->println(n[i].nextBusTime);
        // 
        

        // dbgSerial->print("Next bus in minutes: "); dbgSerial->println(nextBusTime);
      // }
      logMessage("Using bus: " + busUsed + ", First location: " + firstLocation + ", In relation to our time next bus is in: " + String(diffTimeMin) + " min, " + "Next bus time: " + nextBusTimeStr + ", Original walk time: " + String(walkTime) + " min, Original miles: " + String(milesComputeSaved) + " New walk time: " + String(relativeNewWalkTime) + " min" + ", For walk distance of " + String(walkDistNew) + " miles");
      float timeToLeaveFloat = diffTimeMin - relativeNewWalkTime;
      int timeToLeave = roundf(timeToLeaveFloat);
      String leaveMsg;
      if (timeToLeave < 0) {
        leaveMsg = "Now (Run )" + String(timeToLeave * -1) + " min faster";
      } else if (timeToLeave == 0) {
        leaveMsg = "Now";
      } else {
        leaveMsg = String(timeToLeave) + " min";
      }
      sendCommand("t2.txt=\"" + leaveMsg + "\"");
      sendCommand("t4.txt=\"" + busUsed + "\"");
      sendCommand("t7.txt=\"" + firstLocation + "\"");
    } else {
      logMessage("No transit options found for the given addresses.");
      String makeReloadStr = (reloadAttempt < 10) ? " (Reload Attempt " + String(reloadAttempt) + ")" : "(More than 10 reloads)";
      // Log unfound messages or just do something here, suggest re-choosing addresses
      sendCommand("t2.txt=\"N/A " + makeReloadStr + "\"");
      sendCommand("t4.txt=\"N/A\"");
      sendCommand("t7.txt=\"N/A\"");
      if (reloadAttempt < 10) {
        reloadAttempt += 1;
      }
      
    }

    

  }
}




void setup() {
  // put your setup code here, to run once:
  Serial.begin(USB_BAUD);
  // Serial.println("hello");

 

  
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


  
  // stop cleanly
  WiFi.mode(WIFI_OFF);
  delay(200);

  

  // fully disconnect and clear config
  WiFi.disconnect(true);
  delay(200);

  WiFi.mode(WIFI_STA); //Set wifi to station mode
  
  // delay(100);   //Would remove prior connections, it stores it by default, could check to see if it's connected off the bat
  WifiCredentials creds;
  if (FAKE_WIFI) { // This is for testing really
    saveSetting(CONST_KEYS.ssid.c_str(), ssidTest);
    saveSetting(CONST_KEYS.pass.c_str(), passwordTest); 
  }

  if (RESET_SAVED_ADDRS){
    saveSetting(CONST_KEYS.startAddr.c_str(), "");
    saveSetting(CONST_KEYS.endAddr.c_str(), "");
  }

  if (RESET_SAVED_WALKTIME) {
    saveSetting(CONST_KEYS.walkTime.c_str(), String(WALKTIME_NONE).c_str());
    saveSetting(CONST_KEYS.milesCompute.c_str(), String(WALKTIME_NONE).c_str());
    saveSetting(CONST_KEYS.bus.c_str(), ""); //Reset bus as well
    saveSetting(CONST_KEYS.firstBusOnly.c_str(), "false");
    
  }

  creds.ssid = loadStringSetting(CONST_KEYS.ssid.c_str());
  creds.pass = loadStringSetting(CONST_KEYS.pass.c_str());
  startAddr = loadStringSetting(CONST_KEYS.startAddr.c_str());
  endAddr = loadStringSetting(CONST_KEYS.endAddr.c_str());
  walkTime = loadStringSetting(CONST_KEYS.walkTime.c_str()).toFloat(); //This is fine as it'll be stored properly everytime, no parsing needed.
  milesComputeSaved = loadStringSetting(CONST_KEYS.milesCompute.c_str()).toFloat();
  bus = loadStringSetting(CONST_KEYS.bus.c_str()); //Tries for this bus, otherwise it will just default to the first one found, able to also turn off this setting
  firstBusOnlySaved = loadStringSetting(CONST_KEYS.firstBusOnly.c_str());

  logMessage("Loaded walkTime : " + String(walkTime) + ", milesComputeSaved: " + String(milesComputeSaved) + ", bus: " + bus + ", firstBusOnlySaved: " + firstBusOnlySaved);
  logMessage("Walktime equals WALKTIME_NONE? " + String(walkTime == WALKTIME_NONE ? "true" : "false"));
  logMessage("Loaded Start Addr: " + startAddr + ", End Addr: " + endAddr);
  
  // Wifi may not have a password
  
  if (creds.ssid.length() == 0) { //|| !FAKE_WIFI
    logMessage("No prior SSID");
  } else {
    if (!OVERRIDE_WIFI) {
      logMessage("Prior SSID, try to connect to wifi, SSID: " + creds.ssid + ", PASS: " + creds.pass);
      // connectionSequence(true);
      connected = tryWifi(creds.ssid.c_str(), creds.pass.c_str());
      creds.ok = connected;
      String tmpOk = creds.ok ? "true" : "false";
      logMessage("Actually successfully connected: " + tmpOk);
    } else {
      logMessage("OVERRIDE_WIFI is set, skipping WIFI connection attempt");
    }
    
  }

  if (!connected) {
    block_chooseWifi();
    creds = block_wifiLogin();
  }
  
  if (creds.ok || OVERRIDE_WIFI) {
    String msgLog = OVERRIDE_WIFI ? "OVERRIDE_WIFI set, starting setup sequence" : "WiFi connected successfully, starting setup sequence";
    logMessage(msgLog);
    if (creds.ok) {
      c = getCurrentLocation();
      String userTimezone = getUserTimezone();
      const char* posix = iana_to_posix(userTimezone.c_str());
      logMessage("User timezone: " + userTimezone + ", POSIX: " + String(posix));
      logMessage("User lat: " + String(c.lat) + ", lon: " + String(c.lon));
      // configTzTime("EST5EDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org");
      configTzTime(posix, "pool.ntp.org");
      // configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // sync UTC
      // setenv("TZ", "PST8PDT", 1); // or "EST5EDT" or a full TZ string
      // tzset();

    }
    if (startAddr.length() == 0 || endAddr.length() == 0) {

      
      // SEARCH QUERY SEQUENCE
      // String url = "https://www.google.com/s?tbm=map&gs_ri=maps&suggest=p&authuser=0&hl=en&gl=us&psi=Avghab7tBdbV5NoP9PqxgQ0.1763833866758.1&q=Tw&ech=7&pb=!2i2!4m12!1m3!1d14611.795576010498!2d-79.93046255!3d40.44832804999999!2m3!1f0!2f0!3f0!3m2!1i1298!2i924!4f13.1!7i20!10b1!12m25!1m5!18b1!30b1!31m1!1b1!34e1!2m4!5m1!6e2!20e3!39b1!10b1!12b1!13b1!16b1!17m1!3e1!20m3!5e2!6b1!14b1!46m1!1b0!96b1!99b1!19m4!2m3!1i360!2i120!4i8!20m57!2m2!1i203!2i100!3m2!2i4!5b1!6m6!1m2!1i86!2i86!1m2!1i408!2i240!7m33!1m3!1e1!2b0!3e3!1m3!1e2!2b1!3e2!1m3!1e2!2b0!3e3!1m3!1e8!2b0!3e3!1m3!1e10!2b0!3e3!1m3!1e10!2b1!3e2!1m3!1e10!2b0!3e4!1m3!1e9!2b1!3e2!2b1!9b0!15m8!1m7!1m2!1m1!1e2!2m2!1i195!2i195!3i20!22m3!1sAvghab7tBdbV5NoP9PqxgQ0!7e81!17sAvghab7tBdbV5NoP9PqxgQ0%3A83!23m2!4b1!10b1!24m109!1m30!13m9!2b1!3b1!4b1!6i1!8b1!9b1!14b1!20b1!25b1!18m19!3b1!4b1!5b1!6b1!9b1!13b1!14b1!17b1!20b1!21b1!22b1!27m1!1b0!28b0!32b1!33m1!1b1!34b1!36e2!10m1!8e3!11m1!3e1!14m1!3b0!17b1!20m2!1e3!1e6!24b1!25b1!26b1!27b1!29b1!30m1!2b1!36b1!37b1!39m3!2m2!2i1!3i1!43b1!52b1!54m1!1b1!55b1!56m1!1b1!61m2!1m1!1e1!65m5!3m4!1m3!1m2!1i224!2i298!72m22!1m8!2b1!5b1!7b1!12m4!1b1!2b1!4m1!1e1!4b1!8m10!1m6!4m1!1e1!4m1!1e3!4m1!1e4!3sother_user_google_review_posts__and__hotel_and_vr_partner_review_posts!6m1!1e1!9b1!89b1!98m3!1b1!2b1!3b1!103b1!113b1!114m3!1b1!2m1!1b1!117b1!122m1!1b1!126b1!127b1!26m4!2m3!1i80!2i92!4i8!34m19!2b1!3b1!4b1!6b1!8m6!1b1!3b1!4b1!5b1!6b1!7b1!9b1!12b1!14b1!20b1!23b1!25b1!26b1!31b1!37m1!1e81!47m0!49m10!3b1!6m2!1b1!2b1!7m2!1e3!2b1!8b1!9b1!10e2!61b1!67m5!7b1!10b1!14b1!15m1!1b0!69i759"; // example (fragile)
      // searchQuery = "434";
      // getPlaces(searchQuery, places, MAX_RESULTS, c.lat, c.lon, true);
      // dbgSerial->println("----");

      // // DIRECTIONS SEQUENCE
      
      
      
      
      if (!SKIP_ADDR_CHOOSE) {
        block_chooseAddress();
        


      } else {
        // logMessage("SKIP_ADDR_CHOOSE set, using prior addresses");
        startAddr = "434 Shady Ave, Pittsburgh, PA 15206";
        endAddr = "400 E Waterfront Dr, Homestead, PA 15120";
      }
      
    }

    block_walkTimeBus();
    

    if (!SKIP_INFO_PAGE) {
      block_infoPage();
        

      
    }


    
    // logMessage("Current location: " + String(c.lat, 6) + ", " + String(c.lon, 6));
    
    
    // logMessage("Start text: " + startText);
    // sendCommand("get " + NO_WIFI_PAGE_MAP[compId] + ".txt");
    // text = getButtonText(*nextionSerial); // flush any prior response


    
    // searchQuery.replace(" ", "+");
    // String url = "https://www.google.com/s?tbm=map&gs_ri=maps&suggest=p&authuser=0&hl=en&gl=us&psi=Avghab7tBdbV5NoP9PqxgQ0.1763833866758.1&q=" + searchQuery + "&ech=3&pb=!2i13!4m12!1m3!1d14611.795576010498!2d-79.93046255!3d40.44832804999999!2m3!1f0!2f0!3f0!3m2!1i815!2i924!4f13.1!7i20!10b1!12m25!1m5!18b1!30b1!31m1!1b1!34e1!2m4!5m1!6e2!20e3!39b1!10b1!12b1!13b1!16b1!17m1!3e1!20m3!5e2!6b1!14b1!46m1!1b0!96b1!99b1!19m4!2m3!1i360!2i120!4i8!20m57!2m2!1i203!2i100!3m2!2i4!5b1!6m6!1m2!1i86!2i86!1m2!1i408!2i240!7m33!1m3!1e1!2b0!3e3!1m3!1e2!2b1!3e2!1m3!1e2!2b0!3e3!1m3!1e8!2b0!3e3!1m3!1e10!2b0!3e3!1m3!1e10!2b1!3e2!1m3!1e10!2b0!3e4!1m3!1e9!2b1!3e2!2b1!9b0!15m8!1m7!1m2!1m1!1e2!2m2!1i195!2i195!3i20!22m3!1sURMlaa7DOpPe5NoP99fnkQY!7e81!17sURMlaa7DOpPe5NoP99fnkQY%3A63!23m2!4b1!10b1!24m109!1m30!13m9!2b1!3b1!4b1!6i1!8b1!9b1!14b1!20b1!25b1!18m19!3b1!4b1!5b1!6b1!9b1!13b1!14b1!17b1!20b1!21b1!22b1!27m1!1b0!28b0!32b1!33m1!1b1!34b1!36e2!10m1!8e3!11m1!3e1!14m1!3b0!17b1!20m2!1e3!1e6!24b1!25b1!26b1!27b1!29b1!30m1!2b1!36b1!37b1!39m3!2m2!2i1!3i1!43b1!52b1!54m1!1b1!55b1!56m1!1b1!61m2!1m1!1e1!65m5!3m4!1m3!1m2!1i224!2i298!72m22!1m8!2b1!5b1!7b1!12m4!1b1!2b1!4m1!1e1!4b1!8m10!1m6!4m1!1e1!4m1!1e3!4m1!1e4!3sother_user_google_review_posts__and__hotel_and_vr_partner_review_posts!6m1!1e1!9b1!89b1!98m3!1b1!2b1!3b1!103b1!113b1!114m3!1b1!2m1!1b1!117b1!122m1!1b1!126b1!127b1!26m4!2m3!1i80!2i92!4i8!34m19!2b1!3b1!4b1!6b1!8m6!1b1!3b1!4b1!5b1!6b1!7b1!9b1!12b1!14b1!20b1!23b1!25b1!26b1!31b1!37m1!1e81!47m0!49m10!3b1!6m2!1b1!2b1!7m2!1e3!2b1!8b1!9b1!10e2!61b1!67m5!7b1!10b1!14b1!15m1!1b0!69i760";
    // String body = httpGetStream(url);
    // logMessage("Response length: " + String(body.length()));
    // // dbgSerial->println(body); // or parse it
    
    
    // int count = parsePlacesFromBody(body, places, MAX_RESULTS);

    // dbgSerial->printf("Found %d places:\n", count);
    // for (int i = 0; i < count; ++i) {
    //   dbgSerial->printf("%d) %s -> %f, %f\n", i+1, places[i].name.c_str(), places[i].lat, places[i].lon);
    // }
  } else {
    logMessage("Wi-Fi not connected");
    // sendCommand("page NoWifi");
    // safeSetPage("NoWifi");
  }


  
  // if (!FAKE_WIFI) {
  //   WiFi.disconnect(); 
  //   connected = tryWifi(ssidTest, passwordTest); // TEMP for testing so we can have our server
  // }
  // logMessage("Test compute walktime of 10 min for 0.5 miles with new miles of 1.2: " + String(newWalkTime(10, 0.5, 1.2)) + " min");
  
        
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
  // while(1) {
  //   if (minute_changed_now()) {
  //     logMessage("Minute changed now");
  //     // checkForUpdates();
  //   } else {
  //     // break;
  //   }
  // }
  server.handleClient(); 


  // dbgSerial->println(seconds_until_next_minute());


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
// pio device monitor -p COM3 -b 115200 --filter esp32_exception_decoder


/*
Notes on internally what query means
**Short answer:** The two URLs differ only in opaque, internal Google UI parameters — the trailing `i759` vs `i760` is an internal index/token used by Google’s encoded `pb` payload (not a stable API field). The long `authuser=…&hl=…&gl=…&psi=…` sequence are standard query flags (account, language, country) plus a client/session token; the `pb` value itself is a compact, protobuf‑style blob that encodes many UI and pagination details.

### What `i759` / `i760` likely means
- **Not a semantic place ID**: `i759` and `i760` are part of Google’s *internal* request encoding and are not documented public parameters. They typically act as **small incremental tokens or indices** inside the `pb`/encoded payload that help the Maps UI track which page/view or image index is being requested.  
- **Why they differ by 1**: when you change the query (for example from `Tw` to `Two+PNC+Plaza`) the UI generates a slightly different `pb` payload; one of the internal counters or indices increments, producing `i759` → `i760`. This is an implementation detail of Google’s client, not a stable API behavior.

### What the `pb` parameter is
- **`pb` is a compact, encoded payload** used by Google Maps web endpoints. It’s effectively a serialized, Closure‑compiled/protobuf‑style blob that encodes view state, pagination, zoom, result windows, and other UI parameters. People who reverse‑engineer Maps call it a protobuf‑like parameter and decode it with custom tools; it’s not meant for public consumption.

### Meaning of `authuser`, `hl`, `gl`, `psi`
- **`authuser=0`** — selects which signed‑in Google account to use (account index 0 is the primary account in the browser session).  
- **`hl=en`** — sets the **language/locale** for the response (here English).  
- **`gl=us`** — sets the **geographic/country context** (here United States), which can affect ranking and localized results.  
- **`psi=...`** — a client/instance token (a short client/session identifier generated by the browser/JS) used by Google to correlate requests and client state; it’s not a documented API parameter and is part of the UI telemetry/coordination mechanism.

### Practical implications
- **Don’t rely on these parameters** for scraping or production logic — they are fragile and can change any time. Use the official **Maps URLs** or the **Places / Maps APIs** for stable programmatic access instead.  
- If you must parse `pb`, use existing decoders (community projects exist) and treat results as brittle; decoding requires reverse‑engineering and may break when Google changes the client encoding.

### Sources
- `pb` decoding and embedded Maps parameter discussion (community/StackOverflow).  
- Community protobuf decoders and tools for Google Maps `pb` blobs.  
- Official Maps URLs docs explaining `hl`, `gl`, and general URL usage (recommended stable approach).  
- Reverse‑engineering writeups on Maps pagination and client tokens (how `pb`/psi relate to pagination).



Notes on seeing how a particular element gets populated:
**Exploring network requests in Chrome DevTools**

I need to mention using "Copy as cURL" to replay requests. For Google Maps, many requests are encoded in the pb param, and some content is generated client-side from these payloads. I’ll also suggest using the "Pretty print" and "Response" tabs to inspect JSON. To map an element to a network request, I can right-click the element and use "Reveal in Elements panel" or "Inspect." I’ll also explain how to use breakpoints and the "Initiator" column to track requests. I’ll make sure to include step-by-step instructions for Chrome DevTools.
**Yes. Use DevTools to pause DOM changes and inspect the network requests that ran just before the element was created — set a DOM breakpoint on that element, then check the Network panel and the Call Stack to find the exact XHR/fetch that produced its data.**  

### Quick steps to capture the request that created a DOM node
1. **Open DevTools** → Elements panel and locate the `<div class="m6QErb XiKgde">` node.  
2. **Right‑click the node** → Break on → **Subtree modifications**. This causes the debugger to pause when scripts add/remove children under that node.  
3. **Trigger the UI action** that populates the element (type, click, or reload). When the DOM changes, DevTools will pause in the Sources panel and show the **call stack** that performed the modification.  
4. **Inspect the call stack** to find the JS function that appended children. Expand frames to see the script and line; that frame often shows the code that handled the network response.  
5. **Switch to the Network panel** (keep DevTools open and enable Preserve log). Look for XHR/fetch requests that completed just before the paused time; use the **Initiator** column to correlate which script started each request and the timing.  
6. **Open the candidate request** → Headers and Response to inspect the payload that produced the element. Use Copy as cURL or Copy as fetch to replay the request if needed.

### Alternative and complementary techniques
- **XHR/fetch breakpoints** in Sources → Breakpoints → XHR/fetch. This pauses when any XHR or fetch resolves, letting you inspect the response and call stack immediately.  
- **Event Listener Breakpoints** → DOM Mutation → subtree modifications to pause on broader mutation events if you can’t find the exact node.  
- **Filter Network by XHR** and sort by Time or use the Waterfall to find requests that finished right before the DOM change; expand a request to see its initiator chain for exact origin.  
- **Use the Console**: after the element exists, run `$0` (selected element) to inspect it, then check `getEventListeners($0)` or walk up to find attached handlers that may reference the request logic.

### Practical tips to make this reliable
- **Enable Preserve log** in Network so requests aren’t cleared on navigation.  
- **Disable cache** while debugging to avoid cached responses.  
- **Narrow the Network list** by filtering domain or resource type (XHR/fetch).  
- If the page uses websockets or streaming, check the **WS** entries or the streaming response in the request’s response tab.  
- If the site obfuscates requests (protobuf/`pb` blobs), inspect the raw response and the JS code in the paused frame to see how it decodes the payload.

### References
- Chrome DevTools Network panel documentation for inspecting requests and responses.  
- How to set DOM breakpoints in Chrome DevTools to pause on subtree modifications.  
- Understanding request initiator chains to correlate network requests with the code that started them.




Query on directions

https://www.google.com/maps/preview/directions?authuser=0&hl=en&gl=us&pb=!1m7!1s434+Shady+Avenue%2C+Pittsburgh%2C+PA!2s0x8834f20bad463bcb%3A0x4104e286b57ee3d5!3m2!3d40.4546065!4d-79.92213079999999!6e0!19sChIJyztGrQvyNIgR1eN-tYbiBEE!1m5!1s400+East+Waterfront+Drive%2C+Homestead%2C+PA+15120!2s0x8834ee236ec7350f%3A0x73fa84093902b486!3m2!3d40.4120663!4d-79.90993689999999!3m15!1m3!1d3652.469221595039!2d-79.92302947339881!3d40.45715232143318!2m3!1f0!2f0!3f0!3m2!1i413!2i924!4f13.1!6m2!1f0!2f0!6m48!1m5!18b1!30b1!31m1!1b1!34e1!2m4!5m1!6e2!20e3!39b1!6m18!49b1!66b1!74i150000!85b1!91b1!114b1!149b1!178b1!206b1!212b1!213b1!223b1!227b1!232b1!233b1!244b1!246b1!250b1!10b1!12b1!13b1!14b1!16b1!17m2!3e1!3e1!20m5!1e3!2e3!5e2!6b1!14b1!46m1!1b0!96b1!99b1!15m4!1s4comaYyYFdie5NoP6PLzmAQ!4m1!2i10147!7e81!20m0!27b1!28m0!40i760!47m2!8b1!10e2!50sAMAbHIJ9Z-8tJDm9cAYXtpsZjRf8BsO2uA%3A1764149883184

This is the normal URL to analyze in comparison to find the nested patterns:
https://www.google.com/maps/dir/434+Shady+Avenue,+Pittsburgh,+PA/400+East+Waterfront+Drive,+Homestead,+PA+15120/@40.4318528,-79.9255915,12z/data=!4m14!4m13!1m5!1m1!1s0x8834f20bad463bcb:0x4104e286b57ee3d5!2m2!1d-79.9221308!2d40.4546065!1m5!1m1!1s0x8834ee236ec7350f:0x73fa84093902b486!2m2!1d-79.9099369!2d40.4120663!3e3?entry=ttu&g_ep=EgoyMDI1MTEyMy4xIKXMDSoASAFQAw%3D%3D



*/


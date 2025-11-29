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
const bool FAKE_WIFI = true; //If true, always go to no wifi page for testing

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

struct BoardingInfo {
  String busLabel;
  String walkDistance;
  String walkTime;
  String stationName;
  String nextBusTime;
};


struct keys {
  String ssid = "SSID";
  String pass = "PASS";
};

struct locationInputs {
  String startAddr = "START";
  String endAddr = "END";
  String walkTime = "WALKTIME";
};

locationInputs locals;


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
  }
  https.end(); //End session and free resources
  delete client;
  return result;
}

struct Place {
  String name;
  double lat;
  double lon;
};
Place places[MAX_RESULTS];
int placesCount; 
String searchQuery;


String directionsSearch;


bool isDistanceArray(JsonVariant v) {
  // Expect [number, "0.5 mi", flag]
  if (!v.is<JsonArray>()) return false;
  JsonArray a = v.as<JsonArray>();
  if (a.size() < 2) return false;
  return a[1].is<const char*>() && (String((const char*)a[1]).indexOf("mi") >= 0 || String((const char*)a[1]).indexOf("ft") >= 0 || String((const char*)a[1]).indexOf("km") >= 0);
}

bool isDurationArray(JsonVariant v) {
  // Expect [seconds, "11 min", maybe]
  if (!v.is<JsonArray>()) return false;
  JsonArray a = v.as<JsonArray>();
  if (a.size() < 2) return false;
  return a[1].is<const char*>() && (String((const char*)a[1]).indexOf("min") >= 0 || String((const char*)a[1]).indexOf("sec") >= 0);
}

bool stepContainsType5(JsonVariant step, JsonVariant &type5Element) {
  // step is expected to be an array of elements; find element whose first value == 5
  if (!step.is<JsonArray>()) return false;
  for (JsonVariant elem : step.as<JsonArray>()) {
    if (elem.is<JsonArray>() && elem.size() >= 1) {
      JsonVariant first = elem[0];
      if (first.is<int>() && first.as<int>() == 5) {
        type5Element = elem;
        return true;
      }
    }
  }
  return false;
}

String extractLabelFromType5(JsonVariant type5Elem) {
  // type5Elem is like [5, ["61C", 1, "#fff", "#000"]]
  if (!type5Elem.is<JsonArray>()) return String();
  if (type5Elem.size() < 2) return String();
  JsonVariant payload = type5Elem[1];
  if (!payload.is<JsonArray>()) return String();
  if (payload.size() < 1) return String();
  if (payload[0].is<const char*>()) return String((const char*)payload[0]);
  return String();
}

bool findFirstStopInTransitStep(JsonVariant transitStep, JsonVariant &stopOut) {
  // Transit step often contains a nested stops array; search for an array element that looks like a stop:
  // stop looks like ["Stop Name", "stopId", [ts, tz, "5:08 AM", offset, aux], ...]
  if (!transitStep.is<JsonArray>()) return false;
  for (JsonVariant elem : transitStep.as<JsonArray>()) {
    if (elem.is<JsonArray>()) {
      // If the element's first child is a string and second is string and third is array -> candidate stop
      JsonArray candidate = elem.as<JsonArray>();
      if (candidate.size() >= 3 && candidate[0].is<const char*>() && candidate[1].is<const char*>() && candidate[2].is<JsonArray>()) {
        stopOut = candidate;
        return true;
      }
      // Sometimes stops are nested deeper; check children
      for (JsonVariant child : candidate) {
        if (child.is<JsonArray>()) {
          JsonArray c2 = child.as<JsonArray>();
          if (c2.size() >= 3 && c2[0].is<const char*>() && c2[1].is<const char*>() && c2[2].is<JsonArray>()) {
            stopOut = c2;
            return true;
          }
        }
      }
    }
  }
  return false;
}

String extractHumanTimeFromDepartureArray(JsonVariant departureArr) {
  // departureArr expected like [unix_ts, "America/New_York", "5:08 AM", offset, aux]
  if (!departureArr.is<JsonArray>()) return String();
  JsonArray a = departureArr.as<JsonArray>();
  if (a.size() >= 3 && a[2].is<const char*>()) {
    return String((const char*)a[2]);
  }
  // fallback: if unix ts present, format roughly (we avoid heavy time libs on Arduino)
  if (a.size() >= 1 && a[0].is<long>()) {
    long ts = a[0].as<long>();
    // crude fallback: return epoch as string
    return String(ts);
  }
  return String();
}

// Try to extract the five fields from a route object (routeVariant)
bool extractBoardingFromRoute(JsonVariant routeVariant, BoardingInfo &out) {
  // Look for legs -> steps structure first
  if (routeVariant.is<JsonObject>() && routeVariant.containsKey("legs")) {
    JsonArray legs = routeVariant["legs"].as<JsonArray>();
    for (JsonVariant leg : legs) {
      if (!leg.is<JsonObject>() && !leg.is<JsonArray>()) continue;
      // steps may be under "steps"
      if (leg.is<JsonObject>() && leg.containsKey("steps")) {
        JsonArray steps = leg["steps"].as<JsonArray>();
        for (size_t i = 0; i < steps.size(); ++i) {
          JsonVariant step = steps[i];
          JsonVariant type5Elem;
          if (stepContainsType5(step, type5Elem)) {
            // Found transit step
            out.busLabel = extractLabelFromType5(type5Elem);
            // find first stop in this transit step
            JsonVariant firstStop;
            if (findFirstStopInTransitStep(step, firstStop)) {
              if (firstStop[0].is<const char*>()) out.stationName = String((const char*)firstStop[0]);
              if (firstStop[2].is<JsonArray>()) out.nextBusTime = extractHumanTimeFromDepartureArray(firstStop[2]);
            }
            // find walk step immediately before
            if (i > 0) {
              JsonVariant walkStep = steps[i - 1];
              // scan elements in walkStep for distance and duration arrays
              for (JsonVariant elem : walkStep.as<JsonArray>()) {
                if (isDistanceArray(elem)) {
                  out.walkDistance = String((const char*)elem[1].as<const char*>());
                } else if (isDurationArray(elem)) {
                  out.walkTime = String((const char*)elem[1].as<const char*>());
                }
              }
            } else {
              // no previous step in this leg; try previous leg's last step
              // (not implemented here for brevity; could be added)
            }
            return true; // we return the first boarding found for this route
          }
        }
      }
    }
  }

  // Fallback: loose scan inside routeVariant for a type-5 element and pair with nearest preceding distance/duration in the same parent array
  // This is less precise but helps when structure differs.
  // We'll recursively search arrays and when we find a type-5 element, we look backward in that array for distance/duration.
  std::function<bool(JsonVariant&)> recursiveScan;
  recursiveScan = [&](JsonVariant &node) -> bool {
    if (node.is<JsonArray>()) {
      JsonArray arr = node.as<JsonArray>();
      for (size_t idx = 0; idx < arr.size(); ++idx) {
        JsonVariant elem = arr[idx];
        JsonVariant type5Elem;
        if (stepContainsType5(elem, type5Elem)) {
          out.busLabel = extractLabelFromType5(type5Elem);
          // try to find first stop inside elem
          JsonVariant firstStop;
          if (findFirstStopInTransitStep(elem, firstStop)) {
            if (firstStop[0].is<const char*>()) out.stationName = String((const char*)firstStop[0]);
            if (firstStop[2].is<JsonArray>()) out.nextBusTime = extractHumanTimeFromDepartureArray(firstStop[2]);
          }
          // search backward in same array for distance/duration
          for (int j = int(idx) - 1; j >= 0; --j) {
            JsonVariant cand = arr[j];
            if (isDistanceArray(cand) && out.walkDistance.length() == 0) {
              out.walkDistance = String((const char*)cand[1].as<const char*>());
            }
            if (isDurationArray(cand) && out.walkTime.length() == 0) {
              out.walkTime = String((const char*)cand[1].as<const char*>());
            }
            if (out.walkDistance.length() && out.walkTime.length()) break;
          }
          return true;
        }
        // recurse
        if (elem.is<JsonArray>() || elem.is<JsonObject>()) {
          if (recursiveScan(elem)) return true;
        }
      }
    } else if (node.is<JsonObject>()) {
      for (JsonPair kv : node.as<JsonObject>()) {
        JsonVariant v = kv.value();
        if (v.is<JsonArray>() || v.is<JsonObject>()) {
          if (recursiveScan(v)) return true;
        }
      }
    }
    return false;
  };

  JsonVariant rv = routeVariant;
  if (recursiveScan(rv)) return true;

  return false;
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

String extractJsonPartDirections(const String &raw) {
  int len = raw.length();
  int i = 0;
  while (i < len) {
    char c = raw.charAt(i);
    if (c == '{' || c == '[') break;
    ++i;
  }
  if (i >= len) return String();
  dbgSerial->println("extractJsonPartDirections: found JSON start at index " + String(i));
  return raw.substring(i, raw.length());
}

// Returns the index of the matching closing '}' or ']' for the JSON that starts at `start`.
// Returns -1 if no matching end is found (incomplete JSON).
int findJsonEnd(const String &s, int start) {
  if (start < 0 || start >= s.length()) return -1;
  char opener = s.charAt(start);
  if (opener != '{' && opener != '[') return -1;

  bool inString = false;
  bool escape = false;
  int depth = 0;

  for (int i = start; i < s.length(); ++i) {
    char c = s.charAt(i);

    if (inString) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }

    if (c == '"') {
      inString = true;
      continue;
    }

    if (c == '{' || c == '[') {
      ++depth;
      continue;
    }

    if (c == '}' || c == ']') {
      --depth;
      if (depth == 0) {
        return i; // found the matching end
      }
    }
  }

  // Reached end of string without closing all openers
  return -1;
}

String extractJsonPartSafe(const String &raw) {
  // find first opener
  int start = 0;
  while (start < raw.length() && raw.charAt(start) != '{' && raw.charAt(start) != '[') ++start;
  if (start >= raw.length()) return String();

  int endIndex = findJsonEnd(raw, start);
  if (endIndex < 0) return String(); // incomplete JSON

  // Prefer parsing in-place: return pointer/length to deserializeJson
  // If you must return a String, copy once using malloc+memcpy (safer than many substrings)
  int totalLen = endIndex - start + 1;
  char *buf = (char*)malloc((size_t)totalLen + 1);
  if (!buf) return String(); // allocation failed
  memcpy(buf, raw.c_str() + start, (size_t)totalLen);
  buf[totalLen] = '\0';
  String out = String(buf);
  free(buf);
  return out;
}

String extractJsonPartChunked(const String &raw) {
  int len = raw.length();
  int start = 0;
  while (start < len && raw.charAt(start) != '{' && raw.charAt(start) != '[') ++start;
  if (start >= len) return String();

  // Reserve a reasonable amount to reduce fragmentation
  String out;
  out.reserve(min(65536, len - start)); // tune as needed

  // Append in chunks
  const int CHUNK = 4096;
  for (int pos = start; pos < len; pos += CHUNK) {
    int end = min(len, pos + CHUNK);
    out += raw.substring(pos, end);   // smaller allocations
  }
  return out;
}


// ptr: pointer to the JSON start (body.c_str() + start)
// len: length of the JSON slice (endIndex - start + 1)
// dbgSerial: your debug Serial instance (e.g., &Serial)

void debugPtrSlice(Stream &dbgSerial, const char *ptr, size_t len) {
  if (!ptr || len == 0) {
    dbgSerial.println("ptr is null or len == 0");
    return;
  }

  // 1) Basic info
  dbgSerial.print("ptr slice length = ");
  dbgSerial.println((unsigned long)len);

  // 2) First and last byte values (print as ints)
  dbgSerial.print("first byte (char) = ");
  dbgSerial.print((int)ptr[0]);
  dbgSerial.print("  first char = ");
  dbgSerial.println(ptr[0]); // may be non-printable

  dbgSerial.print("last byte (char) = ");
  dbgSerial.print((int)ptr[len - 1]);
  dbgSerial.print("  last char = ");
  dbgSerial.println(ptr[len - 1]); // may be non-printable

  // 3) Print a safe HEAD (first N bytes) using write to avoid building Strings
  const size_t HEAD_N = min((size_t)200, len);
  dbgSerial.println("---- HEAD ----");
  dbgSerial.write(ptr, HEAD_N);   // raw write of bytes
  dbgSerial.println();           // newline after raw bytes

  // 4) Print a safe TAIL (last N bytes)
  const size_t TAIL_N = min((size_t)200, len);
  dbgSerial.println("---- TAIL ----");
  dbgSerial.write(ptr + (len - TAIL_N), TAIL_N);
  dbgSerial.println();

  // 5) Byte dump for the first M bytes (numeric values)
  dbgSerial.println("---- BYTE DUMP (first 80 bytes) ----");
  size_t dumpN = min((size_t)80, len);
  for (size_t i = 0; i < dumpN; ++i) {
    dbgSerial.print(i);
    dbgSerial.print(":");
    dbgSerial.print((int)(unsigned char)ptr[i]);
    dbgSerial.print(" ");
  }
  dbgSerial.println();

  // 6) Quick sanity checks for JSON start/end characters
  dbgSerial.print("starts with: ");
  dbgSerial.println(ptr[0] == '[' ? "[" : (ptr[0] == '{' ? "{" : "other"));
  dbgSerial.print("ends with: ");
  dbgSerial.println(ptr[len - 1] == ']' ? "]" : (ptr[len - 1] == '}' ? "}" : "other"));

  // 7) Optional: check for embedded NULs in the slice (rare but problematic)
  bool foundNull = false;
  for (size_t i = 0; i < len; ++i) {
    if (ptr[i] == '\0') { foundNull = true; dbgSerial.print("NUL at index "); dbgSerial.println((unsigned long)i); break; }
  }
  if (!foundNull) dbgSerial.println("no embedded NUL found in slice");

  // 8) If you want to print the whole slice in safe chunks (avoid one huge write)
  dbgSerial.println("---- FULL SLICE (chunked) ----");
  const size_t CHUNK = 1024;
  size_t pos = 0;
  while (pos < len) {
    size_t chunk = min(CHUNK, len - pos);
    dbgSerial.write(ptr + pos, chunk);
    pos += chunk;
    // optional small delay if serial buffer is small:
    // delay(5);
  }
  dbgSerial.println();
}




// Public API: parse JSON body and fill an array of BoardingInfo
int parseBoardingInfos(const String &body, BoardingInfo results[], int maxResults) {
  // String jsonPart = extractJsonPartSafe(body); //extractJsonPartChunked(body); //extractJsonPartDirections(body);
  // int start = 11; /* index of first '{' or '[' */
  // int endIndex = findJsonEnd(body, start);
  int startObj = body.indexOf('{');
  int startArr = body.indexOf('[');
  int start = -1;
  if (startObj >= 0 && (startObj < startArr || startArr == -1)) start = startObj;
  else start = startArr;

  int endObj = body.lastIndexOf('}');
  int endArr = body.lastIndexOf(']');
  int endIndex = max(endObj, endArr);
  // if (endIndex >= 0 == false) {
  //   return -1;
  // }

  const char *ptr = body.c_str() + start;
  size_t len = (size_t)(endIndex - start + 1);
  // DynamicJsonDocument doc(capacity);
  
  // handle err...

  // dbgSerial->println("jsonPart " + String(jsonPart.length()) + " chars");
  // if (jsonPart.length() == 0) {
  //   dbgSerial->println("No JSON found in body");
  //   return 0;
  // }

  // dbgSerial->print("first char: "); dbgSerial->println((int)body.charAt(0));
  // dbgSerial->print("last char: ");  dbgSerial->println((int)body.charAt(body.length()-1));

  // // // 2) Print small head and tail (safe)
  // dbgSerial->println("HEAD: " + body.substring(0, min(200, int(body.length()))));
  // dbgSerial->println("TAIL: " + body.substring(max(0, int(body.length()-200)), body.length()));

  // debugPtrSlice(*dbgSerial, ptr, len);
  
  // Adjust capacity to your payload size
  const size_t capacity = 200000; // increase if needed
  DynamicJsonDocument doc(capacity);
  // JsonDocument doc;
  // DeserializationError err = deserializeJson(doc, jsonPart);
  dbgSerial->print("ptr non-null? "); dbgSerial->println(ptr != nullptr);
  dbgSerial->print("len = "); dbgSerial->println((unsigned long)len);
  DeserializationError err = deserializeJson(doc, ptr, len, DeserializationOption::NestingLimit(200));

  if (err) {
    dbgSerial->print("JSON parse failed: ");
    dbgSerial->println(err.c_str());
    return 0;
  }

  JsonVariant root = doc.as<JsonVariant>();
  int found = 0;

  // Try canonical path: root["routes"]
  if (root.is<JsonObject>() && root.containsKey("routes")) {
    JsonArray routes = root["routes"].as<JsonArray>();
    for (JsonVariant route : routes) {
      if (found >= maxResults) break;
      BoardingInfo info;
      if (extractBoardingFromRoute(route, info)) {
        results[found++] = info;
      }
    }
    return found;
  }

  // If no "routes" key, try scanning top-level array for route-like objects
  if (root.is<JsonArray>()) {
    for (JsonVariant candidate : root.as<JsonArray>()) {
      if (found >= maxResults) break;
      BoardingInfo info;
      if (extractBoardingFromRoute(candidate, info)) {
        results[found++] = info;
      }
    }
    return found;
  }

  // As a last resort, try the whole document as a single route
  BoardingInfo info;
  if (extractBoardingFromRoute(root, info)) {
    if (found < maxResults) results[found++] = info;
  }
  return found;
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





int getPlaces(String searchQuery, Place places[], int maxPlaces) {
  searchQuery.replace(" ", "+");
  searchQuery.replace(",", "%2C");
  String url = "https://www.google.com/s?tbm=map&gs_ri=maps&suggest=p&authuser=0&hl=en&gl=us&psi=Avghab7tBdbV5NoP9PqxgQ0.1763833866758.1&q=" + searchQuery + "&ech=3&pb=!2i13!4m12!1m3!1d14611.795576010498!2d-79.93046255!3d40.44832804999999!2m3!1f0!2f0!3f0!3m2!1i815!2i924!4f13.1!7i20!10b1!12m25!1m5!18b1!30b1!31m1!1b1!34e1!2m4!5m1!6e2!20e3!39b1!10b1!12b1!13b1!16b1!17m1!3e1!20m3!5e2!6b1!14b1!46m1!1b0!96b1!99b1!19m4!2m3!1i360!2i120!4i8!20m57!2m2!1i203!2i100!3m2!2i4!5b1!6m6!1m2!1i86!2i86!1m2!1i408!2i240!7m33!1m3!1e1!2b0!3e3!1m3!1e2!2b1!3e2!1m3!1e2!2b0!3e3!1m3!1e8!2b0!3e3!1m3!1e10!2b0!3e3!1m3!1e10!2b1!3e2!1m3!1e10!2b0!3e4!1m3!1e9!2b1!3e2!2b1!9b0!15m8!1m7!1m2!1m1!1e2!2m2!1i195!2i195!3i20!22m3!1sURMlaa7DOpPe5NoP99fnkQY!7e81!17sURMlaa7DOpPe5NoP99fnkQY%3A63!23m2!4b1!10b1!24m109!1m30!13m9!2b1!3b1!4b1!6i1!8b1!9b1!14b1!20b1!25b1!18m19!3b1!4b1!5b1!6b1!9b1!13b1!14b1!17b1!20b1!21b1!22b1!27m1!1b0!28b0!32b1!33m1!1b1!34b1!36e2!10m1!8e3!11m1!3e1!14m1!3b0!17b1!20m2!1e3!1e6!24b1!25b1!26b1!27b1!29b1!30m1!2b1!36b1!37b1!39m3!2m2!2i1!3i1!43b1!52b1!54m1!1b1!55b1!56m1!1b1!61m2!1m1!1e1!65m5!3m4!1m3!1m2!1i224!2i298!72m22!1m8!2b1!5b1!7b1!12m4!1b1!2b1!4m1!1e1!4b1!8m10!1m6!4m1!1e1!4m1!1e3!4m1!1e4!3sother_user_google_review_posts__and__hotel_and_vr_partner_review_posts!6m1!1e1!9b1!89b1!98m3!1b1!2b1!3b1!103b1!113b1!114m3!1b1!2m1!1b1!117b1!122m1!1b1!126b1!127b1!26m4!2m3!1i80!2i92!4i8!34m19!2b1!3b1!4b1!6b1!8m6!1b1!3b1!4b1!5b1!6b1!7b1!9b1!12b1!14b1!20b1!23b1!25b1!26b1!31b1!37m1!1e81!47m0!49m10!3b1!6m2!1b1!2b1!7m2!1e3!2b1!8b1!9b1!10e2!61b1!67m5!7b1!10b1!14b1!15m1!1b0!69i760";
  String body = httpGetStream(url);
  logMessage("Response length: " + String(body.length()));
  // dbgSerial->println(body); // or parse it
  
  
  int count = parsePlacesFromBody(body, places, MAX_RESULTS);

  dbgSerial->printf("Found %d places:\n", count);
  for (int i = 0; i < count; ++i) {
    dbgSerial->printf("%d) %s -> %f, %f\n", i+1, places[i].name.c_str(), places[i].lat, places[i].lon);
  }
  return count;
}

BoardingInfo infos[MAX_RESULTS];

void getDirections(String start, String end) {
  start.replace(" ", "+");
  start.replace(",", "%2C");
  end.replace(" ", "+");
  end.replace(",", "%2C");
  String url = "https://www.google.com/maps/preview/directions?authuser=0&hl=en&gl=us&pb=!1m7!1s" + start + "!2s0x8834f20bad463bcb%3A0x4104e286b57ee3d5!3m2!3d40.4546065!4d-79.92213079999999!6e0!19sChIJyztGrQvyNIgR1eN-tYbiBEE!1m5!1s" + end + "!2s0x8834ee236ec7350f%3A0x73fa84093902b486!3m2!3d40.4120663!4d-79.90993689999999!3m15!1m3!1d3652.469221595039!2d-79.92302947339881!3d40.45715232143318!2m3!1f0!2f0!3f0!3m2!1i413!2i924!4f13.1!6m2!1f0!2f0!6m48!1m5!18b1!30b1!31m1!1b1!34e1!2m4!5m1!6e2!20e3!39b1!6m18!49b1!66b1!74i150000!85b1!91b1!114b1!149b1!178b1!206b1!212b1!213b1!223b1!227b1!232b1!233b1!244b1!246b1!250b1!10b1!12b1!13b1!14b1!16b1!17m2!3e1!3e1!20m5!1e3!2e3!5e2!6b1!14b1!46m1!1b0!96b1!99b1!15m4!1s4comaYyYFdie5NoP6PLzmAQ!4m1!2i10147!7e81!20m0!27b1!28m0!40i760!47m2!8b1!10e2!50sAMAbHIJ9Z-8tJDm9cAYXtpsZjRf8BsO2uA%3A1764149883184";
  dbgSerial->println("DirectionsURL: " + url);
  // return 0;
  String body = httpGetStream(url);
  logMessage("Directions response length: " + String(body.length()));
  // dbgSerial->println("Directions response: " + body.substring(0, min(200, int(body.length())))); // print first 200 chars

  int n = parseBoardingInfos(body, infos, MAX_RESULTS);

  dbgSerial->print("Found ");
  dbgSerial->print(n);
  dbgSerial->println(" boarding entries:");
  for (int i = 0; i < n; ++i) {
    dbgSerial->println("---");
    dbgSerial->print("Bus: "); dbgSerial->println(infos[i].busLabel);
    dbgSerial->print("Walk distance: "); dbgSerial->println(infos[i].walkDistance);
    dbgSerial->print("Walk time: "); dbgSerial->println(infos[i].walkTime);
    dbgSerial->print("Station: "); dbgSerial->println(infos[i].stationName);
    dbgSerial->print("Next bus: "); dbgSerial->println(infos[i].nextBusTime);
  }


  // dbgSerial->println(body); // or parse it
  
  
  // int count = parsePlacesFromBody(body, places, MAX_RESULTS);

  // dbgSerial->printf("Found %d places:\n", count);
  // for (int i = 0; i < count; ++i) {
  //   dbgSerial->printf("%d) %s -> %f, %f\n", i+1, places[i].name.c_str(), places[i].lat, places[i].lon);
  // }
  // return count;
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
  
  delay(100);   //Would remove prior connections, it stores it by default, could check to see if it's connected off the bat
  WifiCredentials creds;
  if (FAKE_WIFI) { // This is for testing really
    saveSetting(CONST_KEYS.ssid.c_str(), ssidTest);
    saveSetting(CONST_KEYS.pass.c_str(), passwordTest); 
  }

  creds.ssid = loadStringSetting(CONST_KEYS.ssid.c_str());
  creds.pass = loadStringSetting(CONST_KEYS.pass.c_str());
  
  // Wifi may not have a password
  
  if (creds.ssid.length() == 0) { //|| !FAKE_WIFI
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

    // SEARCH QUERY SEQUENCE
    // String url = "https://www.google.com/s?tbm=map&gs_ri=maps&suggest=p&authuser=0&hl=en&gl=us&psi=Avghab7tBdbV5NoP9PqxgQ0.1763833866758.1&q=Tw&ech=7&pb=!2i2!4m12!1m3!1d14611.795576010498!2d-79.93046255!3d40.44832804999999!2m3!1f0!2f0!3f0!3m2!1i1298!2i924!4f13.1!7i20!10b1!12m25!1m5!18b1!30b1!31m1!1b1!34e1!2m4!5m1!6e2!20e3!39b1!10b1!12b1!13b1!16b1!17m1!3e1!20m3!5e2!6b1!14b1!46m1!1b0!96b1!99b1!19m4!2m3!1i360!2i120!4i8!20m57!2m2!1i203!2i100!3m2!2i4!5b1!6m6!1m2!1i86!2i86!1m2!1i408!2i240!7m33!1m3!1e1!2b0!3e3!1m3!1e2!2b1!3e2!1m3!1e2!2b0!3e3!1m3!1e8!2b0!3e3!1m3!1e10!2b0!3e3!1m3!1e10!2b1!3e2!1m3!1e10!2b0!3e4!1m3!1e9!2b1!3e2!2b1!9b0!15m8!1m7!1m2!1m1!1e2!2m2!1i195!2i195!3i20!22m3!1sAvghab7tBdbV5NoP9PqxgQ0!7e81!17sAvghab7tBdbV5NoP9PqxgQ0%3A83!23m2!4b1!10b1!24m109!1m30!13m9!2b1!3b1!4b1!6i1!8b1!9b1!14b1!20b1!25b1!18m19!3b1!4b1!5b1!6b1!9b1!13b1!14b1!17b1!20b1!21b1!22b1!27m1!1b0!28b0!32b1!33m1!1b1!34b1!36e2!10m1!8e3!11m1!3e1!14m1!3b0!17b1!20m2!1e3!1e6!24b1!25b1!26b1!27b1!29b1!30m1!2b1!36b1!37b1!39m3!2m2!2i1!3i1!43b1!52b1!54m1!1b1!55b1!56m1!1b1!61m2!1m1!1e1!65m5!3m4!1m3!1m2!1i224!2i298!72m22!1m8!2b1!5b1!7b1!12m4!1b1!2b1!4m1!1e1!4b1!8m10!1m6!4m1!1e1!4m1!1e3!4m1!1e4!3sother_user_google_review_posts__and__hotel_and_vr_partner_review_posts!6m1!1e1!9b1!89b1!98m3!1b1!2b1!3b1!103b1!113b1!114m3!1b1!2m1!1b1!117b1!122m1!1b1!126b1!127b1!26m4!2m3!1i80!2i92!4i8!34m19!2b1!3b1!4b1!6b1!8m6!1b1!3b1!4b1!5b1!6b1!7b1!9b1!12b1!14b1!20b1!23b1!25b1!26b1!31b1!37m1!1e81!47m0!49m10!3b1!6m2!1b1!2b1!7m2!1e3!2b1!8b1!9b1!10e2!61b1!67m5!7b1!10b1!14b1!15m1!1b0!69i759"; // example (fragile)
    // searchQuery = "434";
    // placesCount = getPlaces(searchQuery, places, MAX_RESULTS);


    // DIRECTIONS SEQUENCE
    locals.startAddr = "434 Shady Ave, Pittsburgh, PA 15206";
    locals.endAddr = "400 E Waterfront Dr, Homestead, PA 15120";
    getDirections(locals.startAddr, locals.endAddr);


    
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
  }
  
  // if (!FAKE_WIFI) {
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


Array formatter
https://alignhash.codeutility.io/

*/


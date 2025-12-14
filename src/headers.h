#pragma once
#include <Arduino.h>   // include only what prototypes need

// Utility parsing / conversion
bool extractNumber(const String &s, float &outVal);
float distanceStringToMiles(const String &raw);
float minutesStringToFloat(const String &raw);
String trimStr(const String &s);
bool extractFirstDigits(const String &s, String &outDigits);
bool parseTimeString(const String &in, int &outHour, int &outMin);
bool parseUnsignedLongLong(const String &s, unsigned long long &out);
String secondToken(const String &s);

// Settings / persistence / logging / HTTP
void saveSetting(const char* settingName, const char* settingValue);
String loadStringSetting(const char* key, const char* defaultVal = "");
void logMessage(const String& msg);
String httpGetStream(const String &url);
String httpGetDirections(String origin_lat, String origin_lon, String dest_lat, String dest_lon, String apiKey = "");
StaticJsonDocument<512> returnUserData();

// Time / timezone / location
currentLocation getCurrentLocation();
String getUserTimezone();
String getTimeNowUTC(bool verbose=false);
const char* iana_to_posix(const char* iana);
bool minute_changed_now(void);
int minutesDifferenceFromEpochMs(const String &epochMsStr, const String &nextBusTimeStr);

// Nextion / serial UI helpers
void sendCommand(const String &cmd);
std::vector<uint8_t> readNextionPacket(Stream &s, unsigned long timeoutMs = 3000);
int waitForButtonPress(Stream &nx, unsigned long timeoutMs = 10000);
String getButtonText(Stream &nx, unsigned long timeoutMs = 10000);
void debugHex(const String &s);
void handleNextionPacket(uint8_t *p, int len);
void sendComponentTxt(int btnCount, int txtTruncateLength, const std::vector<String>& txtList,
                      String componentType = "b", bool loading = false, int startOffset = 0);
String joinWithNewline(const std::vector<String>& v);
void safeSetPage(String page);

// WiFi helpers
bool tryWifi(const char* ssid, const char* pass, unsigned long timeout_ms = 20000);
std::vector<String> connectionSequence(bool verbose=false);

// Address / place search / directions parsing
String extractJsonPart(const String &raw);
size_t parseAllFirstBoardingInfos(const String &jsonPart, std::vector<BoardingInfo> &outInfos, size_t capacity = 200000);
std::vector<BoardingInfo> parseBoardingInfos(const String &body, BoardingInfo results[], int maxResults);
std::vector<BoardingInfo> extractBoardingInfosManual(const String &body, Stream *dbgSerial, size_t MAX_RESULTS);
int parsePlacesFromBody(const String &body, Place places[], int maxPlaces);
std::vector<placeIdentifier> getPlaces(String searchQuery, Place places[], int maxPlaces, double newLat, double newLong, bool verbose=false);
std::vector<BoardingInfo> getDirections(String start, String end, double newLat, double newLong, bool verbose=false);
String setPbCenter(String url, double newLat, double newLon);

// Manual HTML/JSON body scanning helpers (station/route extraction)
int tryStationBlock(const String &body, int i, std::vector<Station> &stations);
static void scanBodyManual(const String &body,
                           std::vector<Match> &routes,
                           std::vector<Match> &distances,
                           std::vector<Match> &walkTimes,
                           std::vector<Station> &stations,
                           std::vector<Match> &departures,
                           Stream *dbgSerial);
static std::vector<Match> dedupeRoutes(const std::vector<Match> &routes);
static std::vector<BoardingInfo> groupBoardingInfos(const std::vector<Match> &routes_in,
                                                    const std::vector<Match> &distances,
                                                    const std::vector<Match> &walkTimes,
                                                    const std::vector<Station> &stations,
                                                    const std::vector<Match> &departures,
                                                    size_t MAX_RESULTS);

// Place pattern matching helpers
bool isValidLatLon(double a, double b);
bool matchPatternAndStore(JsonArray arr, Place &out);
void searchForPatterns(JsonVariant v, Place places[], int &count, int maxPlaces);

// UI flow / blocking sequences (Nextion-driven)
buttonText SelectWifi();
buttonText WifiLogin();
chooseAddressStruct chooseAddress();
int indexOf(const std::vector<String>& v, const String& target);
float newWalkTime(float oldWalkTime, float oldMiles, float newMiles);
int chooseWalkTime();

// High-level setup / blocks
void block_chooseAddress();
void block_chooseWifi();
WifiCredentials block_wifiLogin();
void block_walkTimeBus();
void block_infoPage();

// Web server handlers
void handleIndex();
void handleLogs();

// Arduino setup/loop (already present but declare if needed)
void setup();
void loop();
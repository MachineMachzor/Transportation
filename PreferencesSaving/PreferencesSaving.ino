#include <Preferences.h>


Preferences prefs;

const char* settingName;

struct keys {
  String ssid = "SSID";
  String pass = "PASS";
  String startAddr = "START";
  String endAddr = "END";
  String walkTime = "WALKTIME";
};


struct WifiCredentials {
  String ssid;
  String pass;
  bool ok = false;
};




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





void setup() 
{
  Serial.begin(115200);
  WifiCredentials creds;
  keys CONST_KEYS;
  // put your setup code here, to run once:
  creds.ssid = loadStringSetting(CONST_KEYS.ssid.c_str());
  if(creds.ssid.length() == 0) {
    Serial.println("Did not load, attempting now");
    saveSetting(CONST_KEYS.ssid.c_str(), "TestSSID");
  } else {
    Serial.printf("Loaded creds ssid %s\n", creds.ssid.c_str());
  }

}

void loop() {
  // put your main code here, to run repeatedly:

}

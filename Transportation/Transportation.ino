#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <driver/ledc.h>
#include <algorithm>  // for std::find
#include <iterator>   // for std::begin/​end
#include <Wire.h>
#include <EEPROM.h>
#include <esp32cam.h>
#include <SPI.h>
#include <vector>
#include <Preferences.h>
#include "camera_index.h"          // pulls in your gzipped HTML


Preferences prefs;

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

void sendCommand(String cmd) {
  Serial1.print(cmd);
  Serial1.write(0xFF); Serial1.write(0xFF); Serial1.write(0xFF);
  delay(10);
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

WifiCredentials connectionSequence() {
  int n = WiFi.scanNetworks();
  WifiCredentials result;

    
  for (int i = 0; i < min(n,5); ++i) {               // send first 5 rows min(n,5)
    String s = WiFi.SSID(i);
    // sendCommand("t" + String(i) + ".txt=\"" + s + "\""); // assumes t0..t4 text fields on Nextion, this is why we may want to limit it to 5 only
    Serial.println(s);
  }
  // Realistically sleep wait until Nextion calls back with a submission attempt for this, and also send creds to Nextion
  // WiFi.begin(ssidTest, passTest);
  saveSetting(CONST_KEYS.ssid.c_str(), ssidTest);
  saveSetting(CONST_KEYS.pass.c_str(), passwordTest);
  bool connected = tryWifi(ssidTest, passwordTest);
  result.ok = connected;
  result.ssid = ssidTest;
  result.pass = passwordTest;
  return result;
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  WiFi.mode(WIFI_STA); //Client/station mode.
  // WiFi.begin(ssid, password);


  // example: RX=16, TX=17
  Serial1.begin(9600, SERIAL_8N1, 16, 17); // ESP32 hardware UART
  // send a Nextion command (must end with 0xFF 0xFF 0xFF)
  // Serial1.print("t0.txt=\"Hello\"\xFF\xFF\xFF");
  bool connected = false;

  
  WiFi.disconnect(); 
  delay(100);   //Would remove prior connections, it stores it by default, could check to see if it's connected off the bat
  WifiCredentials creds;
  creds.ssid = loadStringSetting(CONST_KEYS.ssid.c_str());
  creds.pass = loadStringSetting(CONST_KEYS.pass.c_str());
  // Wifi may not have a password
  if (creds.ssid.length() == 0) {
    Serial.println("No prior SSID");
  } else {
    Serial.println("Prior SSID, try to connect to wifi");
    connected = tryWifi(creds.ssid.c_str(), creds.pass.c_str());
    creds.ok = connected;
    // Serial.printf("Did prior login save allow connection to wifi? creds.ok: %s\n", creds.ok ? "true" : "false");
  }

  if (creds.ok) {
    
  } else {
    Serial.println("No prior wifi");
  }

  // Serial.println(creds);
  
  // WiFi.begin();
  // WiFi.reconnect(); //Try to store credentials?
  
  

  // connected = false;
  if (!connected) {
    Serial.println("Finding WIFI as new");
    creds = connectionSequence();
    connected = creds.ok;
    Serial.printf("Found wifi and logged in, did it work? creds.ok: %s\n", creds.ok ? "true" : "false");

  }
  // Realistically we'll want to wait for a command to select to wifi then try again
  // connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    Serial.println("Connected to Wi-Fi");
  }

  Serial.println("HTTP server running, ready for commands.");
}

void handleNextionPacket(uint8_t *p, int len) {
  if (len <= 0) return;
  uint8_t type = p[0];
  if (type == 0x65 && len >= 4) {           // touch event
    uint8_t page = p[1];
    uint8_t comp = p[2];
    uint8_t event = p[3];                   // 0=press,1=release
    Serial.printf("Touch page=%d comp=%d ev=%d\n", page, comp, event);
  } else if (type == 0x70) {                // string response
    String s = String((char*)&p[1]);
    Serial.println("Nextion string: " + s);
  } else if (type == 0x71 && len >= 5) {    // number response
    long val = (p[1]<<24) | (p[2]<<16) | (p[3]<<8) | p[4];
    Serial.printf("Nextion number: %ld\n", val);
  } else {
    Serial.println("Unknown packet");
  }
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


}

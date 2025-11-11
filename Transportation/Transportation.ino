#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <driver/ledc.h>
// #include "camera_index.h"          // pulls in your gzipped HTML
#include <algorithm>  // for std::find
#include <iterator>   // for std::begin/​end
// #include "Adafruit_MCP23X17.h"
#include <Wire.h>
#include <EEPROM.h>
#include <esp32cam.h>
// #include <SD_MMC.h>
#include <SPI.h>
#include "SPIFFS.h"
#include <vector>
// #include <HTTPClient.h>

char* ssid     = "NETGEAR26";
char* password = "melodicpanda708";

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


void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  WiFi.mode(WIFI_STA); //Client/station mode.
  // WiFi.begin(ssid, password);
  

  // example: RX=16, TX=17
  Serial1.begin(9600, SERIAL_8N1, 16, 17); // ESP32 hardware UART
  // send a Nextion command (must end with 0xFF 0xFF 0xFF)
  // Serial1.print("t0.txt=\"Hello\"\xFF\xFF\xFF");

  
  WiFi.disconnect(); 
  delay(100);   //Would remove prior connections, it stores it by default, could check to see if it's connected off the bat
  
  int n = WiFi.scanNetworks();
  for (int i = 0; i < min(n,5); ++i) {               // send first 5 rows min(n,5)
    String s = WiFi.SSID(i);
    // sendCommand("t" + String(i) + ".txt=\"" + s + "\""); // assumes t0..t4 text fields on Nextion, this is why we may want to limit it to 5 only
    Serial.println(s);
  }
  // while (WiFi.status() != WL_CONNECTED) {
  //   delay(200);
  //   Serial.print(".");
  // }

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

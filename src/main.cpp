#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

#include "config.h"
#include "net.h"
#include "ui.h"

static WiFiManager wm;
static uint32_t lastWifiCheck = 0;
static uint32_t lastReconnect = 0;

static void portalCallback(WiFiManager*) { uiShowWifiPortal(); }

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[espstocks] boot");

  netInit();
  uiBegin();

  pinMode(0, INPUT_PULLUP);
  bool forceCal = (digitalRead(0) == LOW);

  if (forceCal) {
    uiBootSplash("ESP STOCKS", "touch calibration");
    if (!uiCalibrate(60000)) uiUseDefaultCalibration();
  } else if (uiNeedsCalibration()) {
    uiBootSplash("ESP STOCKS", "touch calibration");
    if (!uiCalibrate(15000)) {
      Serial.println("[ui] calibration skipped, using defaults");
      uiUseDefaultCalibration();
    }
  }

  uiShowWifiConnect();
  wm.setDebugOutput(false);
  wm.setConfigPortalTimeout(240);
  wm.setConnectTimeout(25);
  wm.setAPCallback(portalCallback);
  wm.setHostname("esp-stocks");

  if (!wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASS)) {
    Serial.println("[wifi] portal timeout, restarting");
    delay(1500);
    ESP.restart();
  }

  Serial.printf("[wifi] up %s rssi %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  configTzTime("TRT-3", "pool.ntp.org", "time.google.com");

  uiSetOnline(true);
  netStartTask();
  uiShowMain();
}

static char cmdBuf[16];
static uint8_t cmdLen = 0;

static void pollSerialCommand() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      cmdBuf[cmdLen] = 0;
      if (strcmp(cmdBuf, "cal") == 0) {
        Serial.println("[ui] serial calibration");
        uiCalibrate(60000);
        uiShowMain();
      }
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

void loop() {
  pollSerialCommand();
  uiPoll();
  uiTick();

  if (uiWifiResetRequested()) {
    uiClearWifiReset();
    wm.resetSettings();
    delay(300);
    ESP.restart();
  }

  uint32_t now = millis();
  if (now - lastWifiCheck > 5000) {
    lastWifiCheck = now;
    bool up = (WiFi.status() == WL_CONNECTED);
    uiSetOnline(up);
    if (!up && now - lastReconnect > 15000) {
      lastReconnect = now;
      WiFi.reconnect();
    }
  }

  delay(2);
}

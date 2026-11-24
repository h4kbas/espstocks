#pragma once

#include <Arduino.h>

void uiBegin();
bool uiNeedsCalibration();
void uiTick();
void uiPoll();
void uiSetOnline(bool online);
void uiBootSplash(const char* title, const char* sub);
void uiShowWifiConnect();
void uiShowWifiPortal();
void uiShowMain();
bool uiCalibrate(uint32_t timeoutMs = 30000);
void uiUseDefaultCalibration();
bool uiWifiResetRequested();
void uiClearWifiReset();
void uiApplyBrightness(uint8_t level);

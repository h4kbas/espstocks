#pragma once

#include <Arduino.h>

void watchInit();
bool watchEnabled(int i);
bool watchSet(int i, bool on);
int watchCount();
void watchResetDefaults();

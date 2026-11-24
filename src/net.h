#pragma once

#include <Arduino.h>
#include "config.h"
#include "symbols.h"

struct Quote {
  uint8_t idx;
  bool valid;
  uint8_t fails;
  int16_t code;
  uint32_t ts;
  float price;
  float prev;
  float pct;
  float high;
  float low;
  float hist[HIST_LEN];
  uint8_t histN;
};

void netInit();
void netStartTask();
int netCount();
bool netGet(int i, Quote* out);
uint32_t netVersion();
uint32_t netLastCycleMs();
void netSetRefresh(uint32_t secs);
uint32_t netGetRefresh();
bool netOnline();
void netRequestRefresh();

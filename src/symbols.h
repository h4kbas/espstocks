#pragma once

#include <Arduino.h>

enum Source : uint8_t { SRC_YAHOO, SRC_BINANCE };
enum Market : uint8_t { MKT_CRYPTO, MKT_BIST, MKT_US, MKT_ALL };

struct SymbolDef {
  const char* label;
  const char* name;
  const char* id;
  Source src;
  Market mkt;
  bool def;
};

extern const SymbolDef CATALOG[];
extern const int CATALOG_COUNT;
extern const char* const MARKET_NAMES[];

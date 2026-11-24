#include "watch.h"

#include <Preferences.h>

#include "config.h"
#include "symbols.h"

static const uint8_t WATCH_VERSION = 1;
static const uint16_t MASK_BYTES = (MAX_SYMBOLS + 7) / 8;

static Preferences prefs;
static uint8_t mask[MASK_BYTES];

static bool maskGet(int i) { return (mask[i >> 3] >> (i & 7)) & 0x01; }

static void maskSet(int i, bool on) {
  if (on) {
    mask[i >> 3] |= (uint8_t)(1u << (i & 7));
  } else {
    mask[i >> 3] &= (uint8_t)~(1u << (i & 7));
  }
}

static void watchSave() { prefs.putBytes("watch", mask, sizeof(mask)); }

void watchResetDefaults() {
  memset(mask, 0, sizeof(mask));
  for (int i = 0; i < CATALOG_COUNT; i++) {
    if (CATALOG[i].def) maskSet(i, true);
  }
}

void watchInit() {
  prefs.begin("espstocks", false);
  size_t n = prefs.getBytes("watch", mask, sizeof(mask));
  uint8_t ver = prefs.getUChar("wver", 0);
  if (ver < WATCH_VERSION || n != sizeof(mask)) {
    watchResetDefaults();
    prefs.putUChar("wver", WATCH_VERSION);
    watchSave();
    return;
  }
  for (int i = CATALOG_COUNT; i < MAX_SYMBOLS; i++) maskSet(i, false);
  if (watchCount() == 0) {
    watchResetDefaults();
    watchSave();
  }
}

bool watchEnabled(int i) {
  if (i < 0 || i >= CATALOG_COUNT) return false;
  return maskGet(i);
}

int watchCount() {
  int n = 0;
  for (int i = 0; i < CATALOG_COUNT; i++) {
    if (maskGet(i)) n++;
  }
  return n;
}

bool watchSet(int i, bool on) {
  if (i < 0 || i >= CATALOG_COUNT) return false;
  if (on && !maskGet(i) && watchCount() >= MAX_WATCH) return false;
  maskSet(i, on);
  watchSave();
  return true;
}

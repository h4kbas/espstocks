#include "ui.h"

#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#include "config.h"
#include "net.h"
#include "symbols.h"
#include "watch.h"

enum Screen : uint8_t { SCR_MAIN, SCR_DETAIL, SCR_SETTINGS, SCR_SYMBOLS };

struct Rect {
  int16_t x, y, w, h;
};

TFT_eSPI tft = TFT_eSPI();
static SPIClass touchSpi(VSPI);
static XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
static Preferences prefs;

static const uint8_t LAYOUT_COUNT = 4;
static const uint8_t LAYOUT_COLS[LAYOUT_COUNT] = {1, 1, 2, 3};
static const uint8_t LAYOUT_ROWS[LAYOUT_COUNT] = {1, 2, 2, 2};
static const char* const LAYOUT_LABEL[LAYOUT_COUNT] = {"1x1", "1x2", "2x2", "3x2"};

static const uint32_t REFRESH_OPTS[] = {30, 60, 120, 300};
static const uint8_t REFRESH_OPT_COUNT = 4;
static const uint8_t BRIGHT_OPTS[] = {255, 190, 130, 70};
static const uint8_t BRIGHT_OPT_COUNT = 4;
static const char* const BRIGHT_LABEL[] = {"100%", "75%", "50%", "27%"};

static Screen screen = SCR_MAIN;
static Market market = MKT_CRYPTO;
static uint8_t layoutIdx = 2;
static uint8_t page = 0;
static int16_t detailIdx = -1;
static bool online = false;
static bool wifiReset = false;

static int8_t marketIdx[MAX_SYMBOLS];
static int marketCount = 0;

static int16_t calXLeft = CAL_X1, calXRight = CAL_X2, calYTop = CAL_Y1, calYBottom = CAL_Y2;
static bool calValid = false;

static uint32_t drawnTs[MAX_SYMBOLS];
static bool drawnValid[MAX_SYMBOLS];
static int8_t drawnFails[MAX_SYMBOLS];

static Market symFilter = MKT_CRYPTO;
static int16_t symScroll = 0;
static bool pressMoved = false;
static int16_t scrollAnchorY = 0;

#define SYM_Y0 46
#define SYM_ROW_H 32
#define SYM_ROWS 5

static uint32_t lastDetailTick = 0xFFFFFFFF;
static bool pressActive = false;
static int16_t pressX = 0, pressY = 0, lastX = 0, lastY = 0;
static uint32_t pressMs = 0, lastTouchMs = 0, lastAutoPageMs = 0, lastTouchRead = 0;
static int8_t lastMinute = -1;

// ---------------------------------------------------------------- formatting

static void formatNumber(char* out, size_t cap, float v, int dec) {
  char raw[40];
  dtostrf(v, 0, dec, raw);
  const char* p = raw;
  bool neg = false;
  if (*p == '-') {
    neg = true;
    p++;
  }
  const char* dot = strchr(p, '.');
  int intLen = dot ? (int)(dot - p) : (int)strlen(p);
  size_t o = 0;
  if (neg && o + 1 < cap) out[o++] = '-';
  for (int i = 0; i < intLen && o + 1 < cap; i++) {
    if (i > 0 && (intLen - i) % 3 == 0 && o + 1 < cap) out[o++] = ',';
    out[o++] = p[i];
  }
  if (dot) {
    while (*dot && o + 1 < cap) out[o++] = *dot++;
  }
  out[o] = 0;
}

static int priceDecimals(float v) { return v >= 1.0f ? 2 : 4; }

static void fmtPrice(char* out, size_t cap, float v, int maxDec) {
  int dec = priceDecimals(v);
  if (dec > maxDec) dec = maxDec;
  formatNumber(out, cap, v, dec);
}

static void fmtPct(char* out, size_t cap, float v) {
  char tmp[24];
  formatNumber(tmp, sizeof(tmp), v, 2);
  snprintf(out, cap, "%s%s%%", v > 0 ? "+" : "", tmp);
}

static uint16_t pctColor(float pct) {
  if (pct > 0.005f) return C_UP;
  if (pct < -0.005f) return C_DOWN;
  return C_FLAT;
}

static void setFontTiny() { tft.setTextFont(1); }

static void drawSpark(const float* v, uint8_t n, int16_t x, int16_t y, int16_t w, int16_t h,
                      uint16_t col, uint16_t fill) {
  if (n < 2 || w < 3 || h < 3) return;
  float mn = v[0], mx = v[0];
  for (uint8_t i = 1; i < n; i++) {
    if (v[i] < mn) mn = v[i];
    if (v[i] > mx) mx = v[i];
  }
  if (mx - mn < 1e-7f) {
    mx = mn + 1.0f;
    mn -= 1.0f;
  }
  int16_t px = x;
  int16_t py = (int16_t)(y + h - 1 - (v[0] - mn) * (h - 1) / (mx - mn));
  for (uint8_t i = 1; i < n; i++) {
    int16_t nx = (int16_t)(x + (int32_t)i * (w - 1) / (n - 1));
    int16_t ny = (int16_t)(y + h - 1 - (v[i] - mn) * (h - 1) / (mx - mn));
    if (fill) tft.drawFastVLine(nx, ny, y + h - ny, fill);
    tft.drawLine(px, py, nx, ny, col);
    px = nx;
    py = ny;
  }
}

// ---------------------------------------------------------------- geometry

static uint8_t pageSize() { return LAYOUT_COLS[layoutIdx] * LAYOUT_ROWS[layoutIdx]; }

static uint8_t pageCount() {
  if (marketCount == 0) return 1;
  int per = pageSize();
  return (uint8_t)((marketCount + per - 1) / per);
}

static void tileRect(uint8_t slot, Rect* r) {
  uint8_t cols = LAYOUT_COLS[layoutIdx];
  uint8_t rows = LAYOUT_ROWS[layoutIdx];
  const int16_t g = 4;
  int16_t cw = (SCR_W - g * (cols + 1)) / cols;
  int16_t ch = (BODY_H - g * (rows + 1)) / rows;
  uint8_t c = slot % cols;
  uint8_t rw = slot / cols;
  r->x = g + c * (cw + g);
  r->y = BODY_Y + g + rw * (ch + g);
  r->w = cw;
  r->h = ch;
}

static int quoteAt(uint8_t slot) {
  int i = page * pageSize() + slot;
  if (i < 0 || i >= marketCount) return -1;
  return marketIdx[i];
}

static void rebuildMarket() {
  marketCount = 0;
  for (int i = 0; i < CATALOG_COUNT; i++) {
    if (!watchEnabled(i)) continue;
    if (market == MKT_ALL || CATALOG[i].mkt == market) {
      marketIdx[marketCount++] = (int8_t)i;
    }
  }
  if (page >= pageCount()) page = 0;
}

// ---------------------------------------------------------------- touch

static void mapTouch(int16_t rx, int16_t ry, int16_t* sx, int16_t* sy) {
  float fx = (float)(rx - calXLeft) * (SCR_W - 1) / (float)(calXRight - calXLeft);
  float fy = (float)(ry - calYTop) * (SCR_H - 1) / (float)(calYBottom - calYTop);
  if (fx < 0) fx = 0;
  if (fx > SCR_W - 1) fx = SCR_W - 1;
  if (fy < 0) fy = 0;
  if (fy > SCR_H - 1) fy = SCR_H - 1;
  *sx = (int16_t)fx;
  *sy = (int16_t)fy;
}

static bool waitTapRaw(int16_t* rx, int16_t* ry, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (ts.touched() && millis() - start < timeoutMs) delay(10);
  while (millis() - start < timeoutMs) {
    if (ts.touched()) {
      TS_Point p = ts.getPoint();
      uint32_t t0 = millis();
      while (ts.touched() && millis() - t0 < 500) delay(10);
      *rx = p.x;
      *ry = p.y;
      return true;
    }
    delay(10);
  }
  return false;
}

static void applyCalibration() {
  prefs.putShort("calx1", calXLeft);
  prefs.putShort("calx2", calXRight);
  prefs.putShort("caly1", calYTop);
  prefs.putShort("caly2", calYBottom);
  prefs.putBool("cal", true);
  calValid = true;
}

void uiApplyBrightness(uint8_t level) {
  prefs.putUChar("bright", level);
  ledcWrite(0, level);
}

static void drawCalTarget(int16_t x, int16_t y, const char* label) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TXT, C_BG);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.drawString("TOUCH CALIBRATION", 8, 8);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(label, 8, 26);
  tft.drawCircle(x, y, 14, C_ACC);
  tft.drawCircle(x, y, 4, C_ACC);
  tft.drawFastHLine(x - 20, y, 40, C_ACC);
  tft.drawFastVLine(x, y - 20, 40, C_ACC);
  setFontTiny();
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("Tap the circle centre", 8, SCR_H - 16);
}

bool uiCalibrate(uint32_t timeoutMs) {
  const int16_t t1x = 34, t1y = 46;
  const int16_t t2x = SCR_W - 34, t2y = SCR_H - 46;
  tft.fillScreen(C_BG);
  for (int attempt = 0; attempt < 3; attempt++) {
    int16_t rx1, ry1, rx2, ry2;
    drawCalTarget(t1x, t1y, "Top left target");
    if (!waitTapRaw(&rx1, &ry1, timeoutMs)) {
      Serial.println("[ui] calibration timeout");
      return false;
    }
    Serial.printf("[ui] cal point 1 raw %d,%d\n", rx1, ry1);
    delay(250);
    drawCalTarget(t2x, t2y, "Bottom right target");
    if (!waitTapRaw(&rx2, &ry2, timeoutMs)) {
      Serial.println("[ui] calibration timeout");
      return false;
    }
    Serial.printf("[ui] cal point 2 raw %d,%d\n", rx2, ry2);

    if (abs(rx2 - rx1) < 700 || abs(ry2 - ry1) < 700) {
      tft.fillScreen(C_BG);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(C_DOWN, C_BG);
      tft.setFreeFont(&FreeSansBold9pt7b);
      tft.drawString("Calibration failed, retry", 8, 100);
      delay(1500);
      continue;
    }

    float rawPerPxX = (float)(rx2 - rx1) / (float)(t2x - t1x);
    float rawPerPxY = (float)(ry2 - ry1) / (float)(t2y - t1y);
    calXLeft = (int16_t)lroundf((float)rx1 - t1x * rawPerPxX);
    calXRight = (int16_t)lroundf((float)calXLeft + (SCR_W - 1) * rawPerPxX);
    calYTop = (int16_t)lroundf((float)ry1 - t1y * rawPerPxY);
    calYBottom = (int16_t)lroundf((float)calYTop + (SCR_H - 1) * rawPerPxY);
    applyCalibration();
    Serial.printf("[ui] calibrated x %d..%d y %d..%d\n", calXLeft, calXRight, calYTop, calYBottom);

    tft.fillScreen(C_BG);
    tft.setTextColor(C_UP, C_BG);
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.drawString("Touch calibrated", 8, 100);
    delay(700);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- chrome

static void drawClock() {
  char buf[12] = "--:--";
  time_t now = time(nullptr);
  if (now > 1600000000) {
    struct tm t;
    localtime_r(&now, &t);
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    lastMinute = (int8_t)t.tm_min;
  }
  tft.fillRect(SCR_W - 74, 2, 60, 18, C_BG);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_TXT, C_BG);
  tft.drawString(buf, SCR_W - 22, 3);
}

static void drawWifiDot() {
  uint16_t col = online ? C_UP : C_DOWN;
  tft.fillRect(SCR_W - 18, 4, 14, 14, C_BG);
  tft.fillCircle(SCR_W - 11, 11, 4, col);
}

static void drawHeader() {
  tft.fillRect(0, 0, SCR_W, HDR_H, C_BG);
  tft.drawFastHLine(0, HDR_H - 1, SCR_W, C_LINE);

  tft.setTextDatum(TL_DATUM);
  if (screen == SCR_DETAIL) {
    tft.fillRoundRect(2, 3, 26, 16, 3, C_CARD2);
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(C_ACC, C_CARD2);
    tft.drawString("<", 9, 3);

    const Quote* q = nullptr;
    Quote tmp;
    if (detailIdx >= 0 && netGet(detailIdx, &tmp)) q = &tmp;
    if (q) {
      tft.setTextColor(C_TXT, C_BG);
      tft.drawString(CATALOG[q->idx].label, 34, 3);
      int16_t nx = 34 + tft.textWidth(CATALOG[q->idx].label) + 8;
      tft.setFreeFont(&FreeSans9pt7b);
      tft.setTextColor(C_DIM, C_BG);
      const char* name = CATALOG[q->idx].name;
      if (tft.textWidth(name) > SCR_W - 22 - nx) setFontTiny();
      tft.drawString(name, nx, 5);
    }
  } else if (screen == SCR_SETTINGS) {
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(C_TXT, C_BG);
    tft.drawString("SETTINGS", 8, 3);
  } else if (screen == SCR_SYMBOLS) {
    tft.fillRoundRect(2, 3, 26, 16, 3, C_CARD2);
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(C_ACC, C_CARD2);
    tft.drawString("<", 9, 3);
    tft.setTextColor(C_TXT, C_BG);
    tft.drawString("SYMBOLS", 34, 3);
    int16_t lw = tft.textWidth("SYMBOLS");
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%d/%d", watchCount(), MAX_WATCH);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString(cnt, 34 + lw + 8, 5);
  } else {
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(C_ACC, C_BG);
    tft.drawString(MARKET_NAMES[market], 6, 3);
    int16_t tx = 6 + tft.textWidth(MARKET_NAMES[market]) + 8;

    tft.setFreeFont(&FreeSans9pt7b);
    tft.setTextColor(C_DIM, C_BG);
    char pb[12];
    snprintf(pb, sizeof(pb), "%d/%d", page + 1, pageCount());
    tft.drawString(pb, tx, 5);
    tx += tft.textWidth(pb) + 8;

    uint8_t pc = pageCount();
    for (uint8_t i = 0; i < pc && i < 6; i++) {
      if (i == page) {
        tft.fillCircle(tx + i * 8 + 2, 11, 2, C_ACC);
      } else {
        tft.drawCircle(tx + i * 8 + 2, 11, 2, C_LINE);
      }
    }
  }

  drawClock();
  drawWifiDot();
}

static void drawNav() {
  tft.fillRect(0, NAV_Y, SCR_W, NAV_H, C_BG);
  tft.drawFastHLine(0, NAV_Y, SCR_W, C_LINE);

  const char* tabs[4] = {"CRYPTO", "BIST", "US", "ALL"};
  const Market tabMarkets[4] = {MKT_CRYPTO, MKT_BIST, MKT_US, MKT_ALL};
  const int16_t tx[4] = {2, 57, 101, 135};
  const int16_t tw[4] = {55, 44, 34, 42};

  for (int i = 0; i < 4; i++) {
    bool active = (screen == SCR_MAIN && market == tabMarkets[i]);
    if (active) tft.fillRoundRect(tx[i], NAV_Y + 4, tw[i], NAV_H - 8, 3, C_CARD2);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(active ? C_ACC : C_DIM, active ? C_CARD2 : C_BG);
    tft.drawString(tabs[i], tx[i] + tw[i] / 2, NAV_Y + NAV_H / 2);
  }

  tft.fillRoundRect(179, NAV_Y + 4, 60, NAV_H - 8, 3, C_CARD);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TXT, C_CARD);
  tft.drawString(LAYOUT_LABEL[layoutIdx], 209, NAV_Y + NAV_H / 2);

  tft.fillRoundRect(242, NAV_Y + 4, 76, NAV_H - 8, 3, C_CARD);
  tft.setTextColor(C_DIM, C_CARD);
  tft.drawString("SET", 280, NAV_Y + NAV_H / 2);
}

// ---------------------------------------------------------------- tiles

static void drawTile(int qi, uint8_t slot) {
  Quote q;
  if (!netGet(qi, &q)) return;
  Rect r;
  tileRect(slot, &r);

  const bool big = r.h > 130;
  const bool small = r.w < 120 || r.h < 78;
  uint16_t col = q.valid ? pctColor(q.pct) : C_DIM;

  tft.fillRect(r.x, r.y, r.w, r.h, C_CARD);
  tft.drawRect(r.x, r.y, r.w, r.h, C_LINE);
  tft.fillRect(r.x + 1, r.y + 1, 3, r.h - 2, col);

  int16_t pad = small ? 5 : 7;
  int16_t x = r.x + pad + 2;
  int16_t y = r.y + 4;

  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(big ? &FreeSansBold12pt7b : &FreeSansBold9pt7b);
  tft.setTextColor(C_TXT, C_CARD);
  tft.drawString(CATALOG[q.idx].label, x, y);
  int16_t symW = tft.textWidth(CATALOG[q.idx].label);

  char chg[16] = "--";
  if (q.valid) fmtPct(chg, sizeof(chg), q.pct);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(col, C_CARD);
  if (small) {
    setFontTiny();
  } else {
    tft.setFreeFont(big ? &FreeSansBold12pt7b : &FreeSans9pt7b);
  }
  tft.drawString(chg, r.x + r.w - 4, y + (small ? 3 : 1));

  char price[20] = "--";
  if (q.valid) {
    int maxDec = small ? 2 : 4;
    if (r.w < 120 && q.price >= 1000) maxDec = 0;
    if (q.price >= 100000) maxDec = 0;
    fmtPrice(price, sizeof(price), q.price, maxDec);
  }

  int16_t py = y + (big ? 24 : (small ? 15 : 18));
  const GFXfont* priceFont =
      big ? &FreeSansBold24pt7b : (small ? &FreeSansBold9pt7b : &FreeSansBold12pt7b);
  tft.setFreeFont(priceFont);
  if (!q.valid) tft.setTextColor(C_DIM, C_CARD);
  else tft.setTextColor(C_TXT, C_CARD);
  tft.setTextDatum(TL_DATUM);
  if (tft.textWidth(price) > r.w - pad * 2 && priceFont != &FreeSansBold9pt7b) {
    tft.setFreeFont(&FreeSansBold9pt7b);
  }
  tft.drawString(price, x, py);
  int16_t priceH = tft.fontHeight();

  if (q.valid && q.fails > 0) {
    setFontTiny();
    tft.setTextColor(C_AMBER, C_CARD);
    tft.setTextDatum(BR_DATUM);
    tft.drawString("old", r.x + r.w - 3, r.y + r.h - 2);
  }

  int16_t sx = x;
  int16_t sy = py + priceH + 2;
  int16_t sh = r.y + r.h - sy - 4;
  if (sh > 4) {
    drawSpark(q.hist, q.histN, sx, sy, r.w - pad * 2 - 4, sh, col,
              q.valid ? tft.color565(8, 24, 20) : C_CARD);
  }
}

static void drawGrid() {
  tft.fillRect(0, BODY_Y, SCR_W, BODY_H, C_BG);
  if (marketCount == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("No symbols selected", SCR_W / 2, BODY_Y + BODY_H / 2 - 10);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.drawString("SET > Symbols", SCR_W / 2, BODY_Y + BODY_H / 2 + 12);
    return;
  }
  uint8_t per = pageSize();
  for (uint8_t slot = 0; slot < per; slot++) {
    int qi = quoteAt(slot);
    if (qi < 0) {
      Rect r;
      tileRect(slot, &r);
      tft.drawRect(r.x, r.y, r.w, r.h, C_LINE);
      continue;
    }
    drawnTs[qi] = 0xFFFFFFFF;
    drawnFails[qi] = -1;
    drawTile(qi, slot);
    Quote q;
    if (netGet(qi, &q)) {
      drawnTs[qi] = q.ts;
      drawnValid[qi] = q.valid;
      drawnFails[qi] = (int8_t)q.fails;
    }
  }
}

// ---------------------------------------------------------------- detail

static void drawDetail() {
  Quote q;
  if (detailIdx < 0 || !netGet(detailIdx, &q)) return;

  tft.fillRect(0, BODY_Y, SCR_W, BODY_H, C_BG);

  char buf[32];
  if (!q.valid) {
    tft.setTextDatum(MC_DATUM);
    tft.setFreeFont(&FreeSansBold12pt7b);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("loading...", SCR_W / 2, SCR_H / 2);
    return;
  }

  uint16_t col = pctColor(q.pct);

  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.setTextColor(C_TXT, C_BG);
  fmtPrice(buf, sizeof(buf), q.price, 4);
  if (tft.textWidth(buf) > SCR_W - 16) tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString(buf, 8, 26);

  char chg[16];
  fmtPct(chg, sizeof(chg), q.pct);
  char abs[24];
  formatNumber(abs, sizeof(abs), q.price - q.prev, priceDecimals(q.price));
  char line[48];
  snprintf(line, sizeof(line), "%s  (%s%s)", chg, (q.price - q.prev) >= 0 ? "+" : "", abs);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(col, C_BG);
  tft.drawString(line, 8, 62);

  const int16_t cx = 8, cy = 84, cw = SCR_W - 16, ch = 84;
  tft.fillRect(cx, cy, cw, ch, C_CARD);
  if (q.histN >= 2) {
    float mn = q.hist[0], mx = q.hist[0];
    for (uint8_t i = 1; i < q.histN; i++) {
      if (q.hist[i] < mn) mn = q.hist[i];
      if (q.hist[i] > mx) mx = q.hist[i];
    }
    if (mx - mn < 1e-7f) {
      mx += 0.5f;
      mn -= 0.5f;
    }
    for (int i = 1; i < 3; i++) {
      tft.drawFastHLine(cx, cy + ch * i / 3, cw, C_LINE);
    }
    drawSpark(q.hist, q.histN, cx + 2, cy + 2, cw - 4, ch - 4, col, tft.color565(6, 20, 18));

    char lv[20];
    setFontTiny();
    tft.setTextColor(C_DIM, C_CARD);
    tft.setTextDatum(TL_DATUM);
    fmtPrice(lv, sizeof(lv), mx, 2);
    tft.drawString(lv, cx + 3, cy + 2);
    tft.setTextDatum(BL_DATUM);
    fmtPrice(lv, sizeof(lv), mn, 2);
    tft.drawString(lv, cx + 3, cy + ch - 2);
  } else {
    tft.setTextDatum(MC_DATUM);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.setTextColor(C_DIM, C_CARD);
    tft.drawString("no history yet", cx + cw / 2, cy + ch / 2);
  }

  char prevBuf[24], highBuf[24], lowBuf[24], updBuf[16], chgBuf[24];
  fmtPrice(prevBuf, sizeof(prevBuf), q.prev, 2);
  fmtPrice(highBuf, sizeof(highBuf), q.high, 2);
  fmtPrice(lowBuf, sizeof(lowBuf), q.low, 2);
  uint32_t age = millis() / 1000 - q.ts;
  if (age < 120) {
    snprintf(updBuf, sizeof(updBuf), "%lus", (unsigned long)age);
  } else {
    snprintf(updBuf, sizeof(updBuf), "%lum", (unsigned long)(age / 60));
  }
  formatNumber(chgBuf, sizeof(chgBuf), q.price - q.prev, priceDecimals(q.price));

  const char* leftLabel[3] = {"PREV", "LOW", "SRC"};
  const char* rightLabel[3] = {"HIGH", "UPD", "CHG"};
  const char* leftValue[3] = {prevBuf, lowBuf,
                              CATALOG[q.idx].src == SRC_BINANCE ? "BINANCE 15m" : "YAHOO 15m"};
  char chgSigned[26];
  snprintf(chgSigned, sizeof(chgSigned), "%s%s", (q.price - q.prev) >= 0 ? "+" : "", chgBuf);
  const char* rightValue[3] = {highBuf, updBuf, chgSigned};

  for (int i = 0; i < 3; i++) {
    int16_t y = 175 + i * 12;
    setFontTiny();
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString(leftLabel[i], 8, y);
    tft.drawString(rightLabel[i], 166, y);
    tft.setTextColor(C_TXT, C_BG);
    tft.drawString(leftValue[i], 40, y);
    tft.drawString(rightValue[i], 198, y);
  }
}

// ---------------------------------------------------------------- settings

static void drawSettings() {
  tft.fillRect(0, BODY_Y, SCR_W, BODY_H + NAV_H, C_BG);

  uint32_t refresh = netGetRefresh();
  char refBuf[16];
  snprintf(refBuf, sizeof(refBuf), "%lus", (unsigned long)refresh);
  uint8_t bright = prefs.getUChar("bright", 255);
  const char* brightTxt = "100%";
  for (uint8_t i = 0; i < BRIGHT_OPT_COUNT; i++) {
    if (BRIGHT_OPTS[i] == bright) brightTxt = BRIGHT_LABEL[i];
  }

  char symBuf[16];
  snprintf(symBuf, sizeof(symBuf), "%d/%d", watchCount(), MAX_WATCH);

  const char* labels[6] = {"Refresh",   "Brightness", "Symbols",
                           "Touch calibrate", "WiFi reset", "Back"};
  const char* values[6] = {refBuf, brightTxt, symBuf, ">", ">", ""};

  for (int i = 0; i < 6; i++) {
    int16_t y = BODY_Y + 4 + i * 31;
    tft.fillRoundRect(6, y, SCR_W - 12, 28, 4, C_CARD);
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(i == 5 ? C_ACC : C_TXT, C_CARD);
    tft.drawString(labels[i], 16, y + 14);
    if (values[i][0]) {
      tft.setFreeFont(&FreeSans9pt7b);
      tft.setTextDatum(MR_DATUM);
      tft.setTextColor(C_DIM, C_CARD);
      tft.drawString(values[i], SCR_W - 16, y + 14);
    }
  }
}

// ---------------------------------------------------------------- symbol picker

static int symFilterCount() {
  int n = 0;
  for (int i = 0; i < CATALOG_COUNT; i++) {
    if (CATALOG[i].mkt == symFilter) n++;
  }
  return n;
}

static int symIndexAt(int row) {
  int want = symScroll + row;
  int n = 0;
  for (int i = 0; i < CATALOG_COUNT; i++) {
    if (CATALOG[i].mkt != symFilter) continue;
    if (n == want) return i;
    n++;
  }
  return -1;
}

static void drawSymbols() {
  tft.fillRect(0, BODY_Y, SCR_W, SCR_H - BODY_Y, C_BG);

  const char* names[3] = {"CRYPTO", "BIST", "US"};
  const Market mks[3] = {MKT_CRYPTO, MKT_BIST, MKT_US};
  const int16_t xs[3] = {6, 72, 120};
  const int16_t ws[3] = {62, 44, 34};
  for (int i = 0; i < 3; i++) {
    bool active = (symFilter == mks[i]);
    if (active) tft.fillRoundRect(xs[i], 26, ws[i], 18, 3, C_CARD2);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(active ? C_ACC : C_DIM, active ? C_CARD2 : C_BG);
    tft.drawString(names[i], xs[i] + ws[i] / 2, 35);
  }

  int total = symFilterCount();
  for (int row = 0; row < SYM_ROWS; row++) {
    int i = symIndexAt(row);
    if (i < 0) break;
    int16_t y = SYM_Y0 + row * SYM_ROW_H;
    bool on = watchEnabled(i);
    uint16_t bg = on ? C_CARD2 : C_CARD;
    tft.fillRoundRect(6, y, 304, 28, 3, bg);
    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(on ? C_TXT : C_DIM, bg);
    tft.drawString(CATALOG[i].label, 14, y + 2);
    setFontTiny();
    tft.setTextColor(C_DIM, bg);
    tft.drawString(CATALOG[i].name, 14, y + 17);
    tft.drawRect(288, y + 7, 13, 13, C_LINE);
    if (on) tft.fillRect(290, y + 9, 9, 9, C_UP);
  }

  if (total > SYM_ROWS) {
    int16_t trackY = SYM_Y0;
    int16_t trackH = SYM_ROWS * SYM_ROW_H - 4;
    int16_t thumbH = trackH * SYM_ROWS / total;
    if (thumbH < 12) thumbH = 12;
    int16_t thumbY = trackY + (trackH - thumbH) * symScroll / (total - SYM_ROWS);
    tft.fillRect(313, trackY, 3, trackH, C_CARD);
    tft.fillRect(313, thumbY, 3, thumbH, C_ACC);
  }

  tft.fillRoundRect(6, 214, 90, 22, 3, C_CARD);
  tft.fillRoundRect(224, 214, 90, 22, 3, C_CARD);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM, C_CARD);
  tft.drawString("RESET", 51, 225);
  tft.setTextColor(C_ACC, C_CARD);
  tft.drawString("DONE", 269, 225);
}

static void symScrollBy(int delta) {
  int total = symFilterCount();
  int maxScroll = total - SYM_ROWS;
  if (maxScroll < 0) maxScroll = 0;
  int v = symScroll + delta;
  if (v < 0) v = 0;
  if (v > maxScroll) v = maxScroll;
  if (v != symScroll) {
    symScroll = (int16_t)v;
    drawSymbols();
  }
}

// ---------------------------------------------------------------- screens

void uiShowMain() {
  screen = SCR_MAIN;
  drawHeader();
  drawGrid();
  drawNav();
}

void uiBootSplash(const char* title, const char* sub) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.setTextColor(C_ACC, C_BG);
  tft.drawString(title, 14, 70);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(sub, 16, 118);
  tft.drawFastHLine(16, 108, 288, C_LINE);
}

void uiShowWifiConnect() { uiBootSplash("ESP STOCKS", "connecting to WiFi..."); }

void uiShowWifiPortal() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_ACC, C_BG);
  tft.drawString("WiFi setup", 12, 16);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_TXT, C_BG);
  tft.drawString("1. Join WiFi:", 12, 52);
  tft.setTextColor(C_AMBER, C_BG);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.drawString(WIFI_AP_NAME, 24, 70);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_TXT, C_BG);
  tft.drawString("pass:", 12, 88);
  tft.setTextColor(C_AMBER, C_BG);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.drawString(WIFI_AP_PASS, 60, 88);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_TXT, C_BG);
  tft.drawString("2. Open http://192.168.4.1", 12, 116);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("3. Pick your network, save", 12, 134);
  tft.drawString("Portal closes after 4 minutes", 12, 158);
  setFontTiny();
  tft.setTextColor(C_LINE, C_BG);
  tft.drawString("Hold BOOT at power-on to recalibrate touch", 12, SCR_H - 14);
}

void uiSetOnline(bool isOnline) {
  if (online == isOnline) return;
  online = isOnline;
  drawWifiDot();
}

bool uiWifiResetRequested() { return wifiReset; }
void uiClearWifiReset() { wifiReset = false; }

// ---------------------------------------------------------------- interaction

static void openSettings() {
  screen = SCR_SETTINGS;
  drawHeader();
  drawSettings();
}

static void backToMain() {
  screen = SCR_MAIN;
  uiShowMain();
}

static void openDetail(int qi) {
  detailIdx = (int16_t)qi;
  lastDetailTick = 0xFFFFFFFF;
  screen = SCR_DETAIL;
  drawHeader();
  drawDetail();
}

static void openSymbols() {
  screen = SCR_SYMBOLS;
  symFilter = MKT_CRYPTO;
  symScroll = 0;
  drawHeader();
  drawSymbols();
}

static void changePage(int delta) {
  uint8_t pc = pageCount();
  if (pc < 2) return;
  int p = (int)page + delta;
  while (p < 0) p += pc;
  page = (uint8_t)(p % pc);
  lastAutoPageMs = millis();
  drawHeader();
  drawGrid();
  drawNav();
}

static void changeMarket(Market m) {
  market = m;
  page = 0;
  prefs.putUChar("market", (uint8_t)m);
  rebuildMarket();
  drawHeader();
  drawGrid();
  drawNav();
}

static void cycleLayout() {
  layoutIdx = (uint8_t)((layoutIdx + 1) % LAYOUT_COUNT);
  prefs.putUChar("layout", layoutIdx);
  page = 0;
  lastAutoPageMs = millis();
  drawHeader();
  drawGrid();
  drawNav();
}

static void handleTapMain(int16_t x, int16_t y) {
  if (y >= NAV_Y) {
    if (x < 57) {
      changeMarket(MKT_CRYPTO);
    } else if (x < 101) {
      changeMarket(MKT_BIST);
    } else if (x < 135) {
      changeMarket(MKT_US);
    } else if (x < 177) {
      changeMarket(MKT_ALL);
    } else if (x < 240) {
      cycleLayout();
    } else {
      openSettings();
    }
    return;
  }
  if (y < HDR_H) {
    if (x > SCR_W - 84) {
      netRequestRefresh();
    } else if (pageCount() > 1) {
      changePage(1);
    }
    return;
  }

  uint8_t per = pageSize();
  for (uint8_t slot = 0; slot < per; slot++) {
    Rect r;
    tileRect(slot, &r);
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
      int qi = quoteAt(slot);
      if (qi >= 0) openDetail(qi);
      return;
    }
  }
}

static void handleTapSettings(int16_t x, int16_t y) {
  int16_t row = (y - BODY_Y - 4) / 31;
  if (y < BODY_Y + 4 || row > 5) return;

  if (row == 0) {
    uint32_t cur = netGetRefresh();
    uint8_t idx = 0;
    for (uint8_t i = 0; i < REFRESH_OPT_COUNT; i++) {
      if (REFRESH_OPTS[i] == cur) idx = i;
    }
    idx = (uint8_t)((idx + 1) % REFRESH_OPT_COUNT);
    netSetRefresh(REFRESH_OPTS[idx]);
    prefs.putUInt("refresh", REFRESH_OPTS[idx]);
    drawSettings();
  } else if (row == 1) {
    uint8_t cur = prefs.getUChar("bright", 255);
    uint8_t idx = 0;
    for (uint8_t i = 0; i < BRIGHT_OPT_COUNT; i++) {
      if (BRIGHT_OPTS[i] == cur) idx = i;
    }
    idx = (uint8_t)((idx + 1) % BRIGHT_OPT_COUNT);
    uiApplyBrightness(BRIGHT_OPTS[idx]);
    drawSettings();
  } else if (row == 2) {
    openSymbols();
  } else if (row == 3) {
    uiCalibrate();
    backToMain();
  } else if (row == 4) {
    wifiReset = true;
    tft.setTextColor(C_AMBER, C_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.drawString("clearing WiFi, restarting...", 8, BODY_Y + 4 + 5 * 31 + 34);
  } else {
    backToMain();
  }
}

static void handleTapSymbols(int16_t x, int16_t y) {
  if (y < HDR_H) {
    if (x < 34) backToMain();
    return;
  }
  if (y >= 210) {
    if (x < 110) {
      watchResetDefaults();
      rebuildMarket();
      netRequestRefresh();
      symScroll = 0;
      drawHeader();
      drawSymbols();
    } else if (x > 210) {
      backToMain();
    }
    return;
  }
  if (y >= 24 && y < 46) {
    if (x >= 6 && x < 68) {
      symFilter = MKT_CRYPTO;
    } else if (x >= 72 && x < 116) {
      symFilter = MKT_BIST;
    } else if (x >= 120 && x < 154) {
      symFilter = MKT_US;
    } else {
      return;
    }
    symScroll = 0;
    drawSymbols();
    return;
  }
  if (y >= SYM_Y0 && y < SYM_Y0 + SYM_ROWS * SYM_ROW_H) {
    int row = (y - SYM_Y0) / SYM_ROW_H;
    int i = symIndexAt(row);
    if (i < 0) return;
    if (!watchSet(i, !watchEnabled(i))) {
      tft.setTextColor(C_DOWN, C_BG);
      tft.setTextDatum(TC_DATUM);
      tft.setFreeFont(&FreeSansBold9pt7b);
      tft.fillRect(90, 96, 140, 22, C_BG);
      tft.drawString("max 40 symbols", SCR_W / 2, 100);
      delay(900);
    }
    rebuildMarket();
    netRequestRefresh();
    drawHeader();
    drawSymbols();
  }
}

static void handleTap(int16_t x, int16_t y) {
  lastTouchMs = millis();
  lastAutoPageMs = millis();
  static uint8_t tapLog = 0;
  if (tapLog < 24) {
    tapLog++;
    Serial.printf("[ui] tap %d,%d screen %d\n", x, y, (int)screen);
  }

  if (screen == SCR_MAIN) {
    handleTapMain(x, y);
  } else if (screen == SCR_DETAIL) {
    if (y < HDR_H && x < 34) {
      backToMain();
    }
  } else if (screen == SCR_SETTINGS) {
    if (y < HDR_H) {
      backToMain();
    } else {
      handleTapSettings(x, y);
    }
  } else if (screen == SCR_SYMBOLS) {
    handleTapSymbols(x, y);
  }
}

void uiPoll() {
  uint32_t now = millis();
  if (now - lastTouchRead < 8) return;
  lastTouchRead = now;

  bool touched = ts.touched();
  if (touched) {
    TS_Point p = ts.getPoint();
    int16_t sx, sy;
    mapTouch(p.x, p.y, &sx, &sy);
    if (!pressActive) {
      pressActive = true;
      pressMoved = false;
      pressX = sx;
      pressY = sy;
      scrollAnchorY = sy;
      pressMs = now;
      static uint8_t rawLog = 0;
      if (rawLog < 30) {
        rawLog++;
        Serial.printf("[ui] touch raw %d,%d z=%d -> %d,%d\n", p.x, p.y, p.z, sx, sy);
      }
    }
    lastX = sx;
    lastY = sy;
    lastTouchMs = now;

    if (screen == SCR_SYMBOLS && abs(sy - scrollAnchorY) >= 24) {
      symScrollBy(sy > scrollAnchorY ? -1 : 1);
      scrollAnchorY = sy;
      pressMoved = true;
    }
  } else if (pressActive) {
    pressActive = false;
    int16_t dx = (int16_t)(lastX - pressX);
    int16_t dy = (int16_t)(lastY - pressY);
    uint32_t dur = now - pressMs;

    if (abs(dx) > 45 && abs(dy) < 40) {
      if (screen == SCR_MAIN) {
        changePage(dx < 0 ? 1 : -1);
      } else if (screen == SCR_DETAIL) {
        backToMain();
      }
    } else if (!pressMoved && dur < 900 && abs(dx) < 24 && abs(dy) < 24) {
      handleTap(pressX, pressY);
    }
    pressMoved = false;
  }
}

void uiTick() {
  uint32_t now = millis();

  if (screen == SCR_MAIN) {
    uint8_t per = pageSize();
    for (uint8_t slot = 0; slot < per; slot++) {
      int qi = quoteAt(slot);
      if (qi < 0) continue;
      Quote q;
      if (!netGet(qi, &q)) continue;
      if (q.ts != drawnTs[qi] || q.valid != drawnValid[qi] || (int8_t)q.fails != drawnFails[qi]) {
        drawTile(qi, slot);
        drawnTs[qi] = q.ts;
        drawnValid[qi] = q.valid;
        drawnFails[qi] = (int8_t)q.fails;
        if (q.ts != 0xFFFFFFFF) {
          drawHeader();
          drawNav();
        }
      }
    }

    if (pageCount() > 1 && now - lastTouchMs > 20000 && now - lastAutoPageMs > 12000) {
      changePage(1);
    }
  } else if (screen == SCR_DETAIL) {
    Quote q;
    if (detailIdx >= 0 && netGet(detailIdx, &q)) {
      if (q.ts != lastDetailTick) {
        lastDetailTick = q.ts;
        drawHeader();
        drawDetail();
      }
    }
  }

  time_t t = time(nullptr);
  if (t > 1600000000) {
    struct tm tmNow;
    localtime_r(&t, &tmNow);
    if (tmNow.tm_min != lastMinute) drawClock();
  }
}

// ---------------------------------------------------------------- init

void uiBegin() {
  prefs.begin("espstocks", false);
  watchInit();

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  bool tsOk = ts.begin(touchSpi);
  ts.setRotation(1);
  TS_Point probe = ts.getPoint();
  Serial.printf("[ui] touch begin %d probe %d,%d z=%d\n", (int)tsOk, probe.x, probe.y, probe.z);

  Serial.printf("[ui] tft id %02X %02X %02X %02X\n", tft.readcommand8(0x04, 0),
                tft.readcommand8(0x04, 1), tft.readcommand8(0x04, 2), tft.readcommand8(0x04, 3));

  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BL, 0);
  ledcWrite(0, prefs.getUChar("bright", 255));

  calXLeft = prefs.getShort("calx1", CAL_X1);
  calXRight = prefs.getShort("calx2", CAL_X2);
  calYTop = prefs.getShort("caly1", CAL_Y1);
  calYBottom = prefs.getShort("caly2", CAL_Y2);
  calValid = prefs.getBool("cal", false);
  if (prefs.getUChar("calv", 0) < CAL_VERSION) {
    prefs.putUChar("calv", CAL_VERSION);
    uiUseDefaultCalibration();
  }
  if (prefs.getUChar("uiver", 0) < 2) {
    prefs.putUChar("uiver", 2);
    market = MKT_CRYPTO;
    layoutIdx = 2;
    prefs.putUChar("market", (uint8_t)market);
    prefs.putUChar("layout", layoutIdx);
  }

  market = (Market)prefs.getUChar("market", (uint8_t)MKT_CRYPTO);
  if (market > MKT_ALL) market = MKT_CRYPTO;
  layoutIdx = prefs.getUChar("layout", 2);
  if (layoutIdx >= LAYOUT_COUNT) layoutIdx = 2;

  netSetRefresh(prefs.getUInt("refresh", REFRESH_DEFAULT));

  rebuildMarket();
  Serial.printf("[ui] market %d layout %d cal %d refresh %lu\n", (int)market, layoutIdx, (int)calValid,
                (unsigned long)netGetRefresh());
}

bool uiNeedsCalibration() { return !calValid; }

void uiUseDefaultCalibration() {
  calXLeft = CAL_X1;
  calXRight = CAL_X2;
  calYTop = CAL_Y1;
  calYBottom = CAL_Y2;
  applyCalibration();
}

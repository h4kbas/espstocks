#include "net.h"

#include "watch.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static Quote quotes[MAX_SYMBOLS];
static SemaphoreHandle_t quotesMx = nullptr;
static volatile uint32_t version = 0;
static volatile uint32_t refreshSecs = REFRESH_DEFAULT;
static volatile uint32_t lastCycleMs = 0;
static volatile bool online = false;
static int16_t lastCode = 0;
static volatile bool refreshReq = false;

static void pushHist(Quote& q, float v) {
  if (!(v > 0)) return;
  if (q.histN < HIST_LEN) {
    q.hist[q.histN++] = v;
  } else {
    memmove(q.hist, q.hist + 1, sizeof(float) * (HIST_LEN - 1));
    q.hist[HIST_LEN - 1] = v;
  }
}

static String urlEncode(const char* in) {
  String out;
  for (const char* p = in; *p; p++) {
    char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '=') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
      out += buf;
    }
  }
  return out;
}

static bool fetchYahoo(Quote& q) {
  String url = "https://query1.finance.yahoo.com/v8/finance/chart/";
  url += urlEncode(CATALOG[q.idx].id);
  url += "?range=1d&interval=15m&includePrePost=false";

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(8);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", "Mozilla/5.0 (ESP32 desk monitor)");
  http.addHeader("Accept", "application/json");

  lastCode = http.GET();
  if (lastCode != 200) {
    Serial.printf("[net] %-6s http %d\n", CATALOG[q.idx].label, lastCode);
    http.end();
    return false;
  }

  JsonDocument filter;
  JsonObject fMeta = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  fMeta["regularMarketPrice"] = true;
  fMeta["chartPreviousClose"] = true;
  fMeta["regularMarketDayHigh"] = true;
  fMeta["regularMarketDayLow"] = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[net] %-6s json %s\n", CATALOG[q.idx].label, err.c_str());
    return false;
  }

  JsonObject meta = doc["chart"]["result"][0]["meta"];
  float price = meta["regularMarketPrice"] | 0.0f;
  float prev = meta["chartPreviousClose"] | 0.0f;
  float hi = meta["regularMarketDayHigh"] | 0.0f;
  float lo = meta["regularMarketDayLow"] | 0.0f;
  if (!(price > 0)) return false;

  q.histN = 0;
  for (JsonVariant v : doc["chart"]["result"][0]["indicators"]["quote"][0]["close"].as<JsonArray>()) {
    if (v.is<float>()) pushHist(q, v.as<float>());
  }
  if (q.histN == 0) pushHist(q, price);
  if (fabsf(q.hist[q.histN - 1] - price) > 1e-6f) pushHist(q, price);

  if (!(hi > 0)) {
    hi = q.hist[0];
    lo = q.hist[0];
    for (uint8_t i = 1; i < q.histN; i++) {
      if (q.hist[i] > hi) hi = q.hist[i];
      if (q.hist[i] < lo) lo = q.hist[i];
    }
  }
  if (!(prev > 0)) prev = q.hist[0];

  q.price = price;
  q.prev = prev;
  q.pct = (price - prev) / prev * 100.0f;
  q.high = hi;
  q.low = lo;
  return true;
}

static bool fetchBinance(Quote& q) {
  String url = "https://api.binance.com/api/v3/klines?symbol=";
  url += CATALOG[q.idx].id;
  url += "&interval=15m&limit=";
  url += String(HIST_LEN);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(8);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", "ESP32 desk monitor");

  lastCode = http.GET();
  if (lastCode != 200) {
    Serial.printf("[net] %-6s http %d\n", CATALOG[q.idx].label, lastCode);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[net] %-6s json %s\n", CATALOG[q.idx].label, err.c_str());
    return false;
  }

  q.histN = 0;
  float hi = 0, lo = 0;
  for (JsonArray row : doc.as<JsonArray>()) {
    float close = atof(row[4] | "0");
    float rh = atof(row[2] | "0");
    float rl = atof(row[3] | "0");
    pushHist(q, close);
    if (rh > hi) hi = rh;
    if (lo == 0 || (rl > 0 && rl < lo)) lo = rl;
  }
  if (q.histN < 2) return false;

  q.price = q.hist[q.histN - 1];
  q.prev = q.hist[0];
  q.pct = (q.price - q.prev) / q.prev * 100.0f;
  q.high = hi;
  q.low = lo;
  return true;
}

static bool fetchQuote(Quote& q) {
  if (CATALOG[q.idx].src == SRC_BINANCE) return fetchBinance(q);
  return fetchYahoo(q);
}

static void fetchTask(void*) {
  uint32_t cursor = 0;
  bool firstPass = true;

  for (;;) {
    if (refreshReq) {
      refreshReq = false;
      cursor = 0;
      firstPass = true;
    }
    online = (WiFi.status() == WL_CONNECTED);
    if (!online) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    int enabled = watchCount();
    if (enabled == 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    int i = (int)(cursor % (uint32_t)CATALOG_COUNT);
    cursor++;
    if (cursor % (uint32_t)CATALOG_COUNT == 0) {
      Serial.printf("[net] cycle done in %lus\n", (unsigned long)(millis() / 1000));
      firstPass = false;
      lastCycleMs = millis();
    }
    if (!watchEnabled(i)) continue;

    Quote tmp;
    if (!netGet(i, &tmp)) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    bool ok = fetchQuote(tmp);
    if (!ok) Serial.printf("[net] %-6s FAIL code %d\n", CATALOG[i].label, lastCode);
    if (ok) {
      tmp.valid = true;
      tmp.fails = 0;
      tmp.ts = millis() / 1000;
    } else {
      tmp.fails = (uint8_t)(tmp.fails + 1);
    }
    tmp.code = lastCode;

    if (xSemaphoreTake(quotesMx, pdMS_TO_TICKS(200)) == pdTRUE) {
      quotes[i] = tmp;
      version++;
      xSemaphoreGive(quotesMx);
    }

    uint32_t spacing = firstPass ? 350 : refreshSecs * 1000u / (uint32_t)enabled;
    if (spacing < 350) spacing = 350;
    vTaskDelay(pdMS_TO_TICKS(spacing));
  }
}

void netInit() {
  quotesMx = xSemaphoreCreateMutex();
  memset(quotes, 0, sizeof(quotes));
  for (int i = 0; i < CATALOG_COUNT; i++) quotes[i].idx = (uint8_t)i;
}

void netStartTask() {
  xTaskCreatePinnedToCore(fetchTask, "fetch", 10240, nullptr, 1, nullptr, 0);
}

int netCount() { return CATALOG_COUNT; }

bool netGet(int i, Quote* out) {
  if (i < 0 || i >= CATALOG_COUNT || !quotesMx) return false;
  if (xSemaphoreTake(quotesMx, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  *out = quotes[i];
  xSemaphoreGive(quotesMx);
  return true;
}

uint32_t netVersion() { return version; }
uint32_t netLastCycleMs() { return lastCycleMs; }
void netSetRefresh(uint32_t secs) {
  if (secs < REFRESH_MIN) secs = REFRESH_MIN;
  if (secs > REFRESH_MAX) secs = REFRESH_MAX;
  refreshSecs = secs;
}
uint32_t netGetRefresh() { return refreshSecs; }
bool netOnline() { return online; }
void netRequestRefresh() { refreshReq = true; }

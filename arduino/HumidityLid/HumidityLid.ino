#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Adafruit_AHTX0.h>
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <esp_bt.h>
#include <time.h>
#include <math.h>

#include "config.h"

// =========================================================
// CONSTANTS (mirror CircuitPython reference)
// =========================================================

static const char* STATE_FILE  = "/state.json";
static const char* BUFFER_FILE = "/buffer.json";
static const char* ERROR_LOG   = "/error.log";

static const uint32_t BATTERY_CHECK_INTERVAL = 86400;
static const uint32_t LED_ON_SECONDS         = 5;
static const size_t   MAX_ERROR_LOG_BYTES    = 10240;

static const float LEVEL_WATCH    = 15.0f;
static const float LEVEL_WARNING  = 18.0f;
static const float LEVEL_CRITICAL = 20.0f;

static const uint32_t ALERT_INTERVAL_WATCH    = 7UL * 24UL * 3600UL;
static const uint32_t ALERT_INTERVAL_WARNING  = 24UL * 3600UL;
static const uint32_t ALERT_INTERVAL_CRITICAL = 3600UL;

static const float BATTERY_WARNING   = 30.0f;
static const float BATTERY_CRITICAL  = 15.0f;
static const float BATTERY_EMERGENCY = 5.0f;

static const uint32_t BATTERY_ALERT_INTERVAL_CRITICAL  = 86400UL;
static const uint32_t BATTERY_ALERT_INTERVAL_EMERGENCY = 1800UL;

static const uint32_t INTERVAL_LADDER[] = { 1800, 3600, 7200, 21600, 43200 };
static const size_t   INTERVAL_LADDER_LEN = sizeof(INTERVAL_LADDER) / sizeof(INTERVAL_LADDER[0]);
static const uint8_t  STABLE_READINGS_TO_STEP_UP = 3;

static const uint32_t MAX_INTERVAL_BELOW_WATCH = 43200;
static const uint32_t MAX_INTERVAL_WATCH       = 21600;
static const uint32_t MAX_INTERVAL_WARNING     = 3600;
static const uint32_t MAX_INTERVAL_CRITICAL    = 1800;

static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static const uint32_t NTP_SYNC_TIMEOUT_MS     = 15000;
static const time_t   NTP_VALID_AFTER         = 1700000000;

static const uint32_t WDT_TIMEOUT_MS = 300000;

#define DEBUG
#ifdef DEBUG
  #define DBG_BEGIN()    Serial.begin(115200)
  #define DBG(...)       Serial.print(__VA_ARGS__)
  #define DBGLN(...)     Serial.println(__VA_ARGS__)
  #define DBGF(...)      Serial.printf(__VA_ARGS__)
#else
  #define DBG_BEGIN()
  #define DBG(...)
  #define DBGLN(...)
  #define DBGF(...)
#endif

// =========================================================
// GLOBALS
// =========================================================

RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint64_t persistentUs = 0;  // accumulates across deep sleeps

Adafruit_AHTX0 aht;
SFE_MAX1704X   lipo(MAX1704X_MAX17048);

// =========================================================
// MONOTONIC CLOCK (survives deep sleep)
// =========================================================
// persistentUs holds the total elapsed microseconds across all prior boot
// cycles + sleep durations. esp_timer_get_time() gives us the current
// boot's elapsed microseconds. Their sum is a monotonic counter that
// survives deep sleep (zeroes on cold boot / brownout — same caveat as
// the CircuitPython reference).

static uint64_t monotonicMs() {
  return (persistentUs + (uint64_t)esp_timer_get_time()) / 1000ULL;
}

// =========================================================
// ERROR LOG
// =========================================================

static void logError(const String& msg) {
  if (LittleFS.exists(ERROR_LOG)) {
    File f = LittleFS.open(ERROR_LOG, "r");
    size_t sz = f ? f.size() : 0;
    if (f) f.close();
    if (sz > MAX_ERROR_LOG_BYTES) {
      LittleFS.remove(ERROR_LOG);
    }
  }
  File f = LittleFS.open(ERROR_LOG, "a");
  if (!f) {
    DBG("Could not write to error log: ");
    DBGLN(msg);
    return;
  }
  char ts[24];
  time_t now = time(nullptr);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
  f.printf("[%s] %s\n", ts, msg.c_str());
  f.close();
}

// =========================================================
// STATE
// =========================================================

static void stateDefaults(JsonDocument& state) {
  state["last_humidity"]            = nullptr;
  state["last_log_time"]            = nullptr;
  state["last_alert_time"]          = nullptr;
  state["alert_level"]              = nullptr;
  state["watch_alert_sent"]         = false;
  state["consecutive_stable"]       = 0;
  state["current_interval"]         = INTERVAL_LADDER[0];
  state["last_battery_check"]       = nullptr;
  state["battery_warning_sent"]     = false;
  state["last_battery_alert_time"]  = nullptr;
  state["battery_alert_level"]      = nullptr;
}

static void loadState(JsonDocument& state) {
  File f = LittleFS.open(STATE_FILE, "r");
  if (!f) {
    DBGLN("No state file, starting fresh");
    stateDefaults(state);
    return;
  }
  DeserializationError err = deserializeJson(state, f);
  f.close();
  if (err) {
    DBG("State parse error: "); DBGLN(err.c_str());
    state.clear();
    stateDefaults(state);
    return;
  }
  DBGLN("State loaded");
}

static void saveState(const JsonDocument& state) {
  File f = LittleFS.open(STATE_FILE, "w");
  if (!f) {
    logError("Error saving state: open failed");
    return;
  }
  if (serializeJson(state, f) == 0) {
    logError("Error saving state: serialize failed");
  }
  f.close();
}

// =========================================================
// BUFFER
// =========================================================

static void loadBuffer(JsonDocument& buf) {
  buf.to<JsonArray>();
  File f = LittleFS.open(BUFFER_FILE, "r");
  if (!f) return;
  DeserializationError err = deserializeJson(buf, f);
  f.close();
  if (err || !buf.is<JsonArray>()) {
    buf.clear();
    buf.to<JsonArray>();
  }
}

static void saveBuffer(const JsonDocument& buf) {
  File f = LittleFS.open(BUFFER_FILE, "w");
  if (!f) {
    logError("Could not save buffer: open failed");
    return;
  }
  if (serializeJson(buf, f) == 0) {
    logError("Could not save buffer: serialize failed");
  }
  f.close();
}

static void addToBuffer(float temp, float humidity, float batPct, float batV, uint64_t monoMs) {
  JsonDocument buf;
  loadBuffer(buf);
  JsonArray arr = buf.as<JsonArray>();
  JsonObject e = arr.add<JsonObject>();
  e["temp"]     = temp;
  e["humidity"] = humidity;
  if (isnan(batPct)) {
    e["battery_pct"]     = nullptr;
    e["battery_voltage"] = nullptr;
  } else {
    e["battery_pct"]     = batPct;
    e["battery_voltage"] = batV;
  }
  e["monotonic_ms"] = monoMs;
  saveBuffer(buf);
  DBGF("Buffered reading — %d entries pending\n", (int)arr.size());
}

// =========================================================
// INFLUXDB
// =========================================================

static String buildInfluxBody(float temp, float humidity, time_t ts, float batPct, float batV) {
  String fields;
  fields.reserve(128);
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "temperature=%.2f,humidity=%.2f", temp, humidity);
  fields += tmp;
  if (!isnan(batPct)) {
    snprintf(tmp, sizeof(tmp), ",battery_pct=%.1f,battery_voltage=%.2f", batPct, batV);
    fields += tmp;
  }
  String body = "environment,location=bucket ";
  body += fields;
  body += ' ';
  body += String((uint32_t)ts);
  return body;
}

static bool postInflux(const String& body) {
  String url = String(INFLUXDB_HOST) +
               "/api/v2/write?org=" + INFLUXDB_ORG +
               "&bucket=" + INFLUXDB_BUCKET +
               "&precision=s";
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(url)) {
    logError("InfluxDB error: http.begin failed");
    return false;
  }
  http.addHeader("Authorization", String("Token ") + INFLUXDB_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  int code = http.POST(body);
  http.end();
  DBGF("InfluxDB: %d\n", code);
  if (code < 0) {
    logError(String("InfluxDB error: ") + HTTPClient::errorToString(code));
    return false;
  }
  return code == 204;
}

static bool logToInflux(float temp, float humidity, time_t ts, float batPct, float batV) {
  return postInflux(buildInfluxBody(temp, humidity, ts, batPct, batV));
}

static bool flushBuffer(time_t unixNow, uint64_t monoAtSyncMs) {
  JsonDocument buf;
  loadBuffer(buf);
  JsonArray arr = buf.as<JsonArray>();
  if (arr.size() == 0) return true;

  DBGF("Flushing %d buffered readings...\n", (int)arr.size());

  JsonDocument failedDoc;
  JsonArray failed = failedDoc.to<JsonArray>();

  for (JsonObject e : arr) {
    esp_task_wdt_reset();

    uint64_t entryMs = e["monotonic_ms"] | monoAtSyncMs;
    int64_t offsetMs = (int64_t)monoAtSyncMs - (int64_t)entryMs;
    if (offsetMs < 0) offsetMs = 0;
    time_t actualTs = (time_t)((int64_t)unixNow - offsetMs / 1000);

    float temp     = e["temp"]     | NAN;
    float humidity = e["humidity"] | NAN;
    float batPct   = e["battery_pct"].isNull()     ? NAN : (float)e["battery_pct"];
    float batV     = e["battery_voltage"].isNull() ? NAN : (float)e["battery_voltage"];

    if (postInflux(buildInfluxBody(temp, humidity, actualTs, batPct, batV))) {
      DBGF("Flushed entry at %ld\n", (long)actualTs);
    } else {
      DBGLN("Flush failed");
      JsonObject keep = failed.add<JsonObject>();
      keep["temp"]     = temp;
      keep["humidity"] = humidity;
      if (isnan(batPct)) {
        keep["battery_pct"]     = nullptr;
        keep["battery_voltage"] = nullptr;
      } else {
        keep["battery_pct"]     = batPct;
        keep["battery_voltage"] = batV;
      }
      keep["monotonic_ms"] = entryMs;
    }
  }

  saveBuffer(failedDoc);
  if (failed.size() == 0) {
    DBGLN("Buffer fully flushed and cleared");
    return true;
  }
  DBGF("%d entries remain in buffer\n", (int)failed.size());
  return false;
}

// =========================================================
// NTFY
// =========================================================

static void sendAlert(const char* title, const String& message, const char* priority) {
  String url = String(NTFY_HOST) + "/" + NTFY_TOPIC;
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(url)) {
    logError("ntfy error: http.begin failed");
    return;
  }
  http.addHeader("Title", title);
  http.addHeader("Priority", priority);
  int code = http.POST(message);
  http.end();
  DBGF("ntfy: %d\n", code);
  if (code < 0) {
    logError(String("ntfy error: ") + HTTPClient::errorToString(code));
  }
}

// =========================================================
// LEDS (common-cathode dual LED, drive HIGH to light)
// =========================================================

static void setupLeds() {
  pinMode(A0, OUTPUT);
  pinMode(A1, OUTPUT);
  digitalWrite(A0, LOW);
  digitalWrite(A1, LOW);
}

static void showLedStatus(float humidity) {
  digitalWrite(A0, LOW);
  digitalWrite(A1, LOW);
  if (humidity >= LEVEL_WATCH) {
    digitalWrite(A0, HIGH); // red
  } else {
    digitalWrite(A1, HIGH); // green
  }
  delay(LED_ON_SECONDS * 1000UL);
  digitalWrite(A0, LOW);
  digitalWrite(A1, LOW);
}

// =========================================================
// ADAPTIVE INTERVAL
// =========================================================

static uint32_t getMaxInterval(float humidity) {
  if (humidity >= LEVEL_CRITICAL) return MAX_INTERVAL_CRITICAL;
  if (humidity >= LEVEL_WARNING)  return MAX_INTERVAL_WARNING;
  if (humidity >= LEVEL_WATCH)    return MAX_INTERVAL_WATCH;
  return MAX_INTERVAL_BELOW_WATCH;
}

static float getThreshold(float humidity) {
  if (humidity < LEVEL_WATCH)    return 2.0f;
  if (humidity < LEVEL_WARNING)  return 1.0f;
  if (humidity < LEVEL_CRITICAL) return 0.5f;
  return 0.0f;
}

static int ladderIndex(uint32_t interval) {
  for (size_t i = 0; i < INTERVAL_LADDER_LEN; i++) {
    if (INTERVAL_LADDER[i] == interval) return (int)i;
  }
  return -1;
}

static uint32_t updateInterval(JsonDocument& state, float humidity, bool significantChange) {
  uint32_t maxInterval = getMaxInterval(humidity);

  if (significantChange || humidity >= LEVEL_CRITICAL) {
    state["current_interval"] = INTERVAL_LADDER[0];
    state["consecutive_stable"] = 0;
  } else {
    int stable = (int)(state["consecutive_stable"] | 0) + 1;
    state["consecutive_stable"] = stable;
    if (stable >= STABLE_READINGS_TO_STEP_UP) {
      uint32_t current = state["current_interval"] | INTERVAL_LADDER[0];
      int idx = ladderIndex(current);
      if (idx >= 0 && idx < (int)INTERVAL_LADDER_LEN - 1) {
        state["current_interval"] = INTERVAL_LADDER[idx + 1];
      }
      state["consecutive_stable"] = 0;
    }
  }

  uint32_t cur = state["current_interval"] | INTERVAL_LADDER[0];
  if (cur > maxInterval) {
    state["current_interval"] = maxInterval;
    cur = maxInterval;
  }
  return cur;
}

// =========================================================
// HUMIDITY ALERTS
// =========================================================

static void handleHumidityAlerts(float humidity, time_t now, JsonDocument& state) {
  const char* level = nullptr;
  uint32_t alertInterval = 0;

  if (humidity >= LEVEL_CRITICAL)      { level = "critical"; alertInterval = ALERT_INTERVAL_CRITICAL; }
  else if (humidity >= LEVEL_WARNING)  { level = "warning";  alertInterval = ALERT_INTERVAL_WARNING; }
  else if (humidity >= LEVEL_WATCH)    { level = "watch";    alertInterval = ALERT_INTERVAL_WATCH; }

  if (level == nullptr) {
    state["alert_level"]       = nullptr;
    state["last_alert_time"]   = nullptr;
    state["watch_alert_sent"]  = false;
    return;
  }

  bool hasLast = !state["last_alert_time"].isNull();
  time_t lastAlertTime = hasLast ? (time_t)(uint32_t)state["last_alert_time"] : 0;
  bool watchSent = state["watch_alert_sent"] | false;

  bool shouldAlert = false;
  if (strcmp(level, "watch") == 0) {
    if (!watchSent) {
      shouldAlert = true;
      state["watch_alert_sent"] = true;
    } else if (hasLast && (uint32_t)(now - lastAlertTime) >= alertInterval) {
      shouldAlert = true;
    } else if (!hasLast) {
      shouldAlert = true;
    }
  } else {
    if (!hasLast || (uint32_t)(now - lastAlertTime) >= alertInterval) {
      shouldAlert = true;
    }
  }

  if (!shouldAlert) return;

  char msg[160];
  if (strcmp(level, "critical") == 0) {
    snprintf(msg, sizeof(msg),
             "Humidity is %.1f%% — above 20%% limit! Replace desiccant immediately.", humidity);
    sendAlert("\xF0\x9F\x9A\xA8 Humidity Critical!", msg, "urgent");
  } else if (strcmp(level, "warning") == 0) {
    snprintf(msg, sizeof(msg),
             "Humidity is %.1f%% — approaching limit. Replace desiccant soon.", humidity);
    sendAlert("\xE2\x9A\xA0\xEF\xB8\x8F Humidity Warning", msg, "high");
  } else {
    snprintf(msg, sizeof(msg),
             "Humidity is %.1f%% — monitor closely.", humidity);
    sendAlert("\xF0\x9F\x91\x80 Humidity Rising", msg, "default");
  }
  state["last_alert_time"] = (uint32_t)now;
  state["alert_level"]     = level;
}

// =========================================================
// BATTERY ALERTS
// =========================================================

static void handleBatteryAlerts(float batPct, time_t now, JsonDocument& state) {
  const char* level = nullptr;
  uint32_t alertInterval = 0;

  if (batPct <= BATTERY_EMERGENCY)     { level = "emergency"; alertInterval = BATTERY_ALERT_INTERVAL_EMERGENCY; }
  else if (batPct <= BATTERY_CRITICAL) { level = "critical";  alertInterval = BATTERY_ALERT_INTERVAL_CRITICAL; }
  else if (batPct <= BATTERY_WARNING)  { level = "warning";   alertInterval = 0; }

  if (level == nullptr) {
    state["battery_warning_sent"]    = false;
    state["last_battery_alert_time"] = nullptr;
    state["battery_alert_level"]     = nullptr;
    return;
  }

  bool hasLast = !state["last_battery_alert_time"].isNull();
  time_t lastAlertTime = hasLast ? (time_t)(uint32_t)state["last_battery_alert_time"] : 0;
  bool warningSent = state["battery_warning_sent"] | false;

  bool shouldAlert = false;
  if (strcmp(level, "warning") == 0) {
    if (!warningSent) {
      shouldAlert = true;
      state["battery_warning_sent"] = true;
    }
  } else {
    if (!hasLast || (uint32_t)(now - lastAlertTime) >= alertInterval) {
      shouldAlert = true;
    }
  }

  if (!shouldAlert) return;

  char msg[160];
  if (strcmp(level, "emergency") == 0) {
    snprintf(msg, sizeof(msg),
             "Battery at %.0f%% — device will shut down soon. Charge immediately.", batPct);
    sendAlert("\xF0\x9F\x94\x8B Battery Emergency!", msg, "urgent");
  } else if (strcmp(level, "critical") == 0) {
    snprintf(msg, sizeof(msg),
             "Battery at %.0f%% — please charge soon.", batPct);
    sendAlert("\xF0\x9F\x94\x8B Battery Low", msg, "high");
  } else {
    snprintf(msg, sizeof(msg),
             "Battery at %.0f%% — consider charging.", batPct);
    sendAlert("\xF0\x9F\x94\x8B Battery Warning", msg, "default");
  }
  state["last_battery_alert_time"] = (uint32_t)now;
  state["battery_alert_level"]     = level;
}

// =========================================================
// WIFI / NTP
// =========================================================

static bool connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  DBG("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      DBGLN(" timeout");
      logError("WiFi connection failed: timeout");
      return false;
    }
    delay(200);
    DBG('.');
    esp_task_wdt_reset();
  }
  DBG(" connected, IP: ");
  DBGLN(WiFi.localIP());
  return true;
}

static bool syncTime(time_t& unixNow, uint64_t& monoAtSyncMs) {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t start = millis();
  while (true) {
    time_t t = time(nullptr);
    if (t > NTP_VALID_AFTER) {
      unixNow = t;
      monoAtSyncMs = monotonicMs();
      DBGF("Time synced (UTC): %ld\n", (long)t);
      return true;
    }
    if (millis() - start > NTP_SYNC_TIMEOUT_MS) {
      logError("NTP sync failed: timeout");
      return false;
    }
    delay(250);
    esp_task_wdt_reset();
  }
}

// =========================================================
// DEEP SLEEP
// =========================================================

static void enterDeepSleep(uint32_t seconds) {
  DBGF("Entering deep sleep for %u min...\n", (unsigned)(seconds / 60));
#ifdef DEBUG
  Serial.flush();
#endif
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  digitalWrite(NEOPIXEL_I2C_POWER, LOW); // power down Stemma QT bus during sleep

  // Advance the monotonic accumulator past this boot's runtime + the
  // upcoming sleep duration so monotonicMs() stays continuous on wake.
  persistentUs += (uint64_t)esp_timer_get_time() + (uint64_t)seconds * 1000000ULL;

  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_enable_gpio_wakeup(BIT(A4), ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

// =========================================================
// SETUP / LOOP
// =========================================================

void setup() {
  DBG_BEGIN();
  btStop();

  pinMode(NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_I2C_POWER, HIGH);
  delay(10);

  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms    = WDT_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_init(&wdtCfg);
  esp_task_wdt_add(nullptr);
  esp_task_wdt_reset();

  bootCount++;
  DBGF("\n=== Boot #%u ===\n", (unsigned)bootCount);

  if (!LittleFS.begin(true)) {
    DBGLN("LittleFS mount failed");
  }

  JsonDocument state;
  loadState(state);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool buttonWake = (cause == ESP_SLEEP_WAKEUP_GPIO);
  DBGF("Wake reason: %s (cause=%d)\n", buttonWake ? "button press" : "timer/cold", (int)cause);

  Wire.begin();
  if (!aht.begin()) {
    logError("AHT20 init failed");
  }
  if (!lipo.begin()) {
    logError("MAX17048 init failed");
  }

  sensors_event_t humEvt, tempEvt;
  aht.getEvent(&humEvt, &tempEvt);
  float temp     = tempEvt.temperature;
  float humidity = humEvt.relative_humidity;
  uint64_t monoNow = monotonicMs();
  DBGF("Temp: %.1fC  Humidity: %.1f%%\n", temp, humidity);
  esp_task_wdt_reset();

  // Button wake — show LED and sleep, no WiFi or logging
  if (buttonWake) {
    setupLeds();
    DBGLN("Showing LED status...");
    showLedStatus(humidity);
    uint32_t s = state["current_interval"] | INTERVAL_LADDER[0];
    enterDeepSleep(s);
    return;
  }

  // Timer wake — full cycle
  bool wifiOk = connectWifi();
  esp_task_wdt_reset();

  time_t unixNow = 0;
  uint64_t monoAtSyncMs = 0;
  bool timeOk = false;
  if (wifiOk) {
    timeOk = syncTime(unixNow, monoAtSyncMs);
    if (!timeOk) wifiOk = false;
  }
  esp_task_wdt_reset();

  // Battery check (once per day)
  float batPct = NAN, batV = NAN;
  bool didBatteryCheck = false;
  bool needsBatCheck = false;
  if (state["last_battery_check"].isNull()) {
    needsBatCheck = true;
  } else if (timeOk) {
    time_t last = (time_t)(uint32_t)state["last_battery_check"];
    if ((uint32_t)(unixNow - last) >= BATTERY_CHECK_INTERVAL) needsBatCheck = true;
  }
  if (needsBatCheck) {
    batPct = lipo.getSOC();
    batV   = lipo.getVoltage();
    DBGF("Battery: %.0f%%  Voltage: %.2fV\n", batPct, batV);
    didBatteryCheck = true;
    if (timeOk) state["last_battery_check"] = (uint32_t)unixNow;
  }

  // Decide whether to log
  bool hasLastHumidity = !state["last_humidity"].isNull();
  float lastHumidity = hasLastHumidity ? (float)state["last_humidity"] : 0.0f;
  bool hasLastLog = !state["last_log_time"].isNull();
  time_t lastLogTime = hasLastLog ? (time_t)(uint32_t)state["last_log_time"] : 0;
  float threshold = getThreshold(humidity);

  bool significantChange = false;
  bool shouldLog = false;

  if (!hasLastHumidity) {
    shouldLog = true;
    DBGLN("First reading — logging");
  } else if (threshold == 0.0f) {
    shouldLog = true;
    significantChange = true;
    DBGLN("Above critical — logging");
  } else if (fabsf(humidity - lastHumidity) >= threshold) {
    shouldLog = true;
    significantChange = true;
    DBGF("Delta %.1f%% >= %.1f%% — logging\n", fabsf(humidity - lastHumidity), threshold);
  } else if (hasLastLog && timeOk && (uint32_t)(unixNow - lastLogTime) >= 86400UL) {
    shouldLog = true;
    DBGLN("Daily heartbeat — logging");
  } else {
    DBGLN("No significant change — skipping");
  }
  esp_task_wdt_reset();

  if (shouldLog || didBatteryCheck) {
    if (wifiOk && timeOk) {
      flushBuffer(unixNow, monoAtSyncMs);
      bool ok = logToInflux(temp, humidity, unixNow, batPct, batV);
      if (ok) {
        state["last_humidity"] = humidity;
        state["last_log_time"] = (uint32_t)unixNow;
      } else {
        logError("InfluxDB write failed, buffering reading");
        addToBuffer(temp, humidity, batPct, batV, monoNow);
      }
    } else {
      DBGLN("Offline — buffering reading");
      addToBuffer(temp, humidity, batPct, batV, monoNow);
      state["last_humidity"] = humidity;
    }
  }
  esp_task_wdt_reset();

  // Adaptive interval
  uint32_t sleepSeconds = updateInterval(state, humidity, significantChange);
  DBGF("Next check in %u min (%d stable)\n",
       (unsigned)(sleepSeconds / 60),
       (int)(state["consecutive_stable"] | 0));

  // Alerts (only if online)
  if (wifiOk && timeOk) {
    handleHumidityAlerts(humidity, unixNow, state);
    if (didBatteryCheck && !isnan(batPct)) {
      handleBatteryAlerts(batPct, unixNow, state);
    }
  }
  esp_task_wdt_reset();

  saveState(state);
  enterDeepSleep(sleepSeconds);
}

void loop() {
  // unused — every setup() path ends in deep sleep
}

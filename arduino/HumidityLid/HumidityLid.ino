#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Adafruit_AHTX0.h>
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>
#include <esp_sleep.h>
#include <esp_bt.h>
#include <time.h>
#include <math.h>

#include "config.h"

// =========================================================
// CONSTANTS
// =========================================================

static const char* BUFFER_FILE = "/buf.bin";
static const char* ERROR_LOG   = "/error.log";

static const uint32_t LED_ON_SECONDS      = 5;
static const size_t   MAX_ERROR_LOG_BYTES = 10240;

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

// =========================================================
// RTC STATE
// =========================================================

struct State {
  float    last_humidity;
  uint32_t last_log_time;       // 0 = never logged
  uint32_t last_alert_time;     // 0 = never alerted
  uint32_t last_bat_alert_time; // 0 = never alerted
  uint32_t current_interval;
  uint8_t  consecutive_stable;
  uint8_t  hum_alert_level;    // 0=none 1=watch 2=warning 3=critical
  uint8_t  bat_alert_level;    // 0=none 1=warning 2=critical 3=emergency
  bool     watch_alert_sent;
  bool     bat_warning_sent;
  bool     has_humidity;        // last_humidity is valid
};

RTC_DATA_ATTR State    g_state      = {};
RTC_DATA_ATTR bool     g_valid      = false;
RTC_DATA_ATTR uint32_t bootCount    = 0;
RTC_DATA_ATTR uint64_t persistentUs = 0;

// =========================================================
// BINARY BUFFER
// =========================================================

struct Reading {
  float    temp, humidity, batPct, batV;
  uint64_t monoMs;
}; // 24 bytes

// =========================================================
// GLOBALS
// =========================================================

Adafruit_AHTX0 aht;
SFE_MAX1704X   lipo(MAX1704X_MAX17048);

// =========================================================
// MONOTONIC CLOCK
// =========================================================

static uint64_t monotonicMs() {
  return (persistentUs + (uint64_t)esp_timer_get_time()) / 1000ULL;
}

// =========================================================
// ERROR LOG
// =========================================================

static void logError(const String& msg) {
  Serial.println("ERROR: " + msg);
  if (LittleFS.exists(ERROR_LOG)) {
    File f = LittleFS.open(ERROR_LOG, "r");
    bool tooBig = f && f.size() > MAX_ERROR_LOG_BYTES;
    if (f) f.close();
    if (tooBig) LittleFS.remove(ERROR_LOG);
  }
  File f = LittleFS.open(ERROR_LOG, "a");
  if (!f) return;
  char ts[24];
  time_t now = time(nullptr);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
  f.printf("[%s] %s\n", ts, msg.c_str());
  f.close();
}

// =========================================================
// BUFFER
// =========================================================

static void addToBuffer(float temp, float humidity, float batPct, float batV, uint64_t monoMs) {
  File f = LittleFS.open(BUFFER_FILE, "a");
  if (!f) { logError("buffer open failed"); return; }
  Reading r = {temp, humidity, batPct, batV, monoMs};
  f.write((uint8_t*)&r, sizeof(r));
  f.close();
}

// =========================================================
// INFLUXDB
// =========================================================

static String buildInfluxBody(float temp, float humidity, time_t ts, float batPct, float batV) {
  char tmp[128];
  snprintf(tmp, sizeof(tmp),
           "environment,location=bucket temperature=%.2f,humidity=%.2f,battery_pct=%.1f,battery_voltage=%.2f %lu",
           temp, humidity, batPct, batV, (unsigned long)ts);
  return String(tmp);
}

static bool postInflux(const String& body) {
  String url = String(INFLUXDB_HOST)
             + "/api/v2/write?org=" + INFLUXDB_ORG
             + "&bucket=" + INFLUXDB_BUCKET
             + "&precision=s";
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(url)) { logError("InfluxDB: http.begin failed"); return false; }
  http.addHeader("Authorization", String("Token ") + INFLUXDB_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  int code = http.POST(body);
  http.end();
  if (code < 0) { logError(String("InfluxDB error: ") + HTTPClient::errorToString(code)); return false; }
  return code == 204;
}

static bool flushBuffer(time_t unixNow, uint64_t monoAtSyncMs) {
  File f = LittleFS.open(BUFFER_FILE, "r");
  if (!f) return true;
  size_t n = f.size() / sizeof(Reading);
  if (n == 0) { f.close(); return true; }
  Serial.printf("Flushing %u buffered readings\n", (unsigned)n);

  Reading* readings = new Reading[n];
  f.read((uint8_t*)readings, n * sizeof(Reading));
  f.close();

  size_t failures = 0;
  for (size_t i = 0; i < n; i++) {
    int64_t offMs = (int64_t)monoAtSyncMs - (int64_t)readings[i].monoMs;
    if (offMs < 0) offMs = 0;
    time_t ts = (time_t)((int64_t)unixNow - offMs / 1000LL);
    if (!postInflux(buildInfluxBody(readings[i].temp, readings[i].humidity, ts, readings[i].batPct, readings[i].batV)))
      readings[failures++] = readings[i];
  }

  if (failures == 0) {
    LittleFS.remove(BUFFER_FILE);
  } else {
    File fw = LittleFS.open(BUFFER_FILE, "w");
    if (fw) { fw.write((uint8_t*)readings, failures * sizeof(Reading)); fw.close(); }
  }
  delete[] readings;
  return failures == 0;
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
  if (!http.begin(url)) { logError("ntfy: http.begin failed"); return; }
  http.addHeader("Title", title);
  http.addHeader("Priority", priority);
  int code = http.POST(message);
  http.end();
  if (code < 0) logError(String("ntfy error: ") + HTTPClient::errorToString(code));
}

// =========================================================
// LEDS
// =========================================================

static void setupLeds() {
  pinMode(A0, OUTPUT);
  pinMode(A1, OUTPUT);
  digitalWrite(A0, LOW);
  digitalWrite(A1, LOW);
}

static void showLedStatus(float humidity) {
  if (humidity >= LEVEL_WATCH) digitalWrite(A0, HIGH);
  else                         digitalWrite(A1, HIGH);
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
  for (size_t i = 0; i < INTERVAL_LADDER_LEN; i++)
    if (INTERVAL_LADDER[i] == interval) return (int)i;
  return -1;
}

static uint32_t updateInterval(float humidity, bool significantChange) {
  if (significantChange || humidity >= LEVEL_CRITICAL) {
    g_state.current_interval   = INTERVAL_LADDER[0];
    g_state.consecutive_stable = 0;
  } else {
    int stable = (int)g_state.consecutive_stable + 1;
    g_state.consecutive_stable = (uint8_t)stable;
    if (stable >= STABLE_READINGS_TO_STEP_UP) {
      int idx = ladderIndex(g_state.current_interval);
      if (idx >= 0 && idx < (int)INTERVAL_LADDER_LEN - 1)
        g_state.current_interval = INTERVAL_LADDER[idx + 1];
      g_state.consecutive_stable = 0;
    }
  }
  uint32_t maxInterval = getMaxInterval(humidity);
  if (g_state.current_interval > maxInterval)
    g_state.current_interval = maxInterval;
  return g_state.current_interval;
}

// =========================================================
// ALERTS
// =========================================================

static bool isAlertDue(uint32_t lastTime, uint32_t interval, time_t now) {
  return lastTime == 0 || (uint32_t)((uint32_t)now - lastTime) >= interval;
}

static void handleHumidityAlerts(float hum, time_t now) {
  if (hum < LEVEL_WATCH) {
    g_state.hum_alert_level  = 0;
    g_state.last_alert_time  = 0;
    g_state.watch_alert_sent = false;
    return;
  }

  char msg[160];
  const char *title, *priority;

  if (hum >= LEVEL_CRITICAL) {
    if (!isAlertDue(g_state.last_alert_time, ALERT_INTERVAL_CRITICAL, now)) return;
    snprintf(msg, sizeof(msg), "Humidity is %.1f%% — above 20%% limit! Replace desiccant immediately.", hum);
    title = "\xF0\x9F\x9A\xA8 Humidity Critical!"; priority = "urgent";
    g_state.hum_alert_level = 3;
  } else if (hum >= LEVEL_WARNING) {
    if (!isAlertDue(g_state.last_alert_time, ALERT_INTERVAL_WARNING, now)) return;
    snprintf(msg, sizeof(msg), "Humidity is %.1f%% — approaching limit. Replace desiccant soon.", hum);
    title = "\xE2\x9A\xA0\xEF\xB8\x8F Humidity Warning"; priority = "high";
    g_state.hum_alert_level = 2;
  } else {
    if (g_state.watch_alert_sent && !isAlertDue(g_state.last_alert_time, ALERT_INTERVAL_WATCH, now)) return;
    snprintf(msg, sizeof(msg), "Humidity is %.1f%% — monitor closely.", hum);
    title = "\xF0\x9F\x91\x80 Humidity Rising"; priority = "default";
    g_state.hum_alert_level  = 1;
    g_state.watch_alert_sent = true;
  }

  sendAlert(title, msg, priority);
  g_state.last_alert_time = (uint32_t)now;
}

static void handleBatteryAlerts(float batPct, time_t now) {
  if (batPct > BATTERY_WARNING) {
    g_state.bat_alert_level     = 0;
    g_state.last_bat_alert_time = 0;
    g_state.bat_warning_sent    = false;
    return;
  }

  char msg[160];
  const char *title, *priority;

  if (batPct <= BATTERY_EMERGENCY) {
    if (!isAlertDue(g_state.last_bat_alert_time, BATTERY_ALERT_INTERVAL_EMERGENCY, now)) return;
    snprintf(msg, sizeof(msg), "Battery at %.0f%% — device will shut down soon. Charge immediately.", batPct);
    title = "\xF0\x9F\x94\x8B Battery Emergency!"; priority = "urgent";
    g_state.bat_alert_level = 3;
  } else if (batPct <= BATTERY_CRITICAL) {
    if (!isAlertDue(g_state.last_bat_alert_time, BATTERY_ALERT_INTERVAL_CRITICAL, now)) return;
    snprintf(msg, sizeof(msg), "Battery at %.0f%% — please charge soon.", batPct);
    title = "\xF0\x9F\x94\x8B Battery Low"; priority = "high";
    g_state.bat_alert_level = 2;
  } else {
    if (g_state.bat_warning_sent) return;
    snprintf(msg, sizeof(msg), "Battery at %.0f%% — consider charging.", batPct);
    title = "\xF0\x9F\x94\x8B Battery Warning"; priority = "default";
    g_state.bat_alert_level  = 1;
    g_state.bat_warning_sent = true;
  }

  sendAlert(title, msg, priority);
  g_state.last_bat_alert_time = (uint32_t)now;
}

// =========================================================
// WIFI / NTP
// =========================================================

static bool connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      logError("WiFi connection failed: timeout");
      return false;
    }
    delay(200);
  }
  return true;
}

static bool syncTime(time_t& unixNow, uint64_t& monoAtSyncMs) {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t start = millis();
  while (true) {
    time_t t = time(nullptr);
    if (t > NTP_VALID_AFTER) {
      unixNow      = t;
      monoAtSyncMs = monotonicMs();
      return true;
    }
    if (millis() - start > NTP_SYNC_TIMEOUT_MS) {
      logError("NTP sync failed: timeout");
      return false;
    }
    delay(250);
  }
}

// =========================================================
// DEEP SLEEP
// =========================================================

static void enterDeepSleep(uint32_t seconds) {
  Serial.flush();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  digitalWrite(NEOPIXEL_I2C_POWER, LOW);
  persistentUs += (uint64_t)esp_timer_get_time() + (uint64_t)seconds * 1000000ULL;
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_enable_gpio_wakeup(BIT(A4), ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

// =========================================================
// SETUP / LOOP
// =========================================================

void setup() {
  Serial.begin(115200);
  btStop();

  pinMode(NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_I2C_POWER, HIGH);
  delay(100);

  bootCount++;
  if (!g_valid) {
    g_state                  = {};
    g_state.current_interval = INTERVAL_LADDER[0];
    g_valid                  = true;
  }

  if (!LittleFS.begin(true)) logError("LittleFS mount failed");

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool buttonWake = (cause == ESP_SLEEP_WAKEUP_GPIO);

  Wire.begin();
  delay(100);

  if (!aht.begin())  logError("AHT20 init failed");
  if (!lipo.begin()) logError("MAX17048 init failed");

  sensors_event_t humEvt, tempEvt;
  aht.getEvent(&humEvt, &tempEvt);
  float    temp     = tempEvt.temperature;
  float    humidity = humEvt.relative_humidity;
  uint64_t monoNow  = monotonicMs();

  float batPct = lipo.getSOC();
  float batV   = lipo.getVoltage();

  if (buttonWake) {
    setupLeds();
    showLedStatus(humidity);
    enterDeepSleep(g_state.current_interval);
    return;
  }

  bool wifiOk = connectWifi();

  time_t   unixNow      = 0;
  uint64_t monoAtSyncMs = 0;
  bool     timeOk       = false;
  if (wifiOk) {
    timeOk = syncTime(unixNow, monoAtSyncMs);
    if (!timeOk) wifiOk = false;
  }

  float threshold        = getThreshold(humidity);
  bool  significantChange = false;
  bool  shouldLog         = false;

  if (!g_state.has_humidity) {
    shouldLog = true;
  } else if (threshold == 0.0f) {
    shouldLog = true; significantChange = true;
  } else if (fabsf(humidity - g_state.last_humidity) >= threshold) {
    shouldLog = true; significantChange = true;
  } else if (g_state.last_log_time && timeOk && (uint32_t)((uint32_t)unixNow - g_state.last_log_time) >= 86400UL) {
    shouldLog = true;
  }

  bool logged = false;
  if (shouldLog) {
    if (wifiOk && timeOk) {
      flushBuffer(unixNow, monoAtSyncMs);
      bool ok = postInflux(buildInfluxBody(temp, humidity, unixNow, batPct, batV));
      if (ok) {
        logged                 = true;
        g_state.last_humidity  = humidity;
        g_state.has_humidity   = true;
        g_state.last_log_time  = (uint32_t)unixNow;
      } else {
        logError("InfluxDB write failed, buffering reading");
        addToBuffer(temp, humidity, batPct, batV, monoNow);
      }
    } else {
      addToBuffer(temp, humidity, batPct, batV, monoNow);
      g_state.last_humidity = humidity;
      g_state.has_humidity  = true;
    }
  }

  uint32_t sleepSeconds = updateInterval(humidity, significantChange);

  if (wifiOk && timeOk) {
    handleHumidityAlerts(humidity, unixNow);
    handleBatteryAlerts(batPct, unixNow);
  }

  Serial.printf("Boot #%u | %.1fC | Hum: %.1f%% | Bat: %.0f%% %.2fV | WiFi: %s | Logged: %s | Sleep: %umin\n",
    bootCount, temp, humidity, batPct, batV,
    wifiOk ? "ok" : "fail",
    logged  ? "yes" : "no",
    (unsigned)(sleepSeconds / 60));

  enterDeepSleep(sleepSeconds);
}

void loop() {}
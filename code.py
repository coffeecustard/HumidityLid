import time
import json
import os
import board
import digitalio
import adafruit_ahtx0
import adafruit_max1704x
import wifi
import socketpool
import ssl
import adafruit_requests
import adafruit_ntp
import rtc
import alarm
from microcontroller import watchdog as w

# =========================================================
# CONFIGURATION
# =========================================================

WIFI_SSID = os.getenv("WIFI_SSID")
WIFI_PASSWORD = os.getenv("WIFI_PASSWORD")
TIMEZONE_OFFSET = int(os.getenv("TIMEZONE_OFFSET", -4))

INFLUXDB_HOST = os.getenv("INFLUXDB_HOST")
INFLUXDB_TOKEN = os.getenv("INFLUXDB_TOKEN")
INFLUXDB_ORG = os.getenv("INFLUXDB_ORG")
INFLUXDB_BUCKET = os.getenv("INFLUXDB_BUCKET")

NTFY_HOST = os.getenv("NTFY_HOST")
NTFY_TOPIC = os.getenv("NTFY_TOPIC")

STATE_FILE = "/state.json"
BUFFER_FILE = "/buffer.json"
ERROR_LOG = "/error.log"
BATTERY_CHECK_INTERVAL = 86400
LED_ON_SECONDS = 5
MAX_ERROR_LOG_BYTES = 10240

# Humidity alert thresholds
LEVEL_WATCH = 15.0
LEVEL_WARNING = 18.0
LEVEL_CRITICAL = 20.0

# Humidity alert intervals
ALERT_INTERVALS = {
    "watch":    7 * 24 * 3600,
    "warning":  24 * 3600,
    "critical": 3600,
}

# Battery alert thresholds
BATTERY_WARNING = 30.0
BATTERY_CRITICAL = 15.0
BATTERY_EMERGENCY = 5.0

# Battery alert intervals
BATTERY_ALERT_INTERVALS = {
    "warning":   None,
    "critical":  86400,
    "emergency": 1800,
}

# Adaptive interval ladder
INTERVAL_LADDER = [1800, 3600, 7200, 21600, 43200]
STABLE_READINGS_TO_STEP_UP = 3

# Max interval per humidity zone
MAX_INTERVAL_BELOW_WATCH = 43200
MAX_INTERVAL_WATCH = 21600
MAX_INTERVAL_WARNING = 3600
MAX_INTERVAL_CRITICAL = 1800

# =========================================================
# ERROR LOGGING
# =========================================================

def log_error(message):
    try:
        try:
            import os as _os
            size = _os.stat(ERROR_LOG)[6]
            if size > MAX_ERROR_LOG_BYTES:
                _os.remove(ERROR_LOG)
        except Exception:
            pass
        with open(ERROR_LOG, "a") as f:
            t = time.localtime()
            timestamp = f"{t.tm_year}-{t.tm_mon:02d}-{t.tm_mday:02d} {t.tm_hour:02d}:{t.tm_min:02d}:{t.tm_sec:02d}"
            f.write(f"[{timestamp}] {message}\n")
    except Exception as e:
        print(f"Could not write to error log: {e}")

# =========================================================
# LOCAL BUFFER
# =========================================================

def load_buffer():
    try:
        with open(BUFFER_FILE, "r") as f:
            return json.load(f)
    except Exception:
        return []

def save_buffer(buffer):
    try:
        with open(BUFFER_FILE, "w") as f:
            json.dump(buffer, f)
    except Exception as e:
        log_error(f"Could not save buffer: {e}")

def add_to_buffer(temp, humidity, battery_pct, battery_voltage, monotonic):
    buffer = load_buffer()
    buffer.append({
        "temp": temp,
        "humidity": humidity,
        "battery_pct": battery_pct,
        "battery_voltage": battery_voltage,
        "monotonic": monotonic
    })
    save_buffer(buffer)
    print(f"Buffered reading — {len(buffer)} entries pending")

def flush_buffer(session, ntp_time, monotonic_at_sync):
    buffer = load_buffer()
    if not buffer:
        return True

    print(f"Flushing {len(buffer)} buffered readings...")
    failed = []

    for entry in buffer:
        w.feed()
        monotonic_offset = monotonic_at_sync - entry.get("monotonic", monotonic_at_sync)
        actual_timestamp = int(ntp_time - monotonic_offset)

        fields = f"temperature={entry['temp']:.2f},humidity={entry['humidity']:.2f}"
        if entry.get("battery_pct") is not None:
            fields += f",battery_pct={entry['battery_pct']:.1f},battery_voltage={entry['battery_voltage']:.2f}"

        data = f"environment,location=bucket {fields} {actual_timestamp}"
        headers = {
            "Authorization": f"Token {INFLUXDB_TOKEN}",
            "Content-Type": "text/plain; charset=utf-8"
        }
        try:
            response = session.post(
                f"{INFLUXDB_HOST}/api/v2/write?org={INFLUXDB_ORG}&bucket={INFLUXDB_BUCKET}&precision=s",
                headers=headers,
                data=data
            )
            if response.status_code == 204:
                print(f"Flushed entry at {actual_timestamp}")
            else:
                print(f"Flush failed: {response.status_code}")
                failed.append(entry)
            response.close()
        except Exception as e:
            log_error(f"Flush error: {e}")
            failed.append(entry)

    save_buffer(failed)
    if not failed:
        print("Buffer fully flushed and cleared")
        return True
    else:
        print(f"{len(failed)} entries remain in buffer")
        return False

# =========================================================
# LED SETUP
# =========================================================

def setup_leds():
    red = digitalio.DigitalInOut(board.A0)
    red.direction = digitalio.Direction.OUTPUT
    red.value = False

    green = digitalio.DigitalInOut(board.A1)
    green.direction = digitalio.Direction.OUTPUT
    green.value = False

    return red, green

def show_led_status(red, green, humidity):
    red.value = False
    green.value = False

    if humidity >= LEVEL_WATCH:
        red.value = True
    else:
        green.value = True

    time.sleep(LED_ON_SECONDS)

    red.value = False
    green.value = False

def leds_off(red, green):
    red.value = False
    green.value = False

# =========================================================
# STATE
# =========================================================

def load_state():
    try:
        with open(STATE_FILE, "r") as f:
            state = json.load(f)
            print("State loaded")
            return state
    except Exception:
        print("No state file, starting fresh")
        return {
            "last_humidity": None,
            "last_log_time": None,
            "last_alert_time": None,
            "alert_level": None,
            "watch_alert_sent": False,
            "consecutive_stable": 0,
            "current_interval": 1800,
            "last_battery_check": None,
            "battery_warning_sent": False,
            "last_battery_alert_time": None,
            "battery_alert_level": None
        }

def save_state(state):
    try:
        with open(STATE_FILE, "w") as f:
            json.dump(state, f)
    except Exception as e:
        log_error(f"Error saving state: {e}")

# =========================================================
# ADAPTIVE INTERVAL
# =========================================================

def get_max_interval(humidity):
    if humidity >= LEVEL_CRITICAL:
        return MAX_INTERVAL_CRITICAL
    elif humidity >= LEVEL_WARNING:
        return MAX_INTERVAL_WARNING
    elif humidity >= LEVEL_WATCH:
        return MAX_INTERVAL_WATCH
    else:
        return MAX_INTERVAL_BELOW_WATCH

def get_threshold(humidity):
    if humidity < LEVEL_WATCH:
        return 2.0
    elif humidity < LEVEL_WARNING:
        return 1.0
    elif humidity < LEVEL_CRITICAL:
        return 0.5
    else:
        return 0.0

def update_interval(state, humidity, significant_change):
    max_interval = get_max_interval(humidity)

    if significant_change or humidity >= LEVEL_CRITICAL:
        state["current_interval"] = INTERVAL_LADDER[0]
        state["consecutive_stable"] = 0
    else:
        state["consecutive_stable"] += 1
        if state["consecutive_stable"] >= STABLE_READINGS_TO_STEP_UP:
            current = state["current_interval"]
            if current in INTERVAL_LADDER:
                idx = INTERVAL_LADDER.index(current)
                if idx < len(INTERVAL_LADDER) - 1:
                    state["current_interval"] = INTERVAL_LADDER[idx + 1]
            state["consecutive_stable"] = 0

    state["current_interval"] = min(state["current_interval"], max_interval)
    return state["current_interval"]

# =========================================================
# INFLUXDB
# =========================================================

def log_to_influxdb(session, temp, humidity, timestamp, battery_pct=None, battery_voltage=None):
    fields = f"temperature={temp:.2f},humidity={humidity:.2f}"
    if battery_pct is not None:
        fields += f",battery_pct={battery_pct:.1f},battery_voltage={battery_voltage:.2f}"

    data = f"environment,location=bucket {fields} {timestamp}"
    headers = {
        "Authorization": f"Token {INFLUXDB_TOKEN}",
        "Content-Type": "text/plain; charset=utf-8"
    }
    try:
        response = session.post(
            f"{INFLUXDB_HOST}/api/v2/write?org={INFLUXDB_ORG}&bucket={INFLUXDB_BUCKET}&precision=s",
            headers=headers,
            data=data
        )
        print(f"InfluxDB: {response.status_code}")
        response.close()
        return response.status_code == 204
    except Exception as e:
        log_error(f"InfluxDB error: {e}")
        return False

# =========================================================
# NTFY ALERTS
# =========================================================

def send_alert(session, title, message, priority="default"):
    try:
        headers = {
            "Title": title,
            "Priority": priority
        }
        response = session.post(
            f"{NTFY_HOST}/{NTFY_TOPIC}",
            headers=headers,
            data=message
        )
        print(f"ntfy: {response.status_code}")
        response.close()
    except Exception as e:
        log_error(f"ntfy error: {e}")

# =========================================================
# HUMIDITY ALERTS
# =========================================================

def handle_humidity_alerts(session, humidity, now, state):
    level = None
    if humidity >= LEVEL_CRITICAL:
        level = "critical"
    elif humidity >= LEVEL_WARNING:
        level = "warning"
    elif humidity >= LEVEL_WATCH:
        level = "watch"

    if level is None:
        state["alert_level"] = None
        state["last_alert_time"] = None
        state["watch_alert_sent"] = False
        return

    last_alert_time = state.get("last_alert_time")
    alert_interval = ALERT_INTERVALS.get(level, ALERT_INTERVALS["watch"])
    time_since_alert = (now - last_alert_time) if last_alert_time else None

    should_alert = False

    if level == "watch" and not state.get("watch_alert_sent"):
        should_alert = True
        state["watch_alert_sent"] = True
    elif level in ("warning", "critical"):
        if time_since_alert is None or time_since_alert >= alert_interval:
            should_alert = True
    elif level == "watch":
        if time_since_alert is None or time_since_alert >= alert_interval:
            should_alert = True

    if should_alert:
        if level == "critical":
            send_alert(session, "🚨 Humidity Critical!",
                      f"Humidity is {humidity:.1f}% — above 20% limit! Replace desiccant immediately.",
                      priority="urgent")
        elif level == "warning":
            send_alert(session, "⚠️ Humidity Warning",
                      f"Humidity is {humidity:.1f}% — approaching limit. Replace desiccant soon.",
                      priority="high")
        elif level == "watch":
            send_alert(session, "👀 Humidity Rising",
                      f"Humidity is {humidity:.1f}% — monitor closely.",
                      priority="default")
        state["last_alert_time"] = now
        state["alert_level"] = level

# =========================================================
# BATTERY ALERTS
# =========================================================

def handle_battery_alerts(session, battery_pct, now, state):
    level = None
    if battery_pct <= BATTERY_EMERGENCY:
        level = "emergency"
    elif battery_pct <= BATTERY_CRITICAL:
        level = "critical"
    elif battery_pct <= BATTERY_WARNING:
        level = "warning"

    if level is None:
        state["battery_warning_sent"] = False
        state["last_battery_alert_time"] = None
        state["battery_alert_level"] = None
        return

    last_battery_alert_time = state.get("last_battery_alert_time")
    alert_interval = BATTERY_ALERT_INTERVALS.get(level)
    time_since_alert = (now - last_battery_alert_time) if last_battery_alert_time else None

    should_alert = False

    if level == "warning" and not state.get("battery_warning_sent"):
        should_alert = True
        state["battery_warning_sent"] = True
    elif level == "critical":
        if time_since_alert is None or time_since_alert >= alert_interval:
            should_alert = True
    elif level == "emergency":
        if time_since_alert is None or time_since_alert >= alert_interval:
            should_alert = True

    if should_alert:
        if level == "emergency":
            send_alert(session, "🔋 Battery Emergency!",
                      f"Battery at {battery_pct:.0f}% — device will shut down soon. Charge immediately.",
                      priority="urgent")
        elif level == "critical":
            send_alert(session, "🔋 Battery Low",
                      f"Battery at {battery_pct:.0f}% — please charge soon.",
                      priority="high")
        elif level == "warning":
            send_alert(session, "🔋 Battery Warning",
                      f"Battery at {battery_pct:.0f}% — consider charging.",
                      priority="default")
        state["last_battery_alert_time"] = now
        state["battery_alert_level"] = level

# =========================================================
# MAIN
# =========================================================

# Feed watchdog immediately on wake
w.feed()

state = load_state()

# Check what woke us up
button_wake = isinstance(alarm.wake_alarm, alarm.pin.PinAlarm)
print("Wake reason:", "button press" if button_wake else "timer")

# Initialize sensors
sensor = adafruit_ahtx0.AHTx0(board.I2C())
battery = adafruit_max1704x.MAX17048(board.I2C())
print("Sensors initialized")
w.feed()

# Read humidity always
temp = sensor.temperature
humidity = sensor.relative_humidity
monotonic_now = time.monotonic()
print(f"Temp: {temp:.1f}C  Humidity: {humidity:.1f}%")

# Handle button press — show LED, skip WiFi and logging
if button_wake:
    red, green = setup_leds()
    print("Showing LED status...")
    show_led_status(red, green, humidity)
    leds_off(red, green)
    print("LED done, going back to sleep")
    sleep_seconds = state.get("current_interval", 1800)
    time_alarm = alarm.time.TimeAlarm(monotonic_time=time.monotonic() + sleep_seconds)
    pin_alarm = alarm.pin.PinAlarm(pin=board.A4, value=False, pull=True)
    alarm.exit_and_deep_sleep_until_alarms(time_alarm, pin_alarm)

# Scheduled wake — connect to WiFi
wifi_connected = False
session = None

try:
    print("Connecting to WiFi...")
    wifi.radio.connect(WIFI_SSID, WIFI_PASSWORD)
    print(f"Connected! IP: {wifi.radio.ipv4_address}")
    wifi_connected = True
    pool = socketpool.SocketPool(wifi.radio)
    session = adafruit_requests.Session(pool, ssl.create_default_context())
except Exception as e:
    log_error(f"WiFi connection failed: {e}")
    print(f"WiFi failed: {e}")

w.feed()

# NTP sync
now = None
monotonic_at_sync = None

if wifi_connected:
    try:
        print("Syncing NTP...")
        ntp = adafruit_ntp.NTP(pool, tz_offset=TIMEZONE_OFFSET)
        rtc.RTC().datetime = ntp.datetime
        now = time.mktime(time.localtime())
        monotonic_at_sync = time.monotonic()
        print(f"Time synced: {time.localtime()}")
    except Exception as e:
        log_error(f"NTP sync failed: {e}")
        print(f"NTP failed: {e}")
        wifi_connected = False

w.feed()

# Read battery if due
battery_pct = None
battery_voltage = None
last_battery_check = state.get("last_battery_check")
needs_battery_check = (
    last_battery_check is None or
    (now is not None and (now - last_battery_check) >= BATTERY_CHECK_INTERVAL)
)

if needs_battery_check:
    battery_pct = battery.cell_percent
    battery_voltage = battery.cell_voltage
    print(f"Battery: {battery_pct:.0f}%  Voltage: {battery_voltage:.2f}V")
    if now:
        state["last_battery_check"] = now

# Determine if we should log
last_humidity = state.get("last_humidity")
last_log_time = state.get("last_log_time")
threshold = get_threshold(humidity)

significant_change = False
should_log = False

if last_humidity is None:
    should_log = True
    print("First reading — logging")
elif threshold == 0.0:
    should_log = True
    significant_change = True
    print("Above critical — logging")
elif abs(humidity - last_humidity) >= threshold:
    should_log = True
    significant_change = True
    print(f"Delta {abs(humidity - last_humidity):.1f}% >= {threshold}% — logging")
elif last_log_time and now and (now - last_log_time) >= 86400:
    should_log = True
    print("Daily heartbeat — logging")
else:
    print("No significant change — skipping")

w.feed()

if should_log or needs_battery_check:
    if wifi_connected and now:
        flush_buffer(session, now, monotonic_at_sync)
        success = log_to_influxdb(session, temp, humidity, now, battery_pct, battery_voltage)
        if success:
            state["last_humidity"] = humidity
            state["last_log_time"] = now
        else:
            log_error("InfluxDB write failed, buffering reading")
            add_to_buffer(temp, humidity, battery_pct, battery_voltage, monotonic_now)
    else:
        print("Offline — buffering reading")
        add_to_buffer(temp, humidity, battery_pct, battery_voltage, monotonic_now)
        state["last_humidity"] = humidity

w.feed()

# Update adaptive interval
sleep_seconds = update_interval(state, humidity, significant_change)
print(f"Next check in {sleep_seconds // 60} min ({state['consecutive_stable']} stable readings)")

# Handle alerts only if online
if wifi_connected and session and now:
    handle_humidity_alerts(session, humidity, now, state)
    if needs_battery_check and battery_pct is not None:
        handle_battery_alerts(session, battery_pct, now, state)

w.feed()

# Save state
save_state(state)

# Deep sleep
print(f"Entering deep sleep for {sleep_seconds // 60} minutes...")
time_alarm = alarm.time.TimeAlarm(monotonic_time=time.monotonic() + sleep_seconds)
pin_alarm = alarm.pin.PinAlarm(pin=board.A4, value=False, pull=True)
alarm.exit_and_deep_sleep_until_alarms(time_alarm, pin_alarm)
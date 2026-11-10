# Filament Humidity Monitor

A low-power ESP32-C6 sensor that monitors humidity inside a sealed filament storage bucket and alerts you when levels approach the 20% threshold that can damage 3D printing filament. It logs to a self-hosted InfluxDB and sends push alerts through a self-hosted ntfy server.

> **Firmware status:** the supported firmware is the **Arduino/C++ sketch in [`arduino/HumidityLid/`](arduino/HumidityLid/)**. The original [`circuitpython/`](circuitpython/) version is archived and not maintained. See [Why not CircuitPython?](#why-not-circuitpython).

## Components

| Component | Description |
|---|---|
| [Adafruit ESP32-C6 Feather - STEMMA QT](https://www.adafruit.com/product/5933) | Main microcontroller with WiFi 6, Bluetooth 5, and built-in LiPo charging and monitoring (MAX17048) |
| [Adafruit AHT20 - Temperature & Humidity Sensor Breakout Board - STEMMA QT](https://www.adafruit.com/product/4566) | I2C temperature and humidity sensor |
| [STEMMA QT / Qwiic JST SH 4-Pin Cable - 50mm](https://www.adafruit.com/product/4399) | Connects AHT20 to Feather without soldering |
| [Diffused Red/Green Indicator LED - 18mm Round](https://www.adafruit.com/product/4042) | Dual color LED for local status indication (common cathode) |
| [Adafruit LiPo Battery 2500mAh](https://www.adafruit.com/product/328) | Powers the board during battery operation |
| Panel mount momentary pushbutton | Wakes the board and triggers LED status display |
| 2x 80Ω resistors | Current limiting resistors for the LED |

## Wiring

| Component | Feather Pin |
|---|---|
| AHT20 | STEMMA QT port |
| LED red leg | A0 (via 80Ω resistor) |
| LED green leg | A1 (via 80Ω resistor) |
| LED center (cathode) | GND |
| Button | A4 → GND |

## Features

- Reads temperature, humidity, and battery state on every wake
- **Adaptive logging interval:** steps through a ladder of 30 min → 1 h → 3 h → 6 h → 12 h after 3 stable readings, and drops back down when humidity changes significantly or crosses an alert level
- Deep sleep between readings, with timer wake and button (GPIO) wake
- Logs to InfluxDB 2.x over WiFi, with NTP-synced timestamps
- **Offline buffering:** readings are stored in LittleFS when WiFi or InfluxDB is unavailable and replayed on the next successful connection, with timestamps reconstructed from a monotonic offset
- Push notifications through ntfy for humidity and battery thresholds, with escalating priority and reminder intervals
- Button press wakes the board and shows status on the LED (green = OK, red = action needed) without needing WiFi
- An error log on flash (capped at 10 KB) for post-mortem debugging

## Alert levels

### Humidity

| Level | Threshold | ntfy priority | Reminder frequency |
|---|---|---|---|
| Watch ("Humidity Rising") | 15% | default | One-time alert, then weekly |
| Warning | 18% | high | Daily |
| Critical | 20% | urgent | Hourly |

### Battery

| Level | Threshold | ntfy priority | Reminder frequency |
|---|---|---|---|
| Warning | 30% | default | One-time |
| Low | 15% | high | Daily |
| Emergency | 5% | urgent | Every 30 minutes |

## Data flow

```
ESP32-C6 ──WiFi──► InfluxDB (bucket: humidity)   ← time series, built-in dashboards
         └─WiFi──► ntfy       (topic: filament-humidity-monitor)   ← push alerts
```

### InfluxDB

- **Version:** 2.8.0, running in Docker (port `8086`)
- **Bucket:** `humidity` (set via `INFLUXDB_BUCKET`)
- **Measurement:** `environment`
- **Tag:** `location=bucket`
- **Fields:** `temperature` (°C), `humidity` (%RH), `battery_pct` (%), `battery_voltage` (V)
- Written with the v2 `/api/v2/write` line-protocol endpoint, using explicit timestamps so buffered readings land at the right time
- Dashboards use InfluxDB's built-in UI. Grafana was tried and removed as unnecessary

Example query:

```flux
from(bucket: "humidity")
  |> range(start: -7d)
  |> filter(fn: (r) => r._measurement == "environment" and r._field == "humidity")
```

### ntfy

- **Server:** self-hosted ntfy in Docker (port `8088`)
- **Topic:** `filament-humidity-monitor` (set via `NTFY_TOPIC`). Subscribe to it in the ntfy phone app, pointed at your own server
- Alerts are plain HTTP `POST`s to `<NTFY_HOST>/<NTFY_TOPIC>` with `Title` and `Priority` headers
- Humidity alerts: "Humidity Rising" / "Humidity Warning" / "Humidity Critical!". Battery alerts: "Battery Warning" / "Battery Low" / "Battery Emergency!"
- The topic name is effectively the only access control on a default ntfy server. Keep the server on your LAN/VPN, or add ntfy auth if you expose it

### Monitoring the monitor

A battery-powered device that goes quiet looks exactly like a healthy device with nothing to report. After a WiFi change this unit sat offline from 8/17 to 11/10 before it was noticed. The fix is an **InfluxDB deadman check**: alert (via ntfy) if no data has arrived for roughly 24 hours or more. The longest normal logging interval is 12 hours, so a 24 h+ gap means something is wrong. No RTC is needed. NTP plus buffering already covers time and gaps.

## Building and flashing (Arduino)

1. Install the Arduino IDE (or `arduino-cli`) with the Espressif **ESP32** board package.
2. Install libraries: **Adafruit AHTX0** and **SparkFun MAX1704x Fuel Gauge**. HTTP, WiFi, and LittleFS come with the ESP32 core.
3. Copy `arduino/HumidityLid/config.h.example` to `arduino/HumidityLid/config.h` and fill in your credentials. `config.h` is gitignored.
4. Select the **Adafruit Feather ESP32-C6** board (or generic ESP32C6 Dev Module) and apply the settings below.
5. Put the board in bootloader mode (see below) and upload.

```c
// config.h
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

#define INFLUXDB_HOST "http://your-nas-ip:8086"
#define INFLUXDB_TOKEN "your-token-here"
#define INFLUXDB_ORG "your-org-id"
#define INFLUXDB_BUCKET "humidity"

#define NTFY_HOST "http://your-nas-ip:8088"
#define NTFY_TOPIC "filament-humidity-monitor"
```

WiFi credentials are compiled into the firmware. **If your WiFi network or password changes, the device needs to be reflashed.** It will otherwise keep waking, failing to connect, and buffering.

## Lessons learned

### ESP32-C6 + Arduino gotchas

- **Enable "USB CDC On Boot"** in the IDE, or `Serial` output won't appear. (`config.h.example` also defines `ARDUINO_USB_CDC_ON_BOOT` as a fallback.)
- **Manual bootloader entry:** hold **BOOT**, tap **RESET**, release BOOT, then upload.
- **Partition scheme:** use **"Huge APP (3MB No OTA)"** to fit the sketch.
- **Watchdog:** initialize it carefully. Double-initializing the task watchdog gives errors on this core.
- **Flash with esptool.** The C6 has no UF2 bootloader and no native-USB drive.
- **PlatformIO was tried and abandoned.** The Arduino IDE / `arduino-cli` with the Espressif package was the reliable toolchain for this chip.

### InfluxDB

- A `400` error on writes is often a stale state file or a mismatched bucket/org/token. Check the config values first, then the server state.
- Send explicit timestamps. Buffered readings replayed after an outage must land at the time they were measured, not the time they were uploaded.
- InfluxDB's built-in dashboards were enough. Don't add Grafana without a clear need.

### Reliability

- Test the offline path. The buffering exists because WiFi and the NAS will, eventually, both be unavailable at the wrong time.
- Add a deadman alert for silence (see above) rather than relying on the device to report its own failure.
- The board has to come out of the bucket to charge or reflash, so battery alerts matter more than they would on a mains-powered device.

## Why not CircuitPython?

This project started as a CircuitPython build, and a complete implementation was written, but it was abandoned. On the ESP32-C6, **CircuitPython 10.2.0 suffered recurring memory-access hard faults** that persisted across clean reinstalls. A battery-powered, deep-sleeping logger that crashes unpredictably is not usable, so the firmware was rewritten in Arduino/C++, which has been stable.

The old version is kept in [`circuitpython/`](circuitpython/) for reference only. Other boards, or later CircuitPython releases, may behave differently, but it has not been re-tested.

## Status

- Arduino firmware is deployed and logging real data
- The panel-mount button is wired in code but not yet physically installed
- Planned: InfluxDB deadman check with ntfy notification

## License

MIT

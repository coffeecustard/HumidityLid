# Filament Humidity Monitor

A low-power IoT sensor that monitors humidity inside a sealed filament storage bucket and alerts when levels approach the 20% threshold that can damage 3D printing filament.

## Components

| Component | Description |
|---|---|
| [Adafruit ESP32-C6 Feather - STEMMA QT](https://www.adafruit.com/product/5933) | Main microcontroller with WiFi 6, Bluetooth 5, and built-in LiPo charging and monitoring |
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

## Software

- **CircuitPython 10.2.0**
- **Libraries:** `adafruit_ahtx0`, `adafruit_max1704x`, `adafruit_requests`, `adafruit_connection_manager`, `adafruit_ntp`

## Features

- Reads temperature and humidity every 30 minutes minimum
- Adaptive sleep intervals — extends to 12 hours when humidity is stable and below 15%
- Deep sleep between readings for maximum battery life (~6 months on 2500mAh LiPo)
- Logs data to InfluxDB 2.8 over WiFi
- Push notifications via self-hosted ntfy when humidity thresholds are crossed
- Local data buffering when WiFi or InfluxDB is unavailable, synced on next successful connection
- Button press wakes board and shows current status on LED (green = OK, red = action needed)
- Battery monitoring with low battery alerts

## Humidity Alert Levels

| Level | Threshold | Alert Frequency |
|---|---|---|
| Watch | 15% | One-time alert, then weekly reminders |
| Warning | 18% | Daily reminders |
| Critical | 20% | Hourly reminders |

## Infrastructure

- **InfluxDB 2.8.0** — time series database running in Docker
- **ntfy** — self-hosted push notification service running in Docker

## Configuration

Copy `example.settings.toml` to `settings.toml` and fill in your credentials. This file is excluded from version control.

```toml
WIFI_SSID = "your-wifi-ssid"
WIFI_PASSWORD = "your-wifi-password"
TIMEZONE_OFFSET = -4

INFLUXDB_HOST = "http://your-nas-ip:8086"
INFLUXDB_TOKEN = "your-token-here"
INFLUXDB_ORG = "your-org-id"
INFLUXDB_BUCKET = "humidity"

NTFY_HOST = "http://your-nas-ip:8088"
NTFY_TOPIC = "filament-humidity-monitor"
```

## Notes

- The ESP32-C6 does not support UF2 flashing or native USB — use esptool to flash CircuitPython
- File editing is done via code.circuitpython.org over USB Serial
- The board must be taken out of the bucket to charge or update code

# CircuitPython version (archived)

**This implementation is no longer maintained. Use the Arduino firmware in [`../arduino/HumidityLid/`](../arduino/HumidityLid/).**

On the Adafruit ESP32-C6 Feather, CircuitPython 10.2.0 hit recurring memory-access hard faults that persisted across clean reinstalls. The project moved to Arduino/C++, which is stable. See the main [README](../readme.md#why-not-circuitpython) for details.

The code is kept for reference. It uses `settings.toml` (copy from `settings.toml.example`) rather than the Arduino `config.h`, and the libraries `adafruit_ahtx0`, `adafruit_max1704x`, `adafruit_requests`, `adafruit_connection_manager`, and `adafruit_ntp`.

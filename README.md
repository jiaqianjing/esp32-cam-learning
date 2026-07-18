# ESP32-CAM Learning

This repository is a small learning project for the AI-Thinker ESP32-CAM with an OV2640 camera.

The current firmware:

- starts a Wi-Fi access point named `esp32-cam`
- uses password `12345678`
- serves a simple camera page at `http://192.168.4.1/`
- serves a JPEG snapshot at `http://192.168.4.1/jpg`
- serves an MJPEG stream at `http://192.168.4.1:81/stream`

## Hardware

- ESP32-CAM module, AI-Thinker pinout
- OV2640 camera
- USB-to-serial adapter or ESP32-CAM download base

For flashing with a USB-to-serial adapter:

```text
USB-TTL GND -> ESP32-CAM GND
USB-TTL 5V  -> ESP32-CAM 5V
USB-TTL TXD -> ESP32-CAM U0R / RX
USB-TTL RXD -> ESP32-CAM U0T / TX
IO0 -> GND during flashing only
```

After flashing, disconnect `IO0` from `GND` and press `RST` to boot the firmware.

## Build And Upload

This project uses PlatformIO.

```bash
pio run
pio run -t upload
pio device monitor
```

The local development port is currently configured in `platformio.ini` as:

```text
/dev/cu.usbserial-A5069RR4
```

Change `upload_port` and `monitor_port` if your serial device name is different.

## Code Map

- `platformio.ini`: board, framework, serial port, and build settings
- `src/main.cpp`: camera initialization, Wi-Fi AP setup, HTTP routes, snapshot, and stream handlers

Key places to study:

- Wi-Fi name and password: `AP_SSID`, `AP_PASSWORD`
- camera pinout and quality: `init_camera()`
- web page HTML: `INDEX_HTML`
- snapshot route: `jpg_handler()`
- stream route: `stream_handler()`
- URL registration: `start_web_server()`

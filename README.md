# ESP32-CAM Learning

This repository is a small learning project for the AI-Thinker ESP32-CAM with an OV2640 camera.

For downstream system integration, see [docs/api.md](docs/api.md).

The current firmware:

- tries to connect to your normal Wi-Fi in STA mode
- falls back to a Wi-Fi access point named `esp32-cam` if STA credentials are missing or the connection fails
- uses password `12345678`
- starts mDNS as `http://esp32-cam.local/`
- serves a web control console, JPEG snapshot, MJPEG stream, JSON status endpoint, and control API

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

## Wi-Fi Modes

The firmware prefers STA mode:

```text
ESP32-CAM -> your router Wi-Fi
computer/phone -> same router Wi-Fi
browser -> http://esp32-cam.local/
```

To configure STA mode, copy the example secrets file:

```bash
cp include/secrets.example.h include/secrets.h
```

Then edit `include/secrets.h`:

```cpp
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"
```

Classic ESP32-CAM only supports 2.4GHz Wi-Fi. Use a 2.4GHz SSID, not a 5GHz-only network.

`include/secrets.h` is ignored by git so local Wi-Fi credentials are not committed.

If credentials are missing or the router connection fails within 15 seconds, the firmware starts AP fallback:

```text
SSID: esp32-cam
Password: 12345678
Open: http://192.168.4.1/
```

Endpoints:

```text
http://esp32-cam.local/
http://esp32-cam.local/jpg
http://esp32-cam.local/api/status
http://<device-ip>:81/stream
```

In AP fallback mode, use:

```text
http://192.168.4.1/
http://192.168.4.1/jpg
http://192.168.4.1/api/status
http://192.168.4.1:81/stream
```

## Web Console

The root page is a compact control console for using ESP32-CAM as an image acquisition device.

Current controls:

- MJPEG preview
- resolution: QVGA, CIF, VGA, SVGA, XGA
- JPEG quality, auto-reset to a safe recommendation whenever resolution changes
- measured MJPEG FPS and stream frame delay
- brightness, contrast, saturation
- vertical flip and horizontal mirror
- automatic white balance and automatic exposure
- flash LED intensity on GPIO4
- snapshot and raw stream links
- device status: network mode, IP, mDNS host, RSSI, uptime, heap, and PSRAM

Control API:

```text
GET /api/status
GET /api/control?var=<name>&val=<value>
```

Supported control names:

```text
framesize
quality
brightness
contrast
saturation
vflip
hmirror
awb
aec
flash
stream_delay
```

Frame rate note:

- OV2640/ESP32-CAM frame rate is variable in this firmware, not a fixed per-resolution setting.
- Actual FPS depends on resolution, JPEG quality, light/exposure time, Wi-Fi quality, and browser/client speed.
- The web console displays measured MJPEG FPS in `/api/status`.
- `stream_delay` only throttles the stream loop. It can reduce FPS, but it cannot force the sensor to deliver a guaranteed higher FPS.
- When `framesize` changes, the firmware automatically applies a recommended JPEG quality for that resolution. You can still manually adjust quality afterward.

## Code Map

- `platformio.ini`: board, framework, serial port, and build settings
- `include/secrets.example.h`: template for local Wi-Fi credentials
- `src/main.cpp`: camera initialization, STA/AP fallback setup, mDNS, HTTP routes, control console, snapshot, and stream handlers

Key places to study:

- fallback AP name and password: `AP_SSID`, `AP_PASSWORD`
- STA credentials: local `include/secrets.h`
- camera pinout and quality: `init_camera()`
- web page HTML: `INDEX_HTML`
- snapshot route: `jpg_handler()`
- stream route: `stream_handler()`
- status route: `status_handler()`
- control route: `control_handler()`
- network setup: `connect_sta()`, `start_ap_fallback()`, `start_mdns()`
- URL registration: `start_web_server()`

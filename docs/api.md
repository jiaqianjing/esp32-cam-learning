# ESP32-CAM Firmware Interface

This document describes the current ESP32-CAM acquisition firmware for downstream systems such as NAS jobs, Mac services, ESP32-S3 controllers, and browser-based tools.

## Device Role

The ESP32-CAM is designed as a lightweight image acquisition device.

It provides:

- live MJPEG stream
- on-demand JPEG snapshot
- browser control console
- JSON status endpoint
- HTTP control endpoint for runtime camera settings

It does not currently provide:

- local image/video persistence
- SD card recording
- NAS upload
- authentication
- HTTPS
- fixed frame-rate guarantees
- saved runtime camera settings across reboot

## Network Behavior

The firmware first tries STA mode using credentials compiled from local `include/secrets.h`.

```text
ESP32-CAM -> router Wi-Fi
client -> same LAN
```

If STA credentials are missing or connection fails after 15 seconds, it falls back to AP mode:

```text
SSID: esp32-cam
Password: 12345678
Default IP: 192.168.4.1
```

mDNS is advertised as:

```text
http://esp32-cam.local/
```

mDNS is convenient but may take a few seconds to resolve depending on the router/client. For backend integrations, prefer the LAN IP returned by `/api/status` or a DHCP reservation.

## Base URLs

In STA mode:

```text
http://<device-ip>/
http://esp32-cam.local/
```

Current tested LAN IP:

```text
http://192.168.31.91/
```

In AP fallback mode:

```text
http://192.168.4.1/
```

## Endpoints

### Web Console

```http
GET /
```

Returns the browser control console as HTML.

The console itself uses:

- `GET /api/status`
- `GET /api/control?...`
- `GET /jpg`
- `GET http://<same-host>:81/stream`

### Snapshot

```http
GET /jpg
```

Captures and returns one JPEG frame.

Response:

```http
Content-Type: image/jpeg
Content-Disposition: inline; filename=capture.jpg
```

Example:

```bash
curl http://192.168.31.91/jpg -o capture.jpg
```

NAS pull example:

```bash
mkdir -p ./esp32-cam
curl -fsS http://192.168.31.91/jpg -o "./esp32-cam/$(date +%Y%m%d-%H%M%S).jpg"
```

Notes:

- The image is captured at the currently configured resolution and JPEG quality.
- The image is not stored on the ESP32-CAM.
- The endpoint blocks while the camera captures and sends the frame.

### MJPEG Stream

```http
GET http://<device-ip>:81/stream
```

Returns an MJPEG stream.

Response:

```http
Content-Type: multipart/x-mixed-replace;boundary=123456789000000000000987654321
```

Example:

```text
http://192.168.31.91:81/stream
```

Notes:

- This stream is a sequence of JPEG images, not H.264/RTSP.
- Actual FPS is variable.
- FPS depends on resolution, JPEG quality, exposure time, Wi-Fi quality, client speed, and ESP32 load.
- `stream_delay` can throttle the loop, but cannot force the sensor to produce a guaranteed higher FPS.

### Status

```http
GET /api/status
```

Legacy alias:

```http
GET /status
```

Example:

```bash
curl http://192.168.31.91/api/status
```

Example response:

```json
{
  "network": {
    "mode": "STA",
    "hostname": "esp32-cam.local",
    "ip": "192.168.31.91",
    "ap_ssid": "esp32-cam",
    "sta_ssid": "King",
    "rssi": -48
  },
  "reset_reason": "power_on",
  "uptime_ms": 8261,
  "heap": {
    "free": 177088,
    "min_free": 174436
  },
  "psram": {
    "found": true,
    "free": 2064351
  },
  "stream": {
    "mode": "variable",
    "fps_x10": 0,
    "delay_ms": 60,
    "clients": 0
  },
  "flash": 0,
  "sensor": {
    "framesize": 5,
    "quality": 12,
    "brightness": 1,
    "contrast": 0,
    "saturation": -1,
    "vflip": 1,
    "hmirror": 0,
    "awb": 1,
    "aec": 1
  }
}
```

Field notes:

| Field | Meaning |
|---|---|
| `network.mode` | `STA` or `AP_FALLBACK` |
| `network.hostname` | mDNS hostname |
| `network.ip` | Current reachable device IP |
| `network.rssi` | Wi-Fi RSSI in dBm. AP fallback reports `0` |
| `reset_reason` | Cause of the most recent boot, such as `power_on`, `software_reset`, `panic`, or `watchdog` |
| `uptime_ms` | Milliseconds since boot |
| `heap.free` | Free internal heap bytes |
| `heap.min_free` | Minimum free heap since boot |
| `psram.found` | Whether PSRAM was detected |
| `psram.free` | Free PSRAM bytes |
| `stream.mode` | Currently `variable` |
| `stream.fps_x10` | Measured MJPEG FPS multiplied by 10 |
| `stream.delay_ms` | Delay inserted between stream frames |
| `stream.clients` | Number of active MJPEG stream handlers |
| `flash` | Current GPIO4 flash LED PWM value, 0-255 |
| `sensor.*` | Current OV2640 driver settings |

`stream.fps_x10` remains `0` until a client is actively consuming `/stream`.

### Control

```http
GET /api/control?var=<name>&val=<value>
```

Example:

```bash
curl "http://192.168.31.91/api/control?var=brightness&val=1"
```

Success response:

```json
{
  "ok": true,
  "var": "brightness",
  "value": 1
}
```

Failure response:

```json
{
  "ok": false,
  "var": "brightness",
  "value": 99,
  "error": "driver rejected value"
}
```

Supported controls:

| `var` | Range | Meaning |
|---|---:|---|
| `framesize` | `5`, `6`, `8` | Camera resolution enum; VGA is the supported maximum |
| `quality` | `10`-`63` | JPEG compression quality. Lower is better/larger |
| `brightness` | `-2`-`2` | Sensor brightness |
| `contrast` | `-2`-`2` | Sensor contrast |
| `saturation` | `-2`-`2` | Sensor saturation |
| `vflip` | `0` or `1` | Vertical flip |
| `hmirror` | `0` or `1` | Horizontal mirror |
| `awb` | `0` or `1` | Automatic white balance |
| `aec` | `0` or `1` | Automatic exposure control |
| `flash` | `0`-`255` | GPIO4 flash LED PWM level |
| `stream_delay` | `0`-`250` | Delay between MJPEG stream frames in ms |

Resolution values:

| Value | Name | Size |
|---:|---|---|
| `5` | QVGA | 320 x 240 |
| `6` | CIF | 400 x 296 |
| `8` | VGA | 640 x 480 |

Control notes:

- Settings are runtime-only and reset to firmware defaults after reboot.
- The web console pauses its MJPEG connection before changing `framesize`, waits briefly for the sensor to settle, and resumes only if it was previously playing.
- The firmware invalidates and drains active MJPEG handlers before changing `framesize`, so direct API clients cannot reconfigure the sensor underneath an old stream.
- Invalidated streams have their socket shut down and use `Connection: close`, which interrupts a blocked Wi-Fi write and prevents browsers from leaving a stale MJPEG request behind.
- Direct API clients should still reconnect `/stream` after a successful `framesize` response.
- Changing `framesize` also resets JPEG quality to the firmware recommendation for that resolution.
- High resolution plus low `quality` values can increase latency and reduce stability.
- SVGA (`9`) and XGA (`10`) are deliberately rejected before camera reconfiguration because capture at either size has caused a camera panic and reboot on this board.
- `flash` controls GPIO4. Avoid leaving high flash values on for long periods if the board is enclosed or battery powered.

Recommended JPEG quality values applied on every `framesize` change:

| Framesize | Size | Applied quality |
|---|---|---:|
| QVGA | 320 x 240 | `18` |
| CIF | 400 x 296 | `20` |
| VGA | 640 x 480 | `22` |

The quality number is intentionally higher at larger resolutions because ESP32-CAM JPEG quality is inverse: lower numbers produce better/larger images, while higher numbers produce smaller/faster images.

## Recommended Integration Patterns

### NAS Pull Mode

Recommended first integration.

```text
NAS cron/scheduler -> GET /jpg -> save file
```

Pros:

- ESP32-CAM remains simple.
- NAS owns storage, retention, naming, and cleanup.
- Easy to test with `curl`.

Example cron-style script:

```bash
#!/usr/bin/env bash
set -euo pipefail

DEVICE="http://192.168.31.91"
OUT_DIR="/path/to/nas/cache/esp32-cam/$(date +%Y-%m-%d)"
mkdir -p "$OUT_DIR"

curl -fsS "$DEVICE/jpg" -o "$OUT_DIR/$(date +%H%M%S).jpg"
```

### Browser Manual Capture

Use the web console:

```text
http://192.168.31.91/
```

Click `Snapshot`, then save the returned image on the client device.

### CV Pipeline

Recommended architecture for heavier vision work:

```text
ESP32-CAM -> /jpg or /stream
NAS/Mac/ESP32-S3/service -> detection, classification, indexing, storage
```

Keep ESP32-CAM focused on acquisition. Run OpenCV, object detection, OCR, and storage policy elsewhere.

## Current Defaults

Runtime defaults set in firmware:

| Setting | Default |
|---|---|
| STA timeout | 15 seconds |
| AP SSID | `esp32-cam` |
| AP password | `12345678` |
| mDNS hostname | `esp32-cam.local` |
| Initial resolution | QVGA |
| JPEG quality | `18` for initial QVGA |
| Brightness | `1` during init, user-adjustable |
| Saturation | `-1` during init, user-adjustable |
| V flip | `1` during init |
| Flash | `0` |
| Stream delay | `60 ms` |

## Operational Notes

- Classic ESP32-CAM supports only 2.4GHz Wi-Fi.
- Use HTTP, not HTTPS.
- There is no authentication. Use only on a trusted LAN.
- The stream runs on port `81`; the console, snapshot, status, and control API run on port `80`.
- If mDNS is slow or unavailable, use the IP from serial logs or DHCP/router lease.
- A `HEAD /` request may return `405 Method Not Allowed`; use `GET`.
- Avoid writing high-frequency polling loops against `/jpg`; leave enough time for capture and Wi-Fi transfer.
- For stable integrations, reserve the ESP32-CAM IP in the router DHCP settings.

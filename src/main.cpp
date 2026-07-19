#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22
#define FLASH_GPIO_NUM 4
#define FLASH_LEDC_CHANNEL LEDC_CHANNEL_1
#define FLASH_LEDC_TIMER LEDC_TIMER_1

static const char *AP_SSID = "esp32-cam";
static const char *AP_PASSWORD = "12345678";
static const char *MDNS_HOSTNAME = "esp32-cam";
static const uint32_t STA_CONNECT_TIMEOUT_MS = 15000;
static const size_t QUERY_BUFFER_SIZE = 96;

static httpd_handle_t server = nullptr;
static httpd_handle_t stream_server = nullptr;
static bool ap_fallback_mode = false;
static int flash_level = 0;
static uint16_t stream_delay_ms = 60;
static uint16_t stream_fps_x10 = 0;
static uint16_t stream_frames_in_window = 0;
static uint32_t stream_fps_window_started = 0;
static volatile uint32_t stream_generation = 1;
static volatile uint16_t active_stream_clients = 0;
static volatile int active_stream_socket = -1;
static portMUX_TYPE stream_state_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power_on";
    case ESP_RST_EXT:
      return "external_reset";
    case ESP_RST_SW:
      return "software_reset";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
      return "task_watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep_sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unknown";
  }
}

static int recommended_quality_for_framesize(int framesize) {
  switch (framesize) {
    case FRAMESIZE_QVGA:
      return 18;
    case FRAMESIZE_CIF:
      return 20;
    case FRAMESIZE_VGA:
      return 22;
    case FRAMESIZE_SVGA:
      return 26;
    case FRAMESIZE_XGA:
      return 30;
    default:
      return 24;
  }
}

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-CAM Console</title>
  <style>
    :root{color-scheme:dark;--bg:#111418;--panel:#191e24;--line:#303844;--text:#eef3f8;--muted:#9aa6b2;--accent:#55c2ff;--ok:#74d99f;--warn:#ffd166}
    *{box-sizing:border-box}
    body{margin:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:var(--bg);color:var(--text)}
    header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 16px;border-bottom:1px solid var(--line);background:#15191f;position:sticky;top:0;z-index:2}
    h1{font-size:18px;margin:0;font-weight:650}
    main{display:grid;grid-template-columns:minmax(0,1fr) 360px;gap:12px;padding:12px;max-width:1440px;margin:0 auto}
    .preview{min-width:0;position:relative;background:#07090c;border:1px solid var(--line);border-radius:8px;overflow:hidden}
    .preview img{display:block;width:100%;min-height:280px;max-height:calc(100vh - 96px);object-fit:contain;background:#050607}
    .stream-state{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);padding:7px 10px;border:1px solid var(--line);border-radius:6px;background:rgba(11,15,20,.9);color:var(--text);font-size:12px;visibility:hidden}
    .stream-state.visible{visibility:visible}
    .side{display:grid;gap:12px;align-content:start}
    section{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:12px}
    h2{font-size:13px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);margin:0 0 10px}
    .row{display:grid;grid-template-columns:118px minmax(0,1fr) 42px;gap:8px;align-items:center;margin:9px 0}
    .row.switch{grid-template-columns:118px 1fr}
    label{font-size:13px;color:#c8d1dc}
    select,input[type=range]{width:100%}
    select{height:34px;border-radius:6px;border:1px solid var(--line);background:#10151b;color:var(--text);padding:0 8px}
    input[type=range]{accent-color:var(--accent)}
    output{font-variant-numeric:tabular-nums;text-align:right;color:var(--muted);font-size:12px}
    .toggles{display:grid;grid-template-columns:1fr 1fr;gap:8px}
    .toggle{display:flex;align-items:center;gap:8px;background:#10151b;border:1px solid var(--line);border-radius:6px;padding:8px;font-size:13px;color:#c8d1dc}
    .actions{display:grid;grid-template-columns:1fr 1fr;gap:8px}
    button,a.button{height:36px;border:1px solid var(--line);border-radius:6px;background:#10151b;color:var(--text);text-decoration:none;display:inline-flex;align-items:center;justify-content:center;font-size:13px;cursor:pointer}
    button:hover,a.button:hover{border-color:var(--accent)}
    .status{display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:12px}
    .metric{background:#10151b;border:1px solid var(--line);border-radius:6px;padding:8px;min-width:0}
    .metric span{display:block;color:var(--muted);margin-bottom:4px}
    .metric strong{font-weight:600;overflow-wrap:anywhere}
    .log{height:118px;overflow:auto;background:#0b0f14;border:1px solid var(--line);border-radius:6px;padding:8px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:11px;color:#c7d0dc}
    .hint{font-size:12px;line-height:1.4;color:var(--muted);margin:8px 0 0}
    .pill{font-size:12px;border:1px solid var(--line);border-radius:999px;padding:4px 8px;color:var(--muted)}
    .ok{color:var(--ok)}.warn{color:var(--warn)}
    :disabled{cursor:wait;opacity:.55}
    @media(max-width:900px){main{grid-template-columns:1fr}.side{grid-template-columns:1fr}.preview img{max-height:none}.row{grid-template-columns:108px minmax(0,1fr) 38px}}
  </style>
</head>
<body>
  <header>
    <h1>ESP32-CAM Console</h1>
    <span class="pill" id="netPill">connecting</span>
  </header>
  <main>
    <div class="preview">
      <img id="stream" alt="ESP32-CAM stream">
      <span class="stream-state" id="streamState" role="status" aria-live="polite"></span>
    </div>
    <div class="side">
      <section>
        <h2>Capture</h2>
        <div class="row">
          <label for="framesize">Resolution</label>
          <select id="framesize" data-var="framesize">
            <option value="5" data-quality="18">QVGA 320x240</option>
            <option value="6" data-quality="20">CIF 400x296</option>
            <option value="8" data-quality="22">VGA 640x480</option>
            <option value="9" data-quality="26">SVGA 800x600</option>
            <option value="10" data-quality="30">XGA 1024x768</option>
          </select>
          <output></output>
        </div>
        <div class="row"><label for="quality">JPEG Quality</label><input id="quality" data-var="quality" type="range" min="10" max="40" step="1"><output></output></div>
        <div class="row"><label for="stream_delay">Frame Delay</label><input id="stream_delay" data-var="stream_delay" type="range" min="0" max="250" step="10"><output></output></div>
        <div class="row"><label for="brightness">Brightness</label><input id="brightness" data-var="brightness" type="range" min="-2" max="2" step="1"><output></output></div>
        <div class="row"><label for="contrast">Contrast</label><input id="contrast" data-var="contrast" type="range" min="-2" max="2" step="1"><output></output></div>
        <div class="row"><label for="saturation">Saturation</label><input id="saturation" data-var="saturation" type="range" min="-2" max="2" step="1"><output></output></div>
        <div class="row"><label for="flash">Flash</label><input id="flash" data-var="flash" type="range" min="0" max="255" step="1"><output></output></div>
        <div class="toggles">
          <label class="toggle"><input id="vflip" data-var="vflip" type="checkbox"> V Flip</label>
          <label class="toggle"><input id="hmirror" data-var="hmirror" type="checkbox"> H Mirror</label>
          <label class="toggle"><input id="awb" data-var="awb" type="checkbox"> AWB</label>
          <label class="toggle"><input id="aec" data-var="aec" type="checkbox"> AEC</label>
        </div>
        <p class="hint">Sensor FPS is automatic. The console shows measured MJPEG FPS; Frame Delay only slows the stream loop.</p>
      </section>
      <section>
        <h2>Actions</h2>
        <div class="actions">
          <button id="toggleStream">Pause</button>
          <a class="button" id="captureLink" href="/jpg" target="_blank">Snapshot</a>
          <a class="button" id="streamLink" href="#" target="_blank">Stream</a>
          <a class="button" href="/api/status" target="_blank">JSON</a>
        </div>
      </section>
      <section>
        <h2>Status</h2>
        <div class="status" id="statusGrid"></div>
      </section>
      <section>
        <h2>Console</h2>
        <div class="log" id="log"></div>
      </section>
    </div>
  </main>
  <script>
    const streamUrl = `http://${location.hostname}:81/stream`;
    const stream = document.getElementById('stream');
    const streamState = document.getElementById('streamState');
    const emptyFrame = 'data:image/gif;base64,R0lGODlhAQABAAD/ACwAAAAAAQABAAACADs=';
    const logEl = document.getElementById('log');
    const controls = [...document.querySelectorAll('[data-var]')];
    let streaming = true;
    let busy = false;

    function log(msg){const t=new Date().toLocaleTimeString();logEl.textContent+=`[${t}] ${msg}\n`;logEl.scrollTop=logEl.scrollHeight}
    const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
    function showStreamState(message=''){streamState.textContent=message;streamState.classList.toggle('visible',!!message)}
    function setBusy(on){
      busy=on;
      controls.forEach(el=>el.disabled=on);
      document.getElementById('toggleStream').disabled=on;
    }
    function setStream(on){
      streaming=on;
      if(on){stream.src=`${streamUrl}?t=${Date.now()}`;showStreamState()}
      else{stream.src=emptyFrame}
      document.getElementById('toggleStream').textContent=on?'Pause':'Resume';
    }
    function setOutput(el){const out=el.closest('.row')?.querySelector('output');if(out)out.textContent=el.tagName==='SELECT'?el.options[el.selectedIndex].text.split(' ')[0]:el.value}
    async function control(el){
      if(busy)return;
      const val=el.type==='checkbox'?(el.checked?1:0):el.value;
      const switchingResolution=el.dataset.var==='framesize';
      const resumeStream=switchingResolution&&streaming;
      setOutput(el);
      setBusy(true);
      try{
        if(switchingResolution){
          log(`switching resolution to ${el.options[el.selectedIndex].text}`);
          showStreamState('Switching resolution...');
          if(resumeStream){
            setStream(false);
            showStreamState('Switching resolution...');
            await wait(150);
          }
        }
        const r=await fetch(`/api/control?var=${encodeURIComponent(el.dataset.var)}&val=${encodeURIComponent(val)}`);
        const data=await r.json();
        if(!r.ok||!data.ok)throw new Error(data.error||`HTTP ${r.status}`);
        log(data.ok?`${el.dataset.var} = ${val}`:`${el.dataset.var} failed: ${data.error}`);
        if(switchingResolution){
          const q=el.options[el.selectedIndex]?.dataset.quality;
          if(q)log(`recommended JPEG quality ${q} applied`);
          await wait(650);
        }
        await refreshStatus();
      }catch(e){log(`${el.dataset.var} failed: ${e.message}`)}
      finally{
        if(switchingResolution&&resumeStream){
          setStream(true);
          log('stream resumed');
        }else if(switchingResolution){
          showStreamState('Stream paused');
        }
        setBusy(false);
      }
    }
    function bind(){
      controls.forEach(el=>{
        setOutput(el);
        const event=el.type==='range'?'change':'change';
        el.addEventListener('input',()=>setOutput(el));
        el.addEventListener(event,()=>control(el));
      });
      document.getElementById('toggleStream').addEventListener('click',()=>{
        setStream(!streaming);
        showStreamState(streaming?'':'Stream paused');
      });
      document.getElementById('streamLink').href=streamUrl;
      setStream(true);
    }
    function metric(k,v){return `<div class="metric"><span>${k}</span><strong>${v}</strong></div>`}
    async function refreshStatus(){
      const r=await fetch('/api/status');
      const s=await r.json();
      document.getElementById('netPill').textContent=`${s.network.mode} ${s.network.ip}`;
      document.getElementById('netPill').className=`pill ${s.network.mode==='STA'?'ok':'warn'}`;
      const sensor=s.sensor||{};
      for(const el of controls){
        let value;
        if(el.dataset.var==='flash')value=s.flash;
        else if(el.dataset.var==='stream_delay')value=s.stream?.delay_ms;
        else value=sensor[el.dataset.var];
        if(value===undefined)continue;
        if(el.type==='checkbox')el.checked=!!value;else el.value=value;
        setOutput(el);
      }
      const fps=(s.stream.fps_x10/10).toFixed(1);
      document.getElementById('statusGrid').innerHTML=[
        metric('IP',s.network.ip),
        metric('Host',s.network.hostname),
        metric('RSSI',s.network.rssi+' dBm'),
        metric('FPS',fps+' variable'),
        metric('FPS Mode',s.stream.mode),
        metric('Delay',s.stream.delay_ms+' ms'),
        metric('Uptime',Math.floor(s.uptime_ms/1000)+' s'),
        metric('Heap',s.heap.free),
        metric('PSRAM',s.psram.free)
      ].join('');
      return s;
    }
    bind();
    refreshStatus().then(()=>log('status loaded')).catch(e=>log(e.message));
    setInterval(()=>{if(!busy)refreshStatus().catch(()=>{})},5000);
  </script>
</body>
</html>
)HTML";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t jpg_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  esp_err_t res = httpd_resp_send(req, reinterpret_cast<const char *>(fb->buf), fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t status_handler(httpd_req_t *req) {
  sensor_t *sensor = esp_camera_sensor_get();
  IPAddress ip = ap_fallback_mode ? WiFi.softAPIP() : WiFi.localIP();
  String body = "{";
  body += "\"network\":{\"mode\":\"";
  body += ap_fallback_mode ? "AP_FALLBACK" : "STA";
  body += "\",\"hostname\":\"";
  body += MDNS_HOSTNAME;
  body += ".local\",\"ip\":\"";
  body += ip.toString();
  body += "\",\"ap_ssid\":\"";
  body += AP_SSID;
  body += "\",\"sta_ssid\":\"";
  body += WIFI_SSID;
  body += "\",\"rssi\":";
  body += ap_fallback_mode ? 0 : WiFi.RSSI();
  body += "},\"reset_reason\":\"";
  body += reset_reason_name(esp_reset_reason());
  body += "\",\"uptime_ms\":";
  body += millis();
  body += ",\"heap\":{\"free\":";
  body += ESP.getFreeHeap();
  body += ",\"min_free\":";
  body += ESP.getMinFreeHeap();
  body += "},\"psram\":{\"found\":";
  body += psramFound() ? "true" : "false";
  body += ",\"free\":";
  body += ESP.getFreePsram();
  body += "},\"stream\":{\"mode\":\"variable\",\"fps_x10\":";
  body += stream_fps_x10;
  body += ",\"delay_ms\":";
  body += stream_delay_ms;
  body += ",\"clients\":";
  body += active_stream_clients;
  body += "},\"flash\":";
  body += flash_level;
  body += ",\"sensor\":{";
  if (sensor) {
    body += "\"framesize\":";
    body += sensor->status.framesize;
    body += ",\"quality\":";
    body += sensor->status.quality;
    body += ",\"brightness\":";
    body += sensor->status.brightness;
    body += ",\"contrast\":";
    body += sensor->status.contrast;
    body += ",\"saturation\":";
    body += sensor->status.saturation;
    body += ",\"vflip\":";
    body += sensor->status.vflip;
    body += ",\"hmirror\":";
    body += sensor->status.hmirror;
    body += ",\"awb\":";
    body += sensor->status.awb;
    body += ",\"aec\":";
    body += sensor->status.aec;
  }
  body += "}}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, body.c_str(), body.length());
}

static bool get_query_value(httpd_req_t *req, const char *key, char *value, size_t value_len) {
  char query[QUERY_BUFFER_SIZE];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return false;
  }
  return httpd_query_key_value(query, key, value, value_len) == ESP_OK;
}

static void set_flash_level(int value) {
  flash_level = constrain(value, 0, 255);
  ledcWrite(FLASH_LEDC_CHANNEL, flash_level);
}

static void set_stream_delay(int value) {
  stream_delay_ms = constrain(value, 0, 250);
}

static uint32_t register_stream_client(int socket_fd) {
  portENTER_CRITICAL(&stream_state_mux);
  uint32_t generation = stream_generation;
  active_stream_clients++;
  active_stream_socket = socket_fd;
  portEXIT_CRITICAL(&stream_state_mux);
  return generation;
}

static void unregister_stream_client() {
  portENTER_CRITICAL(&stream_state_mux);
  if (active_stream_clients > 0) {
    active_stream_clients--;
  }
  active_stream_socket = -1;
  portEXIT_CRITICAL(&stream_state_mux);
}

static bool stop_active_streams(uint32_t timeout_ms) {
  int socket_fd;
  portENTER_CRITICAL(&stream_state_mux);
  stream_generation++;
  socket_fd = active_stream_socket;
  portEXIT_CRITICAL(&stream_state_mux);

  if (socket_fd >= 0) {
    shutdown(socket_fd, SHUT_RDWR);
  }

  uint32_t started = millis();
  while (active_stream_clients > 0 && millis() - started < timeout_ms) {
    delay(10);
  }
  return active_stream_clients == 0;
}

static esp_err_t send_control_response(httpd_req_t *req, bool ok, const char *var, int value, const char *error = "") {
  String body = "{\"ok\":";
  body += ok ? "true" : "false";
  body += ",\"var\":\"";
  body += var;
  body += "\",\"value\":";
  body += value;
  if (!ok) {
    body += ",\"error\":\"";
    body += error;
    body += "\"";
  }
  body += "}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, body.c_str(), body.length());
}

static esp_err_t control_handler(httpd_req_t *req) {
  char var[24];
  char val[16];
  if (!get_query_value(req, "var", var, sizeof(var)) || !get_query_value(req, "val", val, sizeof(val))) {
    return send_control_response(req, false, "unknown", 0, "missing query");
  }

  sensor_t *sensor = esp_camera_sensor_get();
  int value = atoi(val);
  bool ok = false;

  if (strcmp(var, "flash") == 0) {
    set_flash_level(value);
    ok = true;
  } else if (strcmp(var, "stream_delay") == 0) {
    set_stream_delay(value);
    ok = true;
  } else if (!sensor) {
    return send_control_response(req, false, var, value, "sensor unavailable");
  } else if (strcmp(var, "framesize") == 0) {
    if (!stop_active_streams(1000)) {
      Serial.println("Control framesize failed: stream stop timeout");
      return send_control_response(req, false, var, value, "stream stop timeout");
    }
    int framesize = constrain(value, FRAMESIZE_QVGA, FRAMESIZE_XGA);
    ok = sensor->set_framesize(sensor, static_cast<framesize_t>(framesize)) == 0;
    if (ok) {
      int recommended_quality = recommended_quality_for_framesize(framesize);
      ok = sensor->set_quality(sensor, recommended_quality) == 0;
    }
  } else if (strcmp(var, "quality") == 0) {
    ok = sensor->set_quality(sensor, constrain(value, 10, 63)) == 0;
  } else if (strcmp(var, "brightness") == 0) {
    ok = sensor->set_brightness(sensor, constrain(value, -2, 2)) == 0;
  } else if (strcmp(var, "contrast") == 0) {
    ok = sensor->set_contrast(sensor, constrain(value, -2, 2)) == 0;
  } else if (strcmp(var, "saturation") == 0) {
    ok = sensor->set_saturation(sensor, constrain(value, -2, 2)) == 0;
  } else if (strcmp(var, "vflip") == 0) {
    ok = sensor->set_vflip(sensor, value ? 1 : 0) == 0;
  } else if (strcmp(var, "hmirror") == 0) {
    ok = sensor->set_hmirror(sensor, value ? 1 : 0) == 0;
  } else if (strcmp(var, "awb") == 0) {
    ok = sensor->set_whitebal(sensor, value ? 1 : 0) == 0;
  } else if (strcmp(var, "aec") == 0) {
    ok = sensor->set_exposure_ctrl(sensor, value ? 1 : 0) == 0;
  } else {
    return send_control_response(req, false, var, value, "unknown control");
  }

  Serial.printf("Control %s=%d %s\n", var, value, ok ? "ok" : "failed");
  return send_control_response(req, ok, var, value, ok ? "" : "driver rejected value");
}

static void record_stream_frame() {
  uint32_t now = millis();
  if (stream_fps_window_started == 0) {
    stream_fps_window_started = now;
  }

  stream_frames_in_window++;
  uint32_t elapsed = now - stream_fps_window_started;
  if (elapsed >= 1000) {
    stream_fps_x10 = (stream_frames_in_window * 10000UL) / elapsed;
    stream_frames_in_window = 0;
    stream_fps_window_started = now;
  }
}

static esp_err_t stream_handler(httpd_req_t *req) {
  static const char *boundary = "123456789000000000000987654321";
  char part_buf[96];
  uint32_t generation = register_stream_client(httpd_req_to_sockfd(req));
  esp_err_t result = ESP_OK;

  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=123456789000000000000987654321");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  httpd_resp_set_hdr(req, "Connection", "close");

  while (generation == stream_generation) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera stream capture failed");
      result = ESP_FAIL;
      break;
    }

    size_t hlen = snprintf(part_buf, sizeof(part_buf),
                           "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                           boundary, static_cast<unsigned>(fb->len));

    esp_err_t res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(fb->buf), fb->len);
    }
    esp_camera_fb_return(fb);

    if (res != ESP_OK) {
      result = res;
      break;
    }

    record_stream_frame();
    delay(stream_delay_ms > 0 ? stream_delay_ms : 1);
  }

  unregister_stream_client();
  return result;
}

static bool init_camera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = recommended_quality_for_framesize(config.frame_size);
  config.fb_count = psramFound() ? 2 : 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    sensor->set_vflip(sensor, 1);
    sensor->set_brightness(sensor, 1);
    sensor->set_saturation(sensor, -1);
  }

  Serial.println("Camera init ok");
  return true;
}

static void init_flash() {
  ledcSetup(FLASH_LEDC_CHANNEL, 5000, 8);
  ledcAttachPin(FLASH_GPIO_NUM, FLASH_LEDC_CHANNEL);
  set_flash_level(0);
}

static bool has_sta_credentials() {
  return strlen(WIFI_SSID) > 0;
}

static void print_access_urls(const char *mode, IPAddress ip) {
  Serial.printf("Network mode: %s\n", mode);
  Serial.printf("Open: http://%s/\n", ip.toString().c_str());
  Serial.printf("Snapshot: http://%s/jpg\n", ip.toString().c_str());
  Serial.printf("Stream: http://%s:81/stream\n", ip.toString().c_str());
  Serial.printf("mDNS: http://%s.local/\n", MDNS_HOSTNAME);
}

static bool start_mdns() {
  if (!MDNS.begin(MDNS_HOSTNAME)) {
    Serial.println("mDNS failed to start");
    return false;
  }

  MDNS.addService("http", "tcp", 80);
  Serial.printf("mDNS started: http://%s.local/\n", MDNS_HOSTNAME);
  return true;
}

static bool connect_sta() {
  if (!has_sta_credentials()) {
    Serial.println("STA credentials not configured; using AP fallback");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to Wi-Fi SSID: %s", WIFI_SSID);

  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < STA_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("STA connect failed; using AP fallback");
    WiFi.disconnect(true);
    return false;
  }

  ap_fallback_mode = false;
  Serial.println("STA connected");
  Serial.printf("STA IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

static void start_ap_fallback() {
  ap_fallback_mode = true;
  WiFi.mode(WIFI_AP);
  bool ap_ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("AP fallback %s\n", ap_ok ? "started" : "failed");
  Serial.printf("SSID: %s\n", AP_SSID);
  Serial.printf("Password: %s\n", AP_PASSWORD);
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
}

static void start_web_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  httpd_uri_t index_uri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = index_handler,
      .user_ctx = nullptr,
  };
  httpd_uri_t jpg_uri = {
      .uri = "/jpg",
      .method = HTTP_GET,
      .handler = jpg_handler,
      .user_ctx = nullptr,
  };
  httpd_uri_t status_uri = {
      .uri = "/api/status",
      .method = HTTP_GET,
      .handler = status_handler,
      .user_ctx = nullptr,
  };
  httpd_uri_t legacy_status_uri = {
      .uri = "/status",
      .method = HTTP_GET,
      .handler = status_handler,
      .user_ctx = nullptr,
  };
  httpd_uri_t control_uri = {
      .uri = "/api/control",
      .method = HTTP_GET,
      .handler = control_handler,
      .user_ctx = nullptr,
  };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &jpg_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &legacy_status_uri);
    httpd_register_uri_handler(server, &control_uri);
    Serial.println("HTTP server started on port 80");
  }

  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;

  httpd_uri_t stream_uri = {
      .uri = "/stream",
      .method = HTTP_GET,
      .handler = stream_handler,
      .user_ctx = nullptr,
  };

  if (httpd_start(&stream_server, &stream_config) == ESP_OK) {
    httpd_register_uri_handler(stream_server, &stream_uri);
    Serial.println("Stream server started on port 81");
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Booting ESP32-CAM STA/AP fallback camera firmware");
  init_flash();

  if (!init_camera()) {
    Serial.println("Camera failed. Check OV2640 module and AI-Thinker pinout.");
  }

  bool sta_connected = connect_sta();
  if (!sta_connected) {
    start_ap_fallback();
  }

  start_mdns();
  start_web_server();
  print_access_urls(ap_fallback_mode ? "AP fallback" : "STA", ap_fallback_mode ? WiFi.softAPIP() : WiFi.localIP());
}

void loop() {
  delay(1000);
}

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

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

static const char *AP_SSID = "esp32-cam";
static const char *AP_PASSWORD = "12345678";

static httpd_handle_t server = nullptr;
static httpd_handle_t stream_server = nullptr;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-CAM</title>
  <style>
    body{margin:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#111;color:#eee;text-align:center}
    header{padding:18px 12px}
    h1{font-size:22px;margin:0 0 8px}
    img{width:100%;max-width:900px;height:auto;background:#000}
    a,button{display:inline-block;margin:8px;padding:10px 14px;border:1px solid #666;border-radius:6px;color:#eee;background:#222;text-decoration:none}
  </style>
</head>
<body>
  <header>
    <h1>ESP32-CAM</h1>
    <a href="/jpg">Capture JPG</a>
    <a href="/stream">Stream</a>
  </header>
  <img src="/stream">
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

static esp_err_t stream_handler(httpd_req_t *req) {
  static const char *boundary = "123456789000000000000987654321";
  char part_buf[96];

  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=123456789000000000000987654321");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera stream capture failed");
      return ESP_FAIL;
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
      break;
    }
    delay(60);
  }

  return ESP_OK;
}

static bool init_camera() {
  camera_config_t config;
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
  config.jpeg_quality = 12;
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

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &jpg_uri);
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
  Serial.println("Booting ESP32-CAM AP camera firmware");

  if (!init_camera()) {
    Serial.println("Camera failed. Check OV2640 module and AI-Thinker pinout.");
  }

  WiFi.mode(WIFI_AP);
  bool ap_ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("SoftAP %s\n", ap_ok ? "started" : "failed");
  Serial.printf("SSID: %s\n", AP_SSID);
  Serial.printf("Password: %s\n", AP_PASSWORD);
  Serial.printf("Open: http://%s/\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("Stream: http://%s:81/stream\n", WiFi.softAPIP().toString().c_str());

  start_web_server();
}

void loop() {
  delay(1000);
}

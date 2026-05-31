#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <time.h>

#include "cert.h"
#include "esp_camera.h"
#include "secrets.h"

static WebServer server(80);

// ===== FREENOVE ESP32 WROVER (CAM) pin map =====
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     21
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       19
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM        5
#define Y2_GPIO_NUM        4
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

static constexpr int kLedPin = 2;  // User LED on Freenove ESP32 WROVER (GPIO2, HIGH=ON?)
static constexpr const char* kTzInfo = "JST-9";
static constexpr const char* kNtp1 = "ntp.nict.jp";
static constexpr const char* kNtp2 = "pool.ntp.org";

// Pipeline configuration
static constexpr uint32_t kCheckIntervalMs = 5UL * 60 * 1000;  // 5 min
static constexpr uint32_t kFirstCheckDelayMs = 10000;          // 10s after boot
static constexpr float    kMinConfidence = 0.5f;
static constexpr time_t   kStatusDelaySec = 30 * 60;            // Status notification 30 min after a new brew
// BREWED notification fires only when cups_remaining reaches this threshold;
// avoids posting during the drip phase when the carafe is still filling.
static constexpr float    kBrewedMinCups  = 3.0f;

// Active hours: weekdays 9:00 (inclusive) - 18:00 (exclusive) JST
static constexpr int kActiveStartHour = 9;
static constexpr int kActiveEndHour   = 18;

// State (RAM only; resets on reboot by design).
//   g_last_state  : last est.state ("empty" | "partial" | "full" | "")
//   g_last_cups   : last est.cups_remaining
//   g_brew_epoch  : epoch of the last "empty -> non-empty" detection
//                   (0 = no active brew or status not yet posted)
//   g_status_sent : whether the 30-min status notification has been posted for the current brew
static String  g_last_state;
static float   g_last_cups   = -1.0f;
static time_t  g_brew_epoch  = 0;
static bool    g_status_sent = false;

// Cache of the last image and result sent to Gemini (used by HTTP / dashboard).
static uint8_t* g_cached_jpeg     = nullptr;
static size_t   g_cached_jpeg_len = 0;
static time_t   g_cached_epoch    = 0;
static String   g_cached_state;
static float    g_cached_cups       = 0.0f;
static float    g_cached_confidence = 0.0f;
static String   g_cached_reason;

// Teams target channel ("test" or "prod"). Persisted to NVS.
static Preferences prefsCfg;
static String g_channel = "test";

static const char* teamsUrl() {
  return (g_channel == "prod") ? TEAMS_WEBHOOK_URL_PROD : TEAMS_WEBHOOK_URL_TEST;
}

static void loadChannel() {
  prefsCfg.begin("cw-cfg", true);
  g_channel = prefsCfg.getString("channel", "test");
  prefsCfg.end();
  if (g_channel != "test" && g_channel != "prod") g_channel = "test";
}

static void saveChannel(const String& ch) {
  if (ch != "test" && ch != "prod") return;
  g_channel = ch;
  prefsCfg.begin("cw-cfg", false);
  prefsCfg.putString("channel", ch);
  prefsCfg.end();
}

// Recent event log (ring buffer; visible on the dashboard without serial).
struct LogEntry {
  time_t epoch = 0;
  String tag;       // BOOT / WIFI / NTP / TEAMS / GEMINI / CHECK / EVENT
  String message;
  bool   error = false;
};
static constexpr int kLogSize = 16;
static LogEntry g_log[kLogSize];
static int      g_log_next = 0;   // Next write index
static int      g_log_count = 0;  // Cumulative count (unbounded; dashboard renders latest kLogSize entries)

static void logEvent(const char* tag, const String& msg, bool is_error = false) {
  g_log[g_log_next].epoch   = time(nullptr);
  g_log[g_log_next].tag     = tag;
  g_log[g_log_next].message = msg;
  g_log[g_log_next].error   = is_error;
  g_log_next = (g_log_next + 1) % kLogSize;
  ++g_log_count;
  Serial.printf("[%s%s] %s\n", is_error ? "ERR " : "", tag, msg.c_str());
}

static void blinkFast(int times) {
  for (int i = 0; i < times; ++i) {
    digitalWrite(kLedPin, LOW);
    delay(80);
    digitalWrite(kLedPin, HIGH);
    delay(80);
  }
}

// Phase-complete blink ack: N short pulses followed by a pause.
// 1=Camera, 2=Wi-Fi, 3=NTP, 4=HTTP, 5=Boot notification (full boot sequence done).
static void blinkAck(int times) {
  for (int i = 0; i < times; ++i) {
    digitalWrite(kLedPin, LOW);
    delay(70);
    digitalWrite(kLedPin, HIGH);
    delay(120);
  }
  delay(400);  // Visual gap before the next phase
}

static bool connectWifi(uint32_t timeout_ms = 20000) {
  Serial.printf("[wifi] connecting to SSID '%s' ...\n", WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > timeout_ms) {
      Serial.printf("\n[wifi] connect TIMEOUT (status=%d)\n", WiFi.status());
      digitalWrite(kLedPin, HIGH);  // Turn LED off before returning
      return false;
    }
    // Fast blink (50ms ON / 200ms OFF) to indicate "searching"
    digitalWrite(kLedPin, LOW);
    delay(50);
    digitalWrite(kLedPin, HIGH);
    delay(200);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[wifi] OK  IP=%s  RSSI=%d dBm  ch=%d  MAC=%s\n",
                WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), WiFi.channel(),
                WiFi.macAddress().c_str());
  return true;
}

static bool syncNtp(uint32_t timeout_ms = 10000) {
  configTzTime(kTzInfo, kNtp1, kNtp2);
  Serial.printf("[ntp] syncing via %s / %s (tz=%s) ...\n", kNtp1, kNtp2, kTzInfo);
  struct tm tm{};
  if (!getLocalTime(&tm, timeout_ms)) {
    Serial.println("[ntp] sync TIMEOUT");
    return false;
  }
  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
  Serial.printf("[ntp] OK  %s\n", buf);
  return true;
}

static bool cameraInit() {
  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0 = Y2_GPIO_NUM;
  cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2 = Y4_GPIO_NUM;
  cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4 = Y6_GPIO_NUM;
  cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6 = Y8_GPIO_NUM;
  cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk     = XCLK_GPIO_NUM;
  cfg.pin_pclk     = PCLK_GPIO_NUM;
  cfg.pin_vsync    = VSYNC_GPIO_NUM;
  cfg.pin_href     = HREF_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn     = PWDN_GPIO_NUM;
  cfg.pin_reset    = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size   = FRAMESIZE_VGA;       // 640x480 (used by Gemini and dashboard)
  cfg.jpeg_quality = 12;
  cfg.fb_count     = 2;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[cam] init FAILED: 0x%x\n", err);
    return false;
  }

  // Sensor orientation: enable horizontal mirror so text on the carafe reads correctly.
  if (sensor_t* s = esp_camera_sensor_get()) {
    s->set_hmirror(s, 1);
    s->set_vflip(s, 0);
  }

  // Discard the first frame to let the sensor warm up.
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  sensor_t* s2 = esp_camera_sensor_get();
  Serial.printf("[cam] init OK  PID=0x%02x VER=0x%02x MIDH=0x%02x MIDL=0x%02x  frame=VGA q=10 fb=2 PSRAM\n",
                s2 ? s2->id.PID : 0, s2 ? s2->id.VER : 0,
                s2 ? s2->id.MIDH : 0, s2 ? s2->id.MIDL : 0);
  return true;
}

// HTML-escape for safe rendering in the dashboard.
static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '<')      out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

struct CoffeeEstimate {
  bool ok = false;
  float cups_remaining = 0.0f;
  String state;       // "empty" | "partial" | "full"
  float confidence = 0.0f;
  String reason;
  String error;
};

// Cache the latest analysis result and image in PSRAM (used by HTTP `/`).
static void cacheLatest(const uint8_t* jpeg, size_t len, const CoffeeEstimate& est) {
  if (g_cached_jpeg) {
    free(g_cached_jpeg);
    g_cached_jpeg = nullptr;
    g_cached_jpeg_len = 0;
  }
  uint8_t* buf = static_cast<uint8_t*>(ps_malloc(len));
  if (!buf) {
    Serial.println("[cache] ps_malloc failed");
    return;
  }
  memcpy(buf, jpeg, len);
  g_cached_jpeg       = buf;
  g_cached_jpeg_len   = len;
  g_cached_epoch      = time(nullptr);
  g_cached_state      = est.state;
  g_cached_cups       = est.cups_remaining;
  g_cached_confidence = est.confidence;
  g_cached_reason     = est.reason;
}

// Send a JPEG + prompt to Gemini's generateContent and extract the
// structured JSON output via responseSchema.
static CoffeeEstimate geminiAnalyze(const uint8_t* jpeg, size_t jpeg_len) {
  CoffeeEstimate r;

  // 1. Base64 encode (in PSRAM)
  size_t b64_olen = 0;
  mbedtls_base64_encode(nullptr, 0, &b64_olen, jpeg, jpeg_len);
  uint8_t* b64 = static_cast<uint8_t*>(ps_malloc(b64_olen + 1));
  if (!b64) { r.error = "ps_malloc b64"; return r; }
  if (mbedtls_base64_encode(b64, b64_olen, &b64_olen, jpeg, jpeg_len) != 0) {
    r.error = "base64";
    free(b64);
    return r;
  }
  b64[b64_olen] = 0;

  // 2. Build the request JSON in PSRAM
  static const char* kPrompt =
    "ドリップ式コーヒーメーカーのコーヒーサーバー (ガラス製ポット) の画像です。"
    "コーヒーの残量を推定して、スキーマに沿った JSON で返してください。"
    "- cups_remaining: 杯数の推定値 (0.0=空、最大10.0、小数可)"
    "- state: empty (コーヒーが見えない) / full (ほぼ満杯) / partial (それ以外)"
    "- confidence: 自信度 0.0〜1.0 (画像にコーヒーサーバーが写っていない場合は 0.3 未満にしてください)"
    "- reason: 日本語で 1 文の理由";

  const size_t cap = b64_olen + 4096;
  char* payload = static_cast<char*>(ps_malloc(cap));
  if (!payload) { r.error = "ps_malloc payload"; free(b64); return r; }
  const int n = snprintf(payload, cap,
    "{"
      "\"contents\":[{\"parts\":["
        "{\"text\":\"%s\"},"
        "{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"%s\"}}"
      "]}],"
      "\"generationConfig\":{"
        "\"responseMimeType\":\"application/json\","
        "\"responseSchema\":{"
          "\"type\":\"OBJECT\","
          "\"properties\":{"
            "\"cups_remaining\":{\"type\":\"NUMBER\"},"
            "\"state\":{\"type\":\"STRING\",\"enum\":[\"empty\",\"partial\",\"full\"]},"
            "\"confidence\":{\"type\":\"NUMBER\"},"
            "\"reason\":{\"type\":\"STRING\"}"
          "},"
          "\"required\":[\"cups_remaining\",\"state\",\"confidence\",\"reason\"]"
        "}"
      "}"
    "}",
    kPrompt, reinterpret_cast<char*>(b64));
  free(b64);
  if (n < 0 || static_cast<size_t>(n) >= cap) {
    r.error = "payload truncated";
    free(payload);
    return r;
  }
  Serial.printf("[gemini] payload=%d B (jpeg=%u B)\n", n, (unsigned)jpeg_len);

  // 3. HTTPS POST
  WiFiClientSecure client;
  client.setCACert(GTS_ROOT_R1);
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setTimeout(30000);
  static const char* kUrl =
    "https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-lite-latest:generateContent";
  if (!http.begin(client, kUrl)) {
    r.error = "http.begin";
    free(payload);
    return r;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", GEMINI_API_KEY);

  const uint32_t t0 = millis();
  const int code = http.POST(reinterpret_cast<uint8_t*>(payload), n);
  const uint32_t dt = millis() - t0;
  String resp = http.getString();
  http.end();
  free(payload);

  Serial.printf("[gemini] HTTP %d  %u ms  resp=%u B\n", code, dt, resp.length());
  if (code != 200) {
    r.error = "HTTP " + String(code);
    Serial.printf("[gemini] err: %s\n", resp.substring(0, 500).c_str());
    // Log a short summary (e.g., the "Please retry in X" part of 429 errors)
    String snippet = resp;
    snippet.replace("\n", " ");
    if (snippet.length() > 120) snippet = snippet.substring(0, 117) + "...";
    logEvent("GEMINI", String("HTTP ") + code + " " + snippet, true);
    return r;
  }

  // 4. Parse the outer JSON
  JsonDocument outer;
  if (DeserializationError err = deserializeJson(outer, resp)) {
    r.error = String("outer json: ") + err.c_str();
    return r;
  }
  const char* inner_text = outer["candidates"][0]["content"]["parts"][0]["text"];
  if (!inner_text) {
    r.error = "no text in response";
    Serial.printf("[gemini] raw: %s\n", resp.substring(0, 500).c_str());
    return r;
  }

  // 5. Parse the inner structured JSON (guaranteed by responseSchema)
  JsonDocument inner;
  if (DeserializationError err = deserializeJson(inner, inner_text)) {
    r.error = String("inner json: ") + err.c_str();
    Serial.printf("[gemini] inner raw: %s\n", inner_text);
    return r;
  }

  r.cups_remaining = inner["cups_remaining"].as<float>();
  r.state          = String(inner["state"].as<const char*>() ?: "");
  r.confidence     = inner["confidence"].as<float>();
  r.reason         = String(inner["reason"].as<const char*>() ?: "");
  r.ok = true;
  Serial.printf("[gemini] cups=%.2f state=%s conf=%.2f reason=%s\n",
                r.cups_remaining, r.state.c_str(), r.confidence, r.reason.c_str());
  {
    char msg[160];
    snprintf(msg, sizeof(msg), "state=%s cups=%.1f conf=%.2f",
             r.state.c_str(), r.cups_remaining, r.confidence);
    logEvent("GEMINI", msg, false);
  }
  cacheLatest(jpeg, jpeg_len, r);
  return r;
}

// Upload a JPEG to https://uguu.se via multipart/form-data and return the
// resulting public URL. uguu hosts the file for a few hours, which is enough
// for Teams to render it inline in the Adaptive Card. Returns "" on failure.
// TLS uses setInsecure() since the image carries no secret and uguu's ZeroSSL
// chain isn't in our cert.h.
static String uguuUpload(const uint8_t* jpeg, size_t jpeg_len) {
  static const char* kBoundary = "----coffee-watcher-7f4d2c1a";

  String head;
  head.reserve(180);
  head  = "--";
  head += kBoundary;
  head += "\r\nContent-Disposition: form-data; name=\"files[]\"; filename=\"thumb.jpg\"\r\n";
  head += "Content-Type: image/jpeg\r\n\r\n";

  String tail;
  tail.reserve(48);
  tail  = "\r\n--";
  tail += kBoundary;
  tail += "--\r\n";

  const size_t body_len = head.length() + jpeg_len + tail.length();
  uint8_t* body = static_cast<uint8_t*>(ps_malloc(body_len));
  if (!body) {
    Serial.println("[uguu] ps_malloc body failed");
    return String();
  }
  size_t pos = 0;
  memcpy(body + pos, head.c_str(), head.length()); pos += head.length();
  memcpy(body + pos, jpeg,         jpeg_len);      pos += jpeg_len;
  memcpy(body + pos, tail.c_str(), tail.length()); pos += tail.length();

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, "https://uguu.se/upload")) {
    Serial.println("[uguu] http.begin failed");
    free(body);
    return String();
  }
  http.addHeader("Content-Type", String("multipart/form-data; boundary=") + kBoundary);

  const uint32_t t0 = millis();
  const int code = http.POST(body, body_len);
  const uint32_t dt = millis() - t0;
  const String resp = http.getString();
  http.end();
  free(body);

  Serial.printf("[uguu] HTTP %d  %u ms  body=%u B\n", code, dt, resp.length());
  if (code < 200 || code >= 300) {
    logEvent("UGUU", String("HTTP ") + code, true);
    return String();
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    logEvent("UGUU", String("json parse: ") + err.c_str(), true);
    return String();
  }
  if (!doc["success"].as<bool>()) {
    logEvent("UGUU", "success=false", true);
    return String();
  }
  String url = doc["files"][0]["url"].as<String>();
  if (url.length() == 0) {
    logEvent("UGUU", "url missing in response", true);
    return String();
  }
  Serial.printf("[uguu] url=%s\n", url.c_str());
  return url;
}

// Post an Adaptive Card to a Power Automate (Teams Workflow) HTTP trigger.
// image_url may be empty, in which case the card is text-only. The flow on
// the other side hands the card to "Post card in a chat or channel" verbatim,
// so the request body IS the Adaptive Card JSON.
//
// Power Automate returns HTTP 202 on successful trigger; the actual Teams
// post happens asynchronously and failures only surface in the flow's run
// history. The 202 acceptance is the strongest signal we can check here.
static bool teamsPost(const char* title, const char* text, const String& image_url) {
  JsonDocument card;
  card["type"]    = "AdaptiveCard";
  card["$schema"] = "http://adaptivecards.io/schemas/adaptive-card.json";
  card["version"] = "1.4";
  JsonArray body = card["body"].to<JsonArray>();
  {
    JsonObject t = body.add<JsonObject>();
    t["type"]   = "TextBlock";
    t["text"]   = title;
    t["weight"] = "Bolder";
    t["size"]   = "Medium";
    t["wrap"]   = true;
  }
  if (text && text[0]) {
    JsonObject t = body.add<JsonObject>();
    t["type"] = "TextBlock";
    t["text"] = text;
    t["wrap"] = true;
  }
  if (image_url.length() > 0) {
    JsonObject img = body.add<JsonObject>();
    img["type"]    = "Image";
    img["url"]     = image_url;
    img["size"]    = "Large";
    img["altText"] = "coffee-watcher";
  }

  String payload;
  serializeJson(card, payload);

  WiFiClientSecure client;
  client.setCACert(DIGICERT_GLOBAL_ROOT_G2);
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, teamsUrl())) {
    Serial.println("[teams] http.begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json; charset=utf-8");

  const uint32_t t0 = millis();
  const int code = http.POST(payload);
  const uint32_t dt = millis() - t0;
  const String resp = http.getString();
  http.end();

  Serial.printf("[teams] HTTP %d  %u ms  payload=%u B  resp=%u B\n",
                code, dt, payload.length(), resp.length());

  const bool accepted = (code >= 200 && code < 300);
  if (accepted) {
    String extra = (image_url.length() > 0) ? String(" w/img") : String(" text-only");
    logEvent("TEAMS", String("posted to ") + g_channel + " (\"" + title + "\")" + extra, false);
  } else {
    String snippet = resp;
    if (snippet.length() > 120) snippet = snippet.substring(0, 117) + "...";
    char msg[220];
    snprintf(msg, sizeof(msg), "HTTP %d body=%s", code, snippet.c_str());
    logEvent("TEAMS", msg, true);
  }
  return accepted;
}

// Returns true if it's a weekday (Mon-Fri) and 9:00-18:00 JST. False if NTP is not yet synced.
static bool isActiveNow() {
  struct tm tm{};
  if (!getLocalTime(&tm, 0)) return false;
  if (tm.tm_wday < 1 || tm.tm_wday > 5) return false;       // 1=Mon..5=Fri
  return tm.tm_hour >= kActiveStartHour && tm.tm_hour < kActiveEndHour;
}

// (Forward declaration: defined later in the file but called from runCheck)
static bool teamsPostThumb(const char* title, const char* text,
                           const uint8_t* jpeg, size_t jpeg_len,
                           uint16_t src_w, uint16_t src_h);

// One cycle: capture → Gemini inference → event detection → Teams post → state update.
// force=true bypasses the active-hours check (used by manual /check).
static void runCheck(bool force = false) {
  if (!force && !isActiveNow()) {
    Serial.println("[check] outside active hours; skip");
    return;
  }
  Serial.println("[check] start");
  digitalWrite(kLedPin, LOW);  // LED solid on while processing

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[check] capture failed");
    digitalWrite(kLedPin, HIGH);
    return;
  }
  uint8_t* jpeg = static_cast<uint8_t*>(ps_malloc(fb->len));
  const size_t jpeg_len = fb->len;
  if (!jpeg) {
    Serial.println("[check] ps_malloc jpeg failed");
    esp_camera_fb_return(fb);
    digitalWrite(kLedPin, HIGH);
    return;
  }
  memcpy(jpeg, fb->buf, jpeg_len);
  esp_camera_fb_return(fb);

  CoffeeEstimate est = geminiAnalyze(jpeg, jpeg_len);
  if (!est.ok) {
    Serial.printf("[check] analyze failed: %s\n", est.error.c_str());
    free(jpeg);
    digitalWrite(kLedPin, HIGH);
    return;
  }
  if (est.confidence < kMinConfidence) {
    Serial.printf("[check] confidence %.2f < %.2f, skip\n",
                  est.confidence, kMinConfidence);
    free(jpeg);
    digitalWrite(kLedPin, HIGH);
    return;
  }

  const bool now_empty  = (est.state == "empty");
  const bool first_run  = g_last_state.length() == 0;
  const bool was_empty  = (g_last_state == "empty");
  const time_t now_t    = time(nullptr);
  const String cups_str = String(est.cups_remaining, 1);

  Serial.printf("[check] est state=%s cups=%.2f conf=%.2f  last=%s/%.2f brew_t=%ld\n",
                est.state.c_str(), est.cups_remaining, est.confidence,
                g_last_state.c_str(), g_last_cups,
                static_cast<long>(g_brew_epoch));

  // First observation: record the state silently (the boot moment may be mid-brew)
  if (first_run) {
    g_last_state = est.state;
    g_last_cups  = est.cups_remaining;
    Serial.println("[check] first observation, recorded silently");
    free(jpeg);
    digitalWrite(kLedPin, HIGH);
    return;
  }

  // Evaluate the 3 events in priority order
  bool posted = false;

  // Capture is assumed to be VGA
  const uint16_t img_w = 640, img_h = 480;

  if (was_empty && !now_empty && est.cups_remaining >= kBrewedMinCups) {
    // (1) New brew detected: empty -> non-empty AND cups crossed the threshold.
    // Below the threshold we leave g_last_state unchanged so subsequent cycles
    // keep re-evaluating until the carafe is full enough to be worth a ping.
    const String text = String("約 ") + cups_str + " 杯  — " + est.reason;
    if (teamsPostThumb("☕ 新しくコーヒーがはいりました！", text.c_str(),
                       jpeg, jpeg_len, img_w, img_h)) {
      g_brew_epoch  = now_t;
      g_status_sent = false;
      posted = true;
      logEvent("EVENT", "BREWED posted", false);
    } else {
      logEvent("EVENT", "BREWED post failed; will retry next cycle", true);
    }
  } else if (!was_empty && now_empty) {
    // (3) Empty detected: non-empty -> empty
    const String text = String("残量ゼロ  — ") + est.reason;
    if (teamsPostThumb("☕ コーヒーがなくなりました！", text.c_str(),
                       jpeg, jpeg_len, img_w, img_h)) {
      g_status_sent = true;   // Belt and suspenders: prevent duplicate status post
      posted = true;
      logEvent("EVENT", "EMPTIED posted", false);
    } else {
      logEvent("EVENT", "EMPTIED post failed; will retry next cycle", true);
    }
  } else if (g_brew_epoch > 0 && !g_status_sent &&
             !now_empty && now_t >= g_brew_epoch + kStatusDelaySec) {
    // (2) Post-brew status: >= 30 minutes after brew, not yet posted, currently not empty
    const String text = String("残り約 ") + cups_str + " 杯です  — " + est.reason;
    if (teamsPostThumb("☕ コーヒー残量更新", text.c_str(),
                       jpeg, jpeg_len, img_w, img_h)) {
      g_status_sent = true;
      posted = true;
      logEvent("EVENT", "STATUS posted", false);
    } else {
      logEvent("EVENT", "STATUS post failed; will retry next cycle", true);
    }
  } else {
    Serial.println("[check] no event");
  }

  // If a transition post failed, don't update last_state so the next cycle can retry.
  if (posted || (was_empty == now_empty)) {
    g_last_state = est.state;
    g_last_cups  = est.cups_remaining;
  } else {
    Serial.println("[check] post failed on transition; NOT updating last_state to allow retry");
  }

  free(jpeg);
  digitalWrite(kLedPin, HIGH);
}

// Produce a smaller JPEG by decoding, scaling 1/2, and re-encoding.
// Keeps the uguu upload (and therefore the URL hand-off to Teams) tiny so the
// flow stays within Power Automate's request size budget.
// Returns nullptr on failure. Caller must free() the returned buffer.
static uint8_t* jpegMakeThumb(const uint8_t* src, size_t src_len,
                              uint16_t src_w, uint16_t src_h,
                              uint8_t quality, size_t* out_len) {
  const uint16_t dst_w = src_w / 2;
  const uint16_t dst_h = src_h / 2;
  const size_t rgb_len = static_cast<size_t>(dst_w) * dst_h * 2;  // RGB565
  uint8_t* rgb = static_cast<uint8_t*>(ps_malloc(rgb_len));
  if (!rgb) {
    Serial.println("[thumb] ps_malloc rgb failed");
    return nullptr;
  }
  if (!jpg2rgb565(src, src_len, rgb, JPG_SCALE_2X)) {
    Serial.println("[thumb] jpg2rgb565 failed");
    free(rgb);
    return nullptr;
  }
  // jpg2rgb565 writes MSB-first (big-endian), but fmt2jpg(PIXFORMAT_RGB565)
  // expects little-endian. Swap each 2-byte pixel to match.
  for (size_t i = 0; i + 1 < rgb_len; i += 2) {
    const uint8_t t = rgb[i];
    rgb[i]     = rgb[i + 1];
    rgb[i + 1] = t;
  }
  uint8_t* out = nullptr;
  *out_len = 0;
  if (!fmt2jpg(rgb, rgb_len, dst_w, dst_h, PIXFORMAT_RGB565, quality, &out, out_len)) {
    Serial.println("[thumb] fmt2jpg failed");
    free(rgb);
    return nullptr;
  }
  free(rgb);
  Serial.printf("[thumb] %ux%u %u B → %ux%u %u B\n",
                src_w, src_h, (unsigned)src_len,
                dst_w, dst_h, (unsigned)*out_len);
  return out;
}

// Post to Teams via Power Automate. Downscales the JPEG to keep the uguu
// upload small, uploads to uguu to obtain a public URL, then posts an
// Adaptive Card referencing that URL. If the upload fails, still posts a
// text-only card so the notification isn't silently dropped.
static bool teamsPostThumb(const char* title, const char* text,
                           const uint8_t* jpeg, size_t jpeg_len,
                           uint16_t src_w, uint16_t src_h) {
  size_t thumb_len = 0;
  uint8_t* thumb = jpegMakeThumb(jpeg, jpeg_len, src_w, src_h, 25, &thumb_len);
  const uint8_t* upload_buf = thumb ? thumb : jpeg;
  const size_t   upload_len = thumb ? thumb_len : jpeg_len;
  if (!thumb) Serial.println("[teams] thumb failed, uploading original");

  String image_url = uguuUpload(upload_buf, upload_len);
  if (thumb) free(thumb);
  if (image_url.length() == 0) {
    Serial.println("[teams] uguu upload failed, posting text-only card");
  }
  return teamsPost(title, text, image_url);
}

static void handleJpg() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "capture failed");
    return;
  }
  server.sendHeader("Content-Disposition", "inline; filename=cam.jpg");
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg",
                reinterpret_cast<const char*>(fb->buf), fb->len);
  Serial.printf("[http] served %ux%u %u bytes\n", fb->width, fb->height, fb->len);
  esp_camera_fb_return(fb);
}

void setup() {
  pinMode(kLedPin, OUTPUT);
  digitalWrite(kLedPin, HIGH);

  Serial.begin(115200);
  delay(500);

  Serial.printf("\n=== coffee-watcher boot ===\n");
  Serial.printf("Chip       : %s rev %d, %d cores @ %d MHz\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("Flash size : %u bytes\n", ESP.getFlashChipSize());
  Serial.printf("PSRAM      : total=%u free=%u\n",
                ESP.getPsramSize(), ESP.getFreePsram());
  Serial.printf("Heap       : free=%u\n", ESP.getFreeHeap());
  Serial.printf("SDK        : %s\n", ESP.getSdkVersion());

  if (!cameraInit()) {
    blinkFast(20);
    Serial.println("[fatal] camera init failed; restart in 3s");
    delay(3000);
    ESP.restart();
  }
  blinkAck(1);  // Camera OK

  if (!connectWifi()) {
    blinkFast(20);
    logEvent("WIFI", "connect failed; restart in 3s", true);
    delay(3000);
    ESP.restart();
  }
  logEvent("WIFI", String("connected IP=") + WiFi.localIP().toString() +
                   " RSSI=" + WiFi.RSSI() + " dBm", false);
  blinkAck(2);  // Wi-Fi OK

  if (!syncNtp()) {
    logEvent("NTP", "sync timeout; TLS calls will fail until time syncs", true);
  } else {
    logEvent("NTP", "synced", false);
    blinkAck(3);  // NTP OK
  }

  loadChannel();
  logEvent("CONFIG", String("channel=") + g_channel, false);

  server.on("/", HTTP_GET, []() {
    String html;
    html.reserve(3072);
    html += F(
      "<!DOCTYPE html><html lang=\"ja\"><head>"
      "<meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      "<meta http-equiv=\"refresh\" content=\"60\">"
      "<title>Coffee Watcher</title>"
      "<style>"
      "body{font-family:sans-serif;max-width:720px;margin:1em auto;padding:0 1em;color:#222}"
      "h1{font-size:1.4em;margin:.2em 0}"
      "h2{font-size:1em;color:#666;margin:1.2em 0 .3em;border-bottom:1px solid #eee;padding-bottom:.2em}"
      ".stat{display:flex;gap:.8em;align-items:baseline;margin:.5em 0}"
      ".state{font-size:1.8em;font-weight:bold}"
      ".cups{font-size:1.2em;color:#666}"
      ".reason{color:#444;font-size:.95em}"
      ".meta{color:#888;font-size:.8em;margin:.3em 0 .5em}"
      "img{width:100%;border:1px solid #ccc;display:block}"
      ".nodata{color:#aaa;padding:1em 0;text-align:center;border:1px dashed #ddd}"
      ".actions{margin:1.4em 0;display:flex;gap:.5em;flex-wrap:wrap}"
      ".actions a{flex:1;text-align:center;padding:.7em 1em;background:#0078D4;color:#fff;"
      "text-decoration:none;border-radius:4px;min-width:8em}"
      ".actions a.warn{background:#a04020}"
      ".log{font-family:monospace;font-size:.85em;border-collapse:collapse;width:100%}"
      ".log td{padding:.2em .4em;vertical-align:top;border-bottom:1px solid #eee}"
      ".log td.t{color:#888;white-space:nowrap;width:10em}"
      ".log td.g{color:#555;white-space:nowrap;width:5em;font-weight:bold}"
      ".log tr.err td.g{color:#a02020}"
      ".log tr.err td.m{color:#a02020}"
      "</style></head><body>"
      "<h1>☕ Coffee Watcher</h1>");

    // Live camera image (with a cache buster).
    const uint32_t now_ms = millis();
    html += F("<h2>今の様子 (ライブ)</h2>");
    html += F("<img src=\"/jpg?t=");
    html += String(now_ms);
    html += F("\" alt=\"live\">");

    // Last inference result.
    html += F("<h2>最後の推論</h2>");
    if (g_cached_jpeg && g_cached_epoch > 0) {
      html += F("<img src=\"/last.jpg\" alt=\"last analyzed\">");
      html += F("<div class=\"stat\"><span class=\"state\">");
      html += htmlEscape(g_cached_state);
      html += F("</span><span class=\"cups\">");
      html += String(g_cached_cups, 1);
      html += F(" 杯</span></div>");
      html += F("<div class=\"reason\">");
      html += htmlEscape(g_cached_reason);
      html += F("</div>");

      struct tm tm{};
      char ts[32] = "?";
      if (localtime_r(&g_cached_epoch, &tm)) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
      }
      html += F("<div class=\"meta\">confidence ");
      html += String(g_cached_confidence, 2);
      html += F(" &nbsp;|&nbsp; ");
      html += ts;
      html += F("</div>");
    } else {
      html += F("<p class=\"nodata\">まだ推論していません</p>");
    }

    html += F(
      "<div class=\"actions\">"
      "<a href=\"/analyze?ui=1\">推論を更新</a>"
      "<a class=\"warn\" href=\"/now?ui=1\">推論を更新 + Teams 投稿</a>"
      "</div>");

    // Posting channel
    html += F("<h2>Teams 投稿先</h2>");
    html += F("<p>現在: <b>");
    html += htmlEscape(g_channel);
    html += F("</b></p><div class=\"actions\">");
    if (g_channel == "test") {
      html += F("<a class=\"warn\" href=\"/channel?to=prod&ui=1\">本番チャネルに切替</a>");
    } else {
      html += F("<a href=\"/channel?to=test&ui=1\">テストチャネルに戻す</a>");
    }
    html += F("</div>");

    // Recent event log (newest first).
    html += F("<h2>最近のイベント</h2>");
    if (g_log_count == 0) {
      html += F("<p class=\"nodata\">まだイベントなし</p>");
    } else {
      html += F("<table class=\"log\">");
      // Iterate the ring buffer from newest to oldest
      const int n = (g_log_count < kLogSize) ? g_log_count : kLogSize;
      for (int i = 0; i < n; ++i) {
        const int idx = (g_log_next - 1 - i + kLogSize) % kLogSize;
        const LogEntry& e = g_log[idx];
        char ts[20] = "----- --:--:--";
        struct tm tm{};
        // Treat anything before 2020-01-01 (= 1577836800) as "NTP not yet synced"
        if (e.epoch > 1577836800 && localtime_r(&e.epoch, &tm)) {
          strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tm);
        }
        html += e.error ? F("<tr class=\"err\">") : F("<tr>");
        html += F("<td class=\"t\">");
        html += ts;
        html += F("</td><td class=\"g\">");
        html += htmlEscape(e.tag);
        html += F("</td><td class=\"m\">");
        html += htmlEscape(e.message);
        html += F("</td></tr>");
      }
      html += F("</table>");
    }

    html += F("</body></html>");

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/last.jpg", HTTP_GET, []() {
    if (!g_cached_jpeg) {
      server.send(404, "text/plain", "no cached image yet\n");
      return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "image/jpeg",
                  reinterpret_cast<const char*>(g_cached_jpeg),
                  g_cached_jpeg_len);
  });
  // Manual: post the current level to Teams (does not touch state or event detection).
  server.on("/now", HTTP_GET, []() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      server.send(500, "text/plain", "capture failed\n");
      return;
    }
    uint8_t* jpeg = static_cast<uint8_t*>(ps_malloc(fb->len));
    const size_t jpeg_len = fb->len;
    if (!jpeg) {
      esp_camera_fb_return(fb);
      server.send(500, "text/plain", "ps_malloc failed\n");
      return;
    }
    memcpy(jpeg, fb->buf, jpeg_len);
    esp_camera_fb_return(fb);

    CoffeeEstimate est = geminiAnalyze(jpeg, jpeg_len);
    if (!est.ok) {
      free(jpeg);
      String msg = String("analyze failed: ") + est.error + "\n";
      server.send(502, "text/plain", msg);
      return;
    }

    const String cups_str = String(est.cups_remaining, 1);
    String body;
    if (est.state == "empty") {
      body = String("空です  — ") + est.reason;
    } else if (est.state == "full") {
      body = String("ほぼ満タン (約 ") + cups_str + " 杯)  — " + est.reason;
    } else {
      body = String("約 ") + cups_str + " 杯  — " + est.reason;
    }

    const bool ok = teamsPostThumb("☕ 現在のコーヒー残量", body.c_str(),
                                   jpeg, jpeg_len, 640, 480);
    free(jpeg);

    if (server.hasArg("ui")) {
      server.sendHeader("Location", "/");
      server.send(303, "text/plain", ok ? "posted, redirecting\n"
                                        : "post failed, redirecting\n");
      return;
    }

    JsonDocument out;
    out["posted"]         = ok;
    out["state"]          = est.state;
    out["cups_remaining"] = est.cups_remaining;
    out["confidence"]     = est.confidence;
    out["reason"]         = est.reason;
    String resp;
    serializeJsonPretty(out, resp);
    resp += "\n";
    server.send(ok ? 200 : 502, "application/json", resp);
  });

  server.on("/check", HTTP_GET, []() {
    runCheck(true);  // Manual run bypasses the active-hours check
    JsonDocument out;
    out["last_state"]    = g_last_state;
    out["last_cups"]     = g_last_cups;
    out["brew_epoch"]    = static_cast<long>(g_brew_epoch);
    out["status_sent"]   = g_status_sent;
    out["active_now"]    = isActiveNow();
    String body;
    serializeJsonPretty(out, body);
    body += "\n";
    server.send(200, "application/json", body);
  });
  server.on("/state", HTTP_GET, []() {
    JsonDocument out;
    out["last_state"]  = g_last_state;
    out["last_cups"]   = g_last_cups;
    out["brew_epoch"]  = static_cast<long>(g_brew_epoch);
    out["status_sent"] = g_status_sent;
    out["active_now"]  = isActiveNow();
    String body;
    serializeJsonPretty(out, body);
    body += "\n";
    server.send(200, "application/json", body);
  });
  server.on("/reset-state", HTTP_GET, []() {
    g_last_state  = "";
    g_last_cups   = -1.0f;
    g_brew_epoch  = 0;
    g_status_sent = false;
    server.send(200, "text/plain", "state cleared (RAM only)\n");
  });
  server.on("/channel", HTTP_GET, []() {
    if (server.hasArg("to")) {
      const String to = server.arg("to");
      if (to != "test" && to != "prod") {
        server.send(400, "text/plain", "to must be 'test' or 'prod'\n");
        return;
      }
      const String prev = g_channel;
      saveChannel(to);
      logEvent("CONFIG", String("channel ") + prev + " -> " + to, false);
      if (server.hasArg("ui")) {
        server.sendHeader("Location", "/");
        server.send(303, "text/plain", "switched\n");
        return;
      }
    }
    JsonDocument out;
    out["channel"] = g_channel;
    out["url"]     = teamsUrl();
    String body;
    serializeJsonPretty(out, body);
    body += "\n";
    server.send(200, "application/json", body);
  });

  server.on("/jpg", HTTP_GET, handleJpg);
  // Debug: capture a JPEG, downscale it, and return (same output as the Teams thumbnail)
  server.on("/thumb.jpg", HTTP_GET, []() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      server.send(500, "text/plain", "capture failed");
      return;
    }
    size_t thumb_len = 0;
    uint8_t* thumb = jpegMakeThumb(fb->buf, fb->len, fb->width, fb->height, 25, &thumb_len);
    esp_camera_fb_return(fb);
    if (!thumb) {
      server.send(500, "text/plain", "thumb failed");
      return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "image/jpeg", reinterpret_cast<const char*>(thumb), thumb_len);
    free(thumb);
  });
  server.on("/analyze", HTTP_GET, []() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      server.send(500, "text/plain", "capture failed");
      return;
    }
    CoffeeEstimate est = geminiAnalyze(fb->buf, fb->len);
    esp_camera_fb_return(fb);

    if (server.hasArg("ui")) {
      server.sendHeader("Location", "/");
      server.send(303, "text/plain", est.ok ? "analyzed, redirecting\n"
                                            : "analyze failed, redirecting\n");
      return;
    }

    JsonDocument out;
    out["ok"] = est.ok;
    if (est.ok) {
      out["cups_remaining"] = est.cups_remaining;
      out["state"]          = est.state;
      out["confidence"]     = est.confidence;
      out["reason"]         = est.reason;
    } else {
      out["error"] = est.error;
    }
    String body;
    serializeJsonPretty(out, body);
    body += "\n";
    server.send(est.ok ? 200 : 500, "application/json", body);
  });

  server.on("/post", HTTP_GET, []() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      server.send(500, "text/plain", "capture failed");
      return;
    }
    const bool ok = teamsPostThumb("Coffee Watcher テスト投稿",
                                   "ESP32-CAM からのテスト投稿です。",
                                   fb->buf, fb->len, fb->width, fb->height);
    esp_camera_fb_return(fb);
    server.send(ok ? 200 : 500, "text/plain",
                ok ? "posted OK\n" : "post FAILED\n");
  });
  server.begin();
  Serial.printf("[http] http://%s/\n", WiFi.localIP().toString().c_str());
  logEvent("HTTP", String("server up at http://") + WiFi.localIP().toString() + "/", false);
  blinkAck(4);  // HTTP server up — log is browsable from a browser from here on

  // Boot notification (current camera image + IP / RSSI).
  // Fatal init failures never reach here, so this also serves as proof of a successful boot.
  if (camera_fb_t* fb = esp_camera_fb_get()) {
    uint8_t* jpeg = static_cast<uint8_t*>(ps_malloc(fb->len));
    const size_t jpeg_len = fb->len;
    if (jpeg) {
      memcpy(jpeg, fb->buf, jpeg_len);
      esp_camera_fb_return(fb);

      char ts[32] = "";
      struct tm tm{};
      if (getLocalTime(&tm, 1000)) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
      }
      String body;
      body  = String("IP: http://") + WiFi.localIP().toString() + "/\n";
      body += String("RSSI: ") + WiFi.RSSI() + " dBm\n";
      body += String("MAC: ") + WiFi.macAddress() + "\n";
      if (ts[0]) body += String("起動時刻: ") + ts;
      const bool ok = teamsPostThumb("🟢 Coffee Watcher 起動",
                                     body.c_str(), jpeg, jpeg_len, 640, 480);
      Serial.printf("[boot] notification %s\n", ok ? "posted" : "FAILED");
      logEvent("BOOT", ok ? "notification posted" : "notification FAILED", !ok);
      free(jpeg);
      blinkAck(5);  // Boot sequence complete (regardless of notification success)
    } else {
      esp_camera_fb_return(fb);
      Serial.println("[boot] notification: ps_malloc failed");
    }
  }
}

void loop() {
  server.handleClient();
  const uint32_t now = millis();

  // Pipeline: first run 10s after boot, then every kCheckIntervalMs.
  static uint32_t next_check = kFirstCheckDelayMs;
  if (now >= next_check) {
    next_check = now + kCheckIntervalMs;
    runCheck();
  }

  // Alive log every 30s.
  static uint32_t next_tick = 0;
  if (now >= next_tick) {
    next_tick = now + 30000;
    Serial.printf("[alive] wifi=%s  RSSI=%d dBm  heap=%u  psram_free=%u  last=%s/%.2f\n",
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  WiFi.RSSI(), ESP.getFreeHeap(), ESP.getFreePsram(),
                  g_last_state.c_str(), g_last_cups);
  }

  // LED heartbeat (non-blocking). We ideally want it to stay on during runCheck,
  // but for simplicity we let this loop run normally — and since runCheck is not
  // on a separate task, this loop doesn't execute during it anyway.
  // Slow heartbeat: ~10 s cycle, 50 ms ON.
  static uint32_t last_blink = 0;
  static bool led_on = false;
  const uint32_t blink_ms = led_on ? 50 : 9950;
  if (now - last_blink >= blink_ms) {
    led_on = !led_on;
    digitalWrite(kLedPin, led_on ? LOW : HIGH);
    last_blink = now;
  }
}

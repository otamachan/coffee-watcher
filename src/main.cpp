#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
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

static constexpr int kLedPin = 33;  // Freenove 基板上の青 LED (GPIO33, LOW=ON)
static constexpr const char* kTzInfo = "JST-9";
static constexpr const char* kNtp1 = "ntp.nict.jp";
static constexpr const char* kNtp2 = "pool.ntp.org";

// パイプライン設定
static constexpr uint32_t kCheckIntervalMs = 5UL * 60 * 1000;  // 5 min
static constexpr uint32_t kFirstCheckDelayMs = 10000;          // boot 後 10s
static constexpr float    kMinConfidence = 0.5f;
static constexpr time_t   kStatusDelaySec = 30 * 60;            // 新規ブリュー後 30 分で残量通知

// アクティブ時間: 平日 9:00 (inclusive) - 18:00 (exclusive) JST
static constexpr int kActiveStartHour = 9;
static constexpr int kActiveEndHour   = 18;

// 状態 (RAM のみ。再起動でリセットされる前提)。
//   g_last_state  : 直前の est.state ("empty" | "partial" | "full" | "")
//   g_last_cups   : 直前の est.cups_remaining
//   g_brew_epoch  : 直近の「空→非空」検知時の epoch (0 = 現在ブリュー無し or 未投稿)
//   g_status_sent : 現ブリューの 30 分残量通知を投稿済みか
static String  g_last_state;
static float   g_last_cups   = -1.0f;
static time_t  g_brew_epoch  = 0;
static bool    g_status_sent = false;

// 最後に Gemini に解析を依頼した画像と結果のキャッシュ (HTTP / ダッシュボード用)。
static uint8_t* g_cached_jpeg     = nullptr;
static size_t   g_cached_jpeg_len = 0;
static time_t   g_cached_epoch    = 0;
static String   g_cached_state;
static float    g_cached_cups       = 0.0f;
static float    g_cached_confidence = 0.0f;
static String   g_cached_reason;

// 最近のイベントログ (リングバッファ、シリアルに繋がなくてもダッシュボードで見れる)。
struct LogEntry {
  time_t epoch = 0;
  String tag;       // BOOT / WIFI / NTP / TEAMS / GEMINI / CHECK / EVENT
  String message;
  bool   error = false;
};
static constexpr int kLogSize = 16;
static LogEntry g_log[kLogSize];
static int      g_log_next = 0;   // 次に書き込むインデックス
static int      g_log_count = 0;  // 累計 (上限なし、表示は最新 kLogSize 件)

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

// フェーズ完了の合図: 短い点滅 N 回 + 待ち。
// 1=Camera, 2=Wi-Fi, 3=NTP, 4=HTTP, 5=Boot 通知完了 (起動シーケンス完了)。
static void blinkAck(int times) {
  for (int i = 0; i < times; ++i) {
    digitalWrite(kLedPin, LOW);
    delay(70);
    digitalWrite(kLedPin, HIGH);
    delay(120);
  }
  delay(400);  // 次のフェーズと視覚的に区別するための間
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
      digitalWrite(kLedPin, HIGH);  // 消灯して終了
      return false;
    }
    // 接続中は速い点滅 (50ms ON / 200ms OFF) で「探索中」を視覚化
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
  cfg.frame_size   = FRAMESIZE_CIF;       // 352x288 (Teams payload を小さく保つ)
  cfg.jpeg_quality = 15;
  cfg.fb_count     = 2;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[cam] init FAILED: 0x%x\n", err);
    return false;
  }

  // センサ姿勢: 左右反転を反転させる。OV3660 のデフォルト動作確認も兼ねる。
  if (sensor_t* s = esp_camera_sensor_get()) {
    s->set_hmirror(s, 1);  // ← 0 から 1 に切り替えて比較
    s->set_vflip(s, 0);
  }

  // ウォームアップ用に 1 枚捨てる。
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  sensor_t* s2 = esp_camera_sensor_get();
  Serial.printf("[cam] init OK  PID=0x%02x VER=0x%02x MIDH=0x%02x MIDL=0x%02x  frame=VGA q=10 fb=2 PSRAM\n",
                s2 ? s2->id.PID : 0, s2 ? s2->id.VER : 0,
                s2 ? s2->id.MIDH : 0, s2 ? s2->id.MIDL : 0);
  return true;
}

// HTML 表示用エスケープ。
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

// JSON 文字列に埋め込む前のエスケープ。" と \ と制御文字をエスケープ。
static String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '"')       out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else out += c;
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

// 直近の解析結果と画像を PSRAM にキャッシュ (HTTP `/` 用)。
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

// Gemini 2.0 Flash の generateContent に JPEG + プロンプトを投げ、
// responseSchema 経由で構造化 JSON を取り出す。
static CoffeeEstimate geminiAnalyze(const uint8_t* jpeg, size_t jpeg_len) {
  CoffeeEstimate r;

  // 1. base64 エンコード (PSRAM)
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

  // 2. リクエスト JSON を PSRAM に組み立て
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
    // 短い要約だけログ化 (429 の "Please retry in X" 部分など)
    String snippet = resp;
    snippet.replace("\n", " ");
    if (snippet.length() > 120) snippet = snippet.substring(0, 117) + "...";
    logEvent("GEMINI", String("HTTP ") + code + " " + snippet, true);
    return r;
  }

  // 4. 外側 JSON をパース
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

  // 5. 内側構造化 JSON (responseSchema で保証されてる)
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

// JPEG を Teams Incoming Webhook へ inline base64 (data URI) で投稿。
// payload は PSRAM に確保して内部 RAM 圧迫を避ける。
static bool teamsPost(const char* title, const char* text,
                      const uint8_t* jpeg, size_t jpeg_len) {
  // 1. base64 エンコード
  size_t b64_olen = 0;
  mbedtls_base64_encode(nullptr, 0, &b64_olen, jpeg, jpeg_len);
  uint8_t* b64 = static_cast<uint8_t*>(ps_malloc(b64_olen + 1));
  if (!b64) {
    Serial.println("[teams] ps_malloc b64 failed");
    return false;
  }
  if (mbedtls_base64_encode(b64, b64_olen, &b64_olen, jpeg, jpeg_len) != 0) {
    Serial.println("[teams] base64 encode failed");
    free(b64);
    return false;
  }
  b64[b64_olen] = 0;

  // 2. JSON payload を組み立て (MessageCard)
  const size_t payload_cap = b64_olen + 1024;
  char* payload = static_cast<char*>(ps_malloc(payload_cap));
  if (!payload) {
    Serial.println("[teams] ps_malloc payload failed");
    free(b64);
    return false;
  }
  const String esc_title = jsonEscape(title);
  const String esc_text  = jsonEscape(text);
  const int n = snprintf(payload, payload_cap,
    "{\"@type\":\"MessageCard\","
     "\"@context\":\"http://schema.org/extensions\","
     "\"summary\":\"cam-watcher\","
     "\"themeColor\":\"0078D4\","
     "\"title\":\"%s\","
     "\"text\":\"%s\","
     "\"sections\":[{\"images\":[{\"image\":\"data:image/jpeg;base64,%s\"}]}]"
    "}",
    esc_title.c_str(), esc_text.c_str(), reinterpret_cast<char*>(b64));
  free(b64);
  if (n < 0 || static_cast<size_t>(n) >= payload_cap) {
    Serial.printf("[teams] payload truncated (n=%d cap=%u)\n", n, (unsigned)payload_cap);
    free(payload);
    return false;
  }
  Serial.printf("[teams] payload=%d B (jpeg=%u B)\n", n, (unsigned)jpeg_len);

  // 3. HTTPS POST
  WiFiClientSecure client;
  client.setCACert(DIGICERT_GLOBAL_ROOT_G2);
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, TEAMS_WEBHOOK_URL)) {
    Serial.println("[teams] http.begin failed");
    free(payload);
    return false;
  }
  http.addHeader("Content-Type", "application/json; charset=utf-8");

  const uint32_t t0 = millis();
  const int code = http.POST(reinterpret_cast<uint8_t*>(payload), n);
  const uint32_t dt = millis() - t0;
  const String resp = http.getString();
  http.end();
  free(payload);

  // raw body を hex まで出して原因切り分けを容易にする (空白・改行検知用)。
  Serial.printf("[teams] HTTP %d  %u ms  resp_len=%u\n", code, dt, resp.length());
  Serial.print("[teams] resp(text): ");
  Serial.println(resp.substring(0, 200));
  Serial.print("[teams] resp(hex):  ");
  for (size_t i = 0; i < resp.length() && i < 32; ++i) {
    Serial.printf("%02X ", static_cast<unsigned char>(resp[i]));
  }
  Serial.println();

  // Incoming Webhook は配信成功時 body が "1" (Microsoft はたまに前後に
  // 空白/改行を付けてくる可能性があるので trim してから比較)。
  String trimmed = resp;
  trimmed.trim();
  const bool delivered = (code >= 200 && code < 300) && (trimmed == "1");
  if (!delivered && code >= 200 && code < 300) {
    Serial.println("[teams] HTTP 2xx but body != '1' -> delivery FAILED on Teams side");
  }

  if (delivered) {
    logEvent("TEAMS", String("posted (\"") + title + "\")", false);
  } else {
    String snippet = trimmed;
    if (snippet.length() > 120) snippet = snippet.substring(0, 117) + "...";
    char msg[200];
    snprintf(msg, sizeof(msg), "HTTP %d body=%s", code, snippet.c_str());
    logEvent("TEAMS", msg, true);
  }
  return delivered;
}

// 平日 (Mon-Fri) かつ 9:00-18:00 JST かを判定。NTP 未同期だと false。
static bool isActiveNow() {
  struct tm tm{};
  if (!getLocalTime(&tm, 0)) return false;
  if (tm.tm_wday < 1 || tm.tm_wday > 5) return false;       // 1=Mon..5=Fri
  return tm.tm_hour >= kActiveStartHour && tm.tm_hour < kActiveEndHour;
}

// 1 サイクル: 撮影 → Gemini 推定 → イベント判定 → Teams 投稿 → NVS 更新。
// force=true でアクティブ時間判定を無視する (手動 /check 用)。
static void runCheck(bool force = false) {
  if (!force && !isActiveNow()) {
    Serial.println("[check] outside active hours; skip");
    return;
  }
  Serial.println("[check] start");
  digitalWrite(kLedPin, LOW);  // 処理中は LED 点灯

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

  // 初回観測は無音で状態だけ記録 (起動時点が brew 途中の可能性があるため)
  if (first_run) {
    g_last_state = est.state;
    g_last_cups  = est.cups_remaining;
    Serial.println("[check] first observation, recorded silently");
    free(jpeg);
    digitalWrite(kLedPin, HIGH);
    return;
  }

  // 3 イベントを優先度順に判定
  bool posted = false;

  if (was_empty && !now_empty) {
    // (1) 新規ブリュー検知: 空 → 非空
    const String text = String("約 ") + cups_str + " 杯  — " + est.reason;
    if (teamsPost("☕ 新しくコーヒーがはいりました！", text.c_str(), jpeg, jpeg_len)) {
      g_brew_epoch  = now_t;
      g_status_sent = false;
      posted = true;
      logEvent("EVENT", "BREWED posted", false);
    } else {
      logEvent("EVENT", "BREWED post failed; will retry next cycle", true);
    }
  } else if (!was_empty && now_empty) {
    // (3) 空検知: 非空 → 空
    const String text = String("残量ゼロ  — ") + est.reason;
    if (teamsPost("☕ コーヒーがなくなりました！", text.c_str(), jpeg, jpeg_len)) {
      g_status_sent = true;   // 念のため (status 重複防止)
      posted = true;
      logEvent("EVENT", "EMPTIED posted", false);
    } else {
      logEvent("EVENT", "EMPTIED post failed; will retry next cycle", true);
    }
  } else if (g_brew_epoch > 0 && !g_status_sent &&
             !now_empty && now_t >= g_brew_epoch + kStatusDelaySec) {
    // (2) 30 分経過の残量通知: ブリュー後 30 分以上経過、まだ未投稿、現在空でない
    const String text = String("残り約 ") + cups_str + " 杯です  — " + est.reason;
    if (teamsPost("☕ コーヒー残量更新", text.c_str(), jpeg, jpeg_len)) {
      g_status_sent = true;
      posted = true;
      logEvent("EVENT", "STATUS posted", false);
    } else {
      logEvent("EVENT", "STATUS post failed; will retry next cycle", true);
    }
  } else {
    Serial.println("[check] no event");
  }

  // post 失敗で状態が変わるケースは更新しない (次サイクルで retry)。
  if (posted || (was_empty == now_empty)) {
    g_last_state = est.state;
    g_last_cups  = est.cups_remaining;
  } else {
    Serial.println("[check] post failed on transition; NOT updating last_state to allow retry");
  }

  free(jpeg);
  digitalWrite(kLedPin, HIGH);
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

  Serial.printf("\n=== cam-watcher boot ===\n");
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
      ".log td.t{color:#888;white-space:nowrap;width:7em}"
      ".log td.g{color:#555;white-space:nowrap;width:5em;font-weight:bold}"
      ".log tr.err td.g{color:#a02020}"
      ".log tr.err td.m{color:#a02020}"
      "</style></head><body>"
      "<h1>☕ Coffee Watcher</h1>");

    // 現在のライブカメラ画像 (cache buster 付き)。
    const uint32_t now_ms = millis();
    html += F("<h2>今の様子 (ライブ)</h2>");
    html += F("<img src=\"/jpg?t=");
    html += String(now_ms);
    html += F("\" alt=\"live\">");

    // 最後の推論結果。
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

    // 最近のイベントログ (新しい順)。
    html += F("<h2>最近のイベント</h2>");
    if (g_log_count == 0) {
      html += F("<p class=\"nodata\">まだイベントなし</p>");
    } else {
      html += F("<table class=\"log\">");
      // ring buffer の最新→過去の順で走査
      const int n = (g_log_count < kLogSize) ? g_log_count : kLogSize;
      for (int i = 0; i < n; ++i) {
        const int idx = (g_log_next - 1 - i + kLogSize) % kLogSize;
        const LogEntry& e = g_log[idx];
        char ts[16] = "--:--:--";
        struct tm tm{};
        // 2020-01-01 = 1577836800 以前は NTP 未同期とみなす
        if (e.epoch > 1577836800 && localtime_r(&e.epoch, &tm)) {
          strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
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
  // 手動: 今の残量を Teams に投稿 (NVS 状態には影響しない、イベント判定もしない)。
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

    const bool ok = teamsPost("☕ 現在のコーヒー残量", body.c_str(), jpeg, jpeg_len);
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
    runCheck(true);  // 手動はアクティブ時間外でも動かす
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
  server.on("/jpg", HTTP_GET, handleJpg);
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
    const bool ok = teamsPost("cam-watcher test",
                              "ESP32-CAM からのテスト投稿です。",
                              fb->buf, fb->len);
    esp_camera_fb_return(fb);
    server.send(ok ? 200 : 500, "text/plain",
                ok ? "posted OK\n" : "post FAILED\n");
  });
  server.begin();
  Serial.printf("[http] http://%s/\n", WiFi.localIP().toString().c_str());
  logEvent("HTTP", String("server up at http://") + WiFi.localIP().toString() + "/", false);
  blinkAck(4);  // HTTP server up — 以後はブラウザでログ閲覧可

  // 起動通知 (現在のカメラ画像 + IP / RSSI)。
  // 致命的な初期化失敗時はここに到達しないので、起動成功の証跡を兼ねる。
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
      const bool ok = teamsPost("🟢 cam-watcher 起動",
                                body.c_str(), jpeg, jpeg_len);
      Serial.printf("[boot] notification %s\n", ok ? "posted" : "FAILED");
      logEvent("BOOT", ok ? "notification posted" : "notification FAILED", !ok);
      free(jpeg);
      blinkAck(5);  // 起動シーケンス完了 (Boot 通知の成否は問わず)
    } else {
      esp_camera_fb_return(fb);
      Serial.println("[boot] notification: ps_malloc failed");
    }
  }
}

void loop() {
  server.handleClient();
  const uint32_t now = millis();

  // パイプライン: 起動 10 秒後に初回、その後 kCheckIntervalMs ごと。
  static uint32_t next_check = kFirstCheckDelayMs;
  if (now >= next_check) {
    next_check = now + kCheckIntervalMs;
    runCheck();
  }

  // 30 秒ごとに alive ログ。
  static uint32_t next_tick = 0;
  if (now >= next_tick) {
    next_tick = now + 30000;
    Serial.printf("[alive] wifi=%s  RSSI=%d dBm  heap=%u  psram_free=%u  last=%s/%.2f\n",
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  WiFi.RSSI(), ESP.getFreeHeap(), ESP.getFreePsram(),
                  g_last_state.c_str(), g_last_cups);
  }

  // LED ハートビート (non-blocking)。runCheck 中は LED 点灯のままにしたいが
  // 簡略化のためそのまま流して点滅させる (runCheck の間は別タスクではないので
  // この loop は走らない)。
  static uint32_t last_blink = 0;
  static bool led_on = false;
  const uint32_t blink_ms = led_on ? 50 : 950;
  if (now - last_blink >= blink_ms) {
    led_on = !led_on;
    digitalWrite(kLedPin, led_on ? LOW : HIGH);
    last_blink = now;
  }
}

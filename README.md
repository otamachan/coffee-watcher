# Coffee Watcher

A standalone coffee-level monitor that runs entirely on an ESP32-CAM. It
periodically photographs a drip-coffee carafe (coffee server), asks Google
Gemini Vision how much coffee is left, and posts a notification to a Microsoft
Teams channel whenever the state changes.

```
[ESP32-CAM]
   |  1) NTP sync (required for TLS certificate validation)
   |  2) Capture JPEG (VGA in PSRAM) on a timer
   |  3) Base64 encode and POST to Gemini's generateContent
   |     (responseSchema returns {cups_remaining, state, confidence, reason})
   |  4) Compare with the previous observation (kept in RAM)
   |  5) On BREWED / STATUS / EMPTIED events, post to the
   |     Microsoft Teams Incoming Webhook as a MessageCard
   v
[loop]
```

## Why everything on a single ESP32?

- Avoid running a PC or SBC continuously
- Skip cloud functions; the device is self-contained
- State is held in **RAM only** (occasional reboots are acceptable)
- Trade-off: prompt and threshold tweaks require a re-flash today
  (LittleFS-based hot reload is on the roadmap)

## Hardware (confirmed)

- **FREENOVE ESP32 WROVER (CAM)**
- ESP32-WROVER-E module (ESP32-D0WD-V3 rev 3, dual core @ 240 MHz)
- 4 MB Flash / **4 MB PSRAM** (~4.19 MB free after boot)
- USB-C direct connection / onboard CH340 USB-to-serial, auto reset
  (DTR → IO0 / RTS → EN)
- Camera sensor: **OV3660** (SCCB address `0x3C`)
- Serial port: `/dev/ttyUSB0` (VID:PID `1A86:7523`)

### Camera pin map (Freenove)

| Signal | GPIO |
|---|---|
| PWDN | -1 (not wired) |
| RESET | -1 |
| XCLK | 21 |
| SIOD (SDA) | 26 |
| SIOC (SCL) | 27 |
| Y2-Y9 | 4, 5, 18, 19, 36, 39, 34, 35 |
| VSYNC / HREF / PCLK | 25 / 23 / 22 |

## External services

- **Google Gemini API** (`gemini-flash-lite-latest`) — vision inference,
  runs comfortably within the free tier
  - Auth: `x-goog-api-key` header
  - TLS root: GTS Root R1
- **Microsoft Teams Incoming Webhook** — Adaptive Card / MessageCard posts
  - TLS root: DigiCert Global Root G2
  - Delivery confirmation: body must equal `"1"`
    (HTTP 200 is also returned on internal failures)

## Development environment

- OS: Ubuntu 24.04
- Tooling: PlatformIO Core 6.x (installed via the official installer at
  `~/.platformio/penv`)
- Framework: Arduino on the espressif32 platform

### First-time setup

1. Install PlatformIO Core (no sudo required)

   ```bash
   curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
   python3 /tmp/get-platformio.py
   ln -sf ~/.platformio/penv/bin/pio ~/.local/bin/pio
   ln -sf ~/.platformio/penv/bin/platformio ~/.local/bin/platformio
   pio --version
   ```

2. Grant access to the USB serial port (sudo, one time)

   ```bash
   sudo usermod -aG dialout $USER
   sudo curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
        -o /etc/udev/rules.d/99-platformio-udev.rules
   sudo udevadm control --reload-rules && sudo udevadm trigger
   ```

   Requires logging out and back in to take effect. For an immediate quick
   fix, you can temporarily run `sudo chmod a+rw /dev/ttyUSB0`.

3. Create `secrets.h` (not tracked by Git)

   ```bash
   cp src/secrets.h.example src/secrets.h
   # Edit to fill in Wi-Fi credentials, GEMINI_API_KEY, and TEAMS_WEBHOOK_URL_*
   ```

4. Verify the connection

   ```bash
   pio device list
   # /dev/ttyUSB0 should appear as "USB VID:PID=1A86:7523"
   ```

## Build, flash, and monitor

```bash
pio run                 # Build only
pio run -t upload       # Build + flash
pio device monitor      # Serial monitor (115200)
pio run -t clean        # Clean intermediates
```

The Freenove ESP32 WROVER auto-enters bootloader mode over USB-C; no manual
boot/reset jumper is needed.

## Runtime behavior

Boot sequence: print chip info → initialise the camera → connect to Wi-Fi →
sync NTP → post a boot notification to Teams.

After that, only during **weekdays 9:00–18:00 JST**, every **5 minutes**:

1. Capture a JPEG
2. Send it to Gemini and parse the structured output
   (cups_remaining / state / confidence / reason)
3. Skip if confidence < 0.5
4. Compare with the previous observation and decide whether to post
5. Post to Teams on a matching event

### Events

| Event | Trigger | Teams post title |
|---|---|---|
| BREWED | empty → non-empty | ☕ 新しくコーヒーがはいりました！ |
| STATUS | ≥ 30 min after BREWED (once per brew) | ☕ コーヒー残量更新 |
| EMPTIED | non-empty → empty | ☕ コーヒーがなくなりました！ |

### Teams channel switching

`secrets.h` holds two webhook URLs (`TEAMS_WEBHOOK_URL_TEST`,
`TEAMS_WEBHOOK_URL_PROD`). The active channel is stored in NVS
(namespace `cw-cfg`, key `channel`) and survives reboots. Switch from the
dashboard or via `GET /channel?to=test|prod`. Default is `test`.

### LED indication

A single blue LED (GPIO 33) acts as a boot progress indicator and heartbeat:

| Phase | Pattern |
|---|---|
| Boot → camera init | Off |
| Camera ready | 1 short blink |
| Wi-Fi connecting | Fast blink (50 ms on / 200 ms off) |
| Wi-Fi connected | 2 short blinks |
| NTP synced | 3 short blinks |
| HTTP server up | 4 short blinks |
| Boot notification posted | 5 short blinks |
| Idle in loop | Heartbeat (50 ms flash every 1 s) |
| Processing (analyze / Teams post) | Solid on |
| Fatal error | Rapid continuous blink |

### Dashboard / HTTP endpoints

Open `http://<ESP32-IP>/` to see the dashboard. It shows the latest live
image, the last analyzed snapshot and inference result, the current Teams
target channel, and a recent-event log (errors highlighted in red).

| Path | Purpose |
|---|---|
| `GET /` | Dashboard (auto-refreshes every 60 s) |
| `GET /jpg` | Fresh JPEG (no inference) |
| `GET /last.jpg` | Cached JPEG from the last Gemini call |
| `GET /thumb.jpg` | Downscaled Teams-style thumbnail (debug) |
| `GET /analyze[?ui=1]` | Capture + Gemini inference (no Teams post). `ui=1` redirects back to `/` |
| `GET /now[?ui=1]` | Capture + Gemini inference + Teams post. Same redirect option |
| `GET /check` | Run the full pipeline cycle once (debug; bypasses active hours) |
| `GET /state` | Current in-memory state as JSON |
| `GET /reset-state` | Clear in-memory state |
| `GET /channel[?to=test\|prod[&ui=1]]` | Show or switch the Teams target channel |
| `GET /post` | Fixed-text Teams test post (no Gemini) |

## Prompt

Defined as `kPrompt` in `src/main.cpp`, intentionally written in Japanese
since the response is rendered in Teams to Japanese-speaking users.
`responseSchema` pins the output shape:

```
ドリップ式コーヒーメーカーのコーヒーサーバー (ガラス製ポット) の画像です。
コーヒーの残量を推定して、スキーマに沿った JSON で返してください。
- cups_remaining: 杯数の推定値 (0.0=空、最大10.0、小数可)
- state: empty / partial / full
- confidence: 0.0〜1.0 (サーバーが写っていない場合は 0.3 未満)
- reason: 日本語で 1 文の理由
```

## Project layout

```
coffee-watcher/
├── platformio.ini      # board=esp32cam, enables PSRAM, ttyUSB0
├── src/
│   ├── main.cpp        # Firmware (capture / Gemini / Teams / HTTP / loop)
│   ├── cert.h          # GTS Root R1 (Gemini) and DigiCert G2 (Teams)
│   ├── secrets.h       # Secrets (Git-ignored)
│   └── secrets.h.example
└── README.md
```

## Required values in secrets.h

| Name | Purpose |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | Wi-Fi connection |
| `GEMINI_API_KEY` | Issued in Google AI Studio (`AIza...`) |
| `TEAMS_WEBHOOK_URL_TEST` | Webhook URL for the test channel |
| `TEAMS_WEBHOOK_URL_PROD` | Webhook URL for the production channel |

## Roadmap

- [x] PlatformIO setup
- [x] Wi-Fi + NTP
- [x] Camera init (OV3660, VGA JPEG)
- [x] Teams Webhook posts with inline base64 image
- [x] Gemini API connectivity + structured-output level estimation
- [x] Pipeline integration + 3-event detection (BREWED / STATUS / EMPTIED)
- [x] Active-hours gating (weekdays 9:00–18:00 JST)
- [x] HTTP dashboard with recent-event log
- [x] Teams-only thumbnail (320×240) to dodge Microsoft's payload limits
- [x] LED phase indication
- [x] Channel switching with NVS persistence
- [ ] LittleFS for hot-reloading prompt / thresholds
- [ ] Wi-Fi auto-reconnect / exponential backoff / WDT / heap monitoring

## Gotchas

- The Gemini free tier is rate-limited *per minute* (`gemini-flash-lite-latest`
  is generous; `gemini-2.5-flash` is much tighter). Continuous testing can
  hit 429. The 5-minute production cadence is well under the limit.
- The Microsoft Teams Incoming Webhook (Office 365 Connector) is being phased
  out and sometimes returns HTTP 200 with a delivery-failure body. Always
  check that the body equals `"1"`.
- Teams' Webhook rejects very large payloads intermittently. The firmware
  sends a 320×240 thumbnail (JPEG re-encoded after a 1/2 downscale) to Teams
  while keeping the original VGA frame for Gemini and the dashboard.
- A solid-color background (white sheet, etc.) behind the carafe dramatically
  improves Gemini's accuracy. Strong backlight (a window, curtain) washes out
  the coffee colour and triggers false "empty" readings.

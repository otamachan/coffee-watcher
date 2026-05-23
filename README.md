# cam-watcher

ESP32-CAM 単体で動くコーヒーメーカーのコーヒー残量ウォッチャー。
定期的にカラフェを撮影 → Claude Vision で残量推定 → 変化があれば Microsoft Teams に通知。

```
[ESP32-CAM]
   |  1) NTP 同期 (TLS 証明書検証に必須)
   |  2) インターバル毎にカメラキャプチャ (VGA JPEG, PSRAM)
   |  3) base64 化 → Anthropic Messages API へ HTTPS POST
   |     (tool_use で {cups_remaining, state, confidence, reason} を構造化出力)
   |  4) 前回値 (NVS) と比較
   |  5) 変化が閾値を超えたら Teams Webhook へ POST
   |  6) NVS に最新値を保存
   v
[loop]
```

## なぜ ESP32 単体か

- 常時稼働の PC・SBC を置きたくない
- クラウド関数も挟まず、デバイス 1 個で完結させる
- 代償: プロンプト/閾値の調整に再書き込み (or LittleFS 経由の動的更新機構) が要る

## ハードウェア (確認済み)

- **FREENOVE ESP32 WROVER (CAM)**
- ESP32-WROVER-E モジュール (ESP32-D0WD-V3 rev 3, 2 cores @ 240 MHz)
- Flash 4 MB / **PSRAM 4 MB** (起動時 free 約 4.19 MB)
- USB-C 直付け / CH340 USB-TTL オンボード, 自動リセット (DTR→IO0 / RTS→EN)
- カメラセンサ: **OV3660** (SCCB アドレス `0x3C`)
- シリアル: `/dev/ttyUSB0` (VID:PID `1A86:7523`)

### カメラ ピン定義 (Freenove)

| 信号 | GPIO |
|---|---|
| PWDN | -1 (未配線) |
| RESET | -1 |
| XCLK | 21 |
| SIOD (SDA) | 26 |
| SIOC (SCL) | 27 |
| Y2-Y9 | 4, 5, 18, 19, 36, 39, 34, 35 |
| VSYNC / HREF / PCLK | 25 / 23 / 22 |

## 開発環境

- OS: Ubuntu 24.04
- ツール: PlatformIO Core 6.x (公式インストーラ経由、`~/.platformio/penv`)
- フレームワーク: Arduino (espressif32 platform)

### 初回セットアップ

1. PlatformIO Core を導入 (sudo 不要)

   ```bash
   curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
   python3 /tmp/get-platformio.py
   ln -sf ~/.platformio/penv/bin/pio ~/.local/bin/pio
   ln -sf ~/.platformio/penv/bin/platformio ~/.local/bin/platformio
   pio --version
   ```

2. USB シリアルへのアクセス権 (要 sudo・1 回だけ)

   ```bash
   sudo usermod -aG dialout $USER
   sudo curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
        -o /etc/udev/rules.d/99-platformio-udev.rules
   sudo udevadm control --reload-rules && sudo udevadm trigger
   ```

   反映には再ログインが必要。即時に試したい場合は一時的に `sudo chmod a+rw /dev/ttyUSB0`。

3. 接続確認

   ```bash
   pio device list
   # /dev/ttyUSB0 に "USB VID:PID=1A86:7523" が出れば OK
   ```

## ビルド・書き込み・モニタ

```bash
pio run                 # ビルドのみ
pio run -t upload       # ビルド + 書き込み
pio device monitor      # シリアルモニタ (115200)
pio run -t clean        # 中間物クリア
```

Freenove ESP32 WROVER は USB-C 経由で自動リセット・自動書き込みモードに入る。手動の boot/reset 操作は不要。

## 現状の `src/main.cpp`

起動時にチップ情報・PSRAM 量を出力し、カメラ初期化 → Wi-Fi 接続 → NTP 同期。
平日 9:00-18:00 JST のみ 5 分ごとに「撮影 → Gemini で残量推定 → 前回比較 → 状態変化があれば Teams 投稿」。

### HTTP エンドポイント (ESP32)

| パス | 動作 |
|---|---|
| `GET /` , `/jpg` | 現在の JPEG を返す |
| `GET /analyze` | Gemini に投げて構造化結果を JSON で返す (Teams 投稿はしない) |
| `GET /post` | 現在の JPEG を Teams にテスト投稿 (LLM 介さず固定文言) |
| `GET /check` | 通常のパイプライン1サイクルを強制実行 (アクティブ時間外でも動く) |
| `GET /state` | 現在の NVS 状態を JSON で返す |
| `GET /reset-state` | NVS をクリア |

### イベント

| イベント | 条件 | メッセージ |
|---|---|---|
| BREWED | 空→非空 | ☕ 新しくコーヒーがはいりました！ |
| STATUS | BREWED から 30 分経過 | ☕ コーヒー残量更新 (残り約N杯です) |
| EMPTIED | 非空→空 | ☕ コーヒーがなくなりました！ |

## プロジェクト構成

```
cam-watcher/
├── platformio.ini   # ボード=esp32cam, PSRAM 有効化, ttyUSB0 設定
├── src/main.cpp     # 現状は診断スケッチ
├── include/         # ヘッダ (未使用)
├── lib/             # プロジェクトローカルライブラリ (未使用)
├── test/            # PlatformIO unit test (未使用)
└── README.md
```

## ロードマップ

- [x] 開発環境構築 (PlatformIO + プロジェクト初期化)
- [x] 診断スケッチでビルド通過
- [x] 実機書き込みでチップ情報・PSRAM 量を確認 (ESP32-D0WD-V3, PSRAM 4MB)
- [x] Wi-Fi 接続 + NTP 同期 (JST, ntp.nict.jp/pool.ntp.org)
- [x] カメラ初期化と JPEG キャプチャ (OV3660, VGA ~9KB/frame)
- [x] Teams Webhook へ画像付き投稿 (MessageCard + data URI base64)
- [x] Gemini API 疎通 (GTS Root R1, gemini-2.5-flash)
- [x] Gemini Vision で残量推定 (responseSchema で構造化出力)
- [x] パイプライン統合 + 3 イベント検知 (BREWED / STATUS / EMPTIED, 状態は RAM のみ)
- [x] アクティブ時間制限 (平日 9:00-18:00 JST)
- [ ] LittleFS でプロンプト・閾値を動的更新
- [ ] Wi-Fi 再接続 / 指数バックオフ / WDT / heap 監視

## 設定すべき値 (実装時に必要)

| 項目 | 用途 |
|---|---|
| Wi-Fi SSID / パスワード | Wi-Fi 接続 |
| `ANTHROPIC_API_KEY` | Claude Messages API |
| Teams Webhook URL | 投稿先 (Incoming Webhook or Power Automate URL) |
| 満タン杯数 | プロンプトで残量レンジの上限に使う |
| ポーリング間隔 | 推奨 5〜15 分。コストと検知頻度のトレードオフ |

機密値は `src/secrets.h` を作って書き、Git には含めない (`.gitignore` 済み)。

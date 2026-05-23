# Coffee Watcher

ESP32-CAM 単体で動くコーヒーメーカーの残量ウォッチャー。
定期的にコーヒーサーバーを撮影 → Google Gemini Vision で残量推定 → 状態変化があれば Microsoft Teams に通知。

```
[ESP32-CAM]
   |  1) NTP 同期 (TLS 証明書検証に必須)
   |  2) インターバル毎にカメラキャプチャ (VGA JPEG, PSRAM)
   |  3) base64 化 → Gemini API (generateContent) へ HTTPS POST
   |     (responseSchema で {cups_remaining, state, confidence, reason})
   |  4) 前回観測 (RAM 保持) と比較しイベントを判定
   |  5) BREWED / STATUS / EMPTIED のいずれかに該当すれば
   |     Teams Incoming Webhook へ MessageCard で投稿
   v
[loop]
```

## なぜ ESP32 単体か

- 常時稼働の PC や SBC を置きたくない
- クラウド関数も挟まず、デバイス 1 個で完結
- 状態は **RAM のみ**で保持 (再起動時はリセット、再起動は稀という割り切り)
- 代償: プロンプト/閾値の調整に再書き込みが要る (将来 LittleFS で動的化したい)

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

## 外部サービス

- **Google Gemini API** (`gemini-flash-lite-latest`) — vision 推論、無料枠で運用
  - 認証: `x-goog-api-key` ヘッダ
  - TLS root: GTS Root R1
- **Microsoft Teams Incoming Webhook** — Adaptive Card / MessageCard 投稿
  - TLS root: DigiCert Global Root G2
  - 配信成功判定: body == `"1"` (HTTP 200 は配信失敗時も返る)

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

3. `secrets.h` を作成 (Git 管理外)

   ```bash
   cp src/secrets.h.example src/secrets.h
   # 編集して Wi-Fi / GEMINI_API_KEY / TEAMS_WEBHOOK_URL を埋める
   ```

4. 接続確認

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

## 実行時の動作

起動シーケンス: チップ情報出力 → カメラ初期化 → Wi-Fi 接続 → NTP 同期 → 起動通知を Teams へ投稿。

その後 **平日 9:00-18:00 JST** のみ、**5 分間隔**で以下を繰り返す:

1. JPEG キャプチャ
2. Gemini に投げて構造化出力 (cups_remaining / state / confidence / reason) を取得
3. confidence < 0.5 はスキップ
4. 直前観測と比較しイベント判定
5. 該当イベントがあれば Teams 投稿

### イベント

| イベント | 条件 | Teams 投稿タイトル |
|---|---|---|
| BREWED | 空 → 非空 | ☕ 新しくコーヒーがはいりました！ |
| STATUS | BREWED から 30 分経過 (1 ブリューにつき 1 回) | ☕ コーヒー残量更新 |
| EMPTIED | 非空 → 空 | ☕ コーヒーがなくなりました！ |

### HTTP エンドポイント

ブラウザは `http://<ESP32-IP>/` を開けば OK。

| パス | 用途 |
|---|---|
| `GET /` | **ダッシュボード**: ライブ画像 + 最後の推論結果 + 操作ボタン (60s 自動リロード) |
| `GET /jpg` | 撮りたて JPEG を返す (推論なし) |
| `GET /last.jpg` | 最後に推論したときの JPEG (キャッシュ) |
| `GET /analyze[?ui=1]` | 撮影 + Gemini 推論 (Teams 投稿なし)。`ui=1` で `/` にリダイレクト |
| `GET /now[?ui=1]` | 撮影 + Gemini 推論 + Teams 投稿。同上 |
| `GET /check` | 自動ループと同じ処理を 1 回強制実行 (デバッグ用) |
| `GET /state` | 現在の RAM 状態を JSON で返す |
| `GET /reset-state` | RAM 状態をクリア |
| `GET /post` | 固定文言を Teams にテスト投稿 (Gemini を介さない) |

## プロンプト

`src/main.cpp` 内の `kPrompt` に日本語で記述。`responseSchema` でフィールドを固定:

```
ドリップ式コーヒーメーカーのコーヒーサーバー (ガラス製ポット) の画像です。
コーヒーの残量を推定して、スキーマに沿った JSON で返してください。
- cups_remaining: 杯数の推定値 (0.0=空、最大10.0、小数可)
- state: empty / partial / full
- confidence: 0.0〜1.0 (サーバーが写っていない場合は 0.3 未満)
- reason: 日本語で 1 文の理由
```

## プロジェクト構成

```
coffee-watcher/
├── platformio.ini      # board=esp32cam, PSRAM 有効化, ttyUSB0
├── src/
│   ├── main.cpp        # 本体 (キャプチャ / Gemini / Teams / HTTP / ループ)
│   ├── cert.h          # GTS Root R1 (Gemini) と DigiCert G2 (Teams)
│   ├── secrets.h       # 機密値 (Git 除外)
│   └── secrets.h.example
└── README.md
```

## secrets.h に必要な値

| 名前 | 用途 |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | Wi-Fi 接続 |
| `GEMINI_API_KEY` | Google AI Studio で発行 (`AIza...`) |
| `TEAMS_WEBHOOK_URL` | Teams チャネルの Incoming Webhook URL |

## ロードマップ

- [x] 開発環境構築 (PlatformIO)
- [x] Wi-Fi + NTP
- [x] カメラ初期化 (OV3660, VGA JPEG)
- [x] Teams Webhook へ画像付き投稿 (data URI base64)
- [x] Gemini API 疎通 + 残量推定 (構造化出力)
- [x] パイプライン統合 + 3 イベント検知 (BREWED / STATUS / EMPTIED)
- [x] アクティブ時間制限 (平日 9:00-18:00 JST)
- [x] HTTP ダッシュボード
- [ ] LittleFS でプロンプト / 閾値を再書き込みなしで更新
- [ ] Wi-Fi 再接続 / 指数バックオフ / WDT / heap 監視

## 注意点

- Gemini Free Tier は **1 分あたりのリクエスト数**で制限される (`gemini-flash-lite-latest` は緩め、`gemini-2.5-flash` は厳しめ)。テストで連続叩くと 429 が出やすい。5 分間隔運用なら通常問題なし
- Teams Incoming Webhook (Office 365 Connector) は廃止移行中。配信失敗時も HTTP 200 が返るので、body が `"1"` かを必ず確認すること
- カメラに対して **背景が単色** (白い紙など) のほうが Gemini の検知精度が大幅に上がる。窓・カーテンなど明暗差が強いと中身の液体色が飛んでハルシネーションの原因になる

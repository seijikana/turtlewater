# turtlewater — ESP32-DevKitC 亀水槽自動水替えシステム

## ファイル構成

```
turtlewater/
├── turtlewater.ino   # メインスケッチ
├── config.h          # ピン定義・定数
├── switchbot.h/.cpp  # SwitchBot API v1.1 (HTTPS + HMAC-SHA256)
├── webui.h           # Web UI HTML (PROGMEM)
└── README.md
```

---

## 1. Arduino IDE セットアップ

### ボードマネージャ追加 URL
`https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`

1. **ファイル → 環境設定** → 追加のボードマネージャURL に上記を貼り付け
2. **ツール → ボード → ボードマネージャ** → "esp32 by Espressif" をインストール (2.0.x 以上推奨)
3. **ツール → ボード** → "ESP32 Dev Module" を選択

### ボード設定

| 項目 | 値 |
|---|---|
| Flash Mode | DIO |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | Default 4MB with spiffs |
| CPU Frequency | 240MHz |
| Upload Speed | 921600 |

### 使用ライブラリ (すべてコア同梱 / 追加不要)

| ライブラリ | 用途 |
|---|---|
| WiFi.h | Wi-Fi 接続 |
| WebServer.h | HTTP サーバー |
| ESPmDNS.h | mDNS (`turtle.local`) |
| Preferences.h | 不揮発設定保存 |
| driver/i2s.h | MAX98357A I2S 音声出力 |
| mbedtls/md.h, mbedtls/base64.h | HMAC-SHA256 署名 |
| WiFiClientSecure.h, HTTPClient.h | HTTPS クライアント |

---

## 2. SwitchBot トークン取得方法

1. **SwitchBot アプリ** → プロフィール → 開発者向けオプション
2. 「トークンを取得」をタップ → **Token** と **Client Secret** をコピー
3. **デバイスID**: アプリ → 対象スマートプラグ → 歯車アイコン → デバイス情報 → "デバイスID" をコピー
   （または SwitchBot API `GET /v1.1/devices` で取得）

> **注意**: Token・Secret は再表示できません。必ず安全な場所に控えてください。

---

## 3. GPIO34/35 の外付けプルアップ抵抗について

GPIO34・35 は **入力専用**かつ **内部プルアップなし** です。  
将来的にこれらのピンへ水位センサーを変更する場合は、以下の接続が必要です。

```
3.3V ──┬── 10kΩ ──┬── GPIO34/35
       │           │
       │       センサー (NC 型)
       │           │
      GND ─────── GND
```

本スケッチでは GPIO32/33/23/19 を使用しており、これらは内部プルアップが使用できます。

---

## 4. 初回設定手順

### Step 1: フラッシュ書き込み
Arduino IDE で `turtlewater.ino` をコンパイル・書き込み。

### Step 2: AP モードで接続
Wi-Fi 設定が未保存の場合、デバイスは自動的に AP モードで起動します。

- AP SSID: `TurtleWater`（パスワードなし）
- Web UI: `http://192.168.4.1`

### Step 3: Wi-Fi 設定
Web UI 下部 **「Wi-Fi 設定」** セクションに SSID・パスワードを入力し「保存して再起動」。

### Step 4: SwitchBot 設定
再起動後、`http://turtle.local`（または IP アドレス）を開き、  
**「SwitchBot 設定」** に Token・Secret・Device ID を入力して保存。

### Step 5: タイムアウト確認
**「タイムアウト設定」** でデフォルト値（排水 300 秒・移送 300 秒・給水 600 秒）を確認・調整。

---

## 5. キャリブレーション手順

水位センサーの取り付け位置確認と動作検証を行います。

### センサー確認
1. Web UI を開き、センサーセクション（A_H / A_L / B_H / B_L）を確認
2. 各タライに手で水を注ぎ、センサーが 💧（水検知）に変わることを確認
3. 水を抜いて ⬜（未検知）に戻ることを確認

### 手動シーケンスによる動作確認
1. **手動制御** → `V2 ON` でバルブが開くことを確認（流水音）→ `V2 OFF`
2. **手動制御** → `V1 ON` で給水が始まることを確認 → `V1 OFF`
3. **手動制御** → `Pump ON` → SwitchBot スマートプラグが ON になることを確認 → `Pump OFF`
4. 問題がなければ 💧**給水** / 🚿**排水** の単体動作を試す
5. すべて正常であれば **▶ 自動** で全シーケンスを実行

### タイムアウト調整
- 排水が完了するのに T_DRAIN_MAX よりも短ければ問題なし
- タイムアウト超過でエラーになる場合は値を増やす

---

## 6. シリアルモニタによるデバッグ

ボーレート `115200` で接続するとログが確認できます。

```
[WiFi] Connected: 192.168.1.xxx
[mDNS] http://turtle.local
[HTTP] Server started on port 80
[SwitchBot] turnOn attempt 1 → 200
[WD] Overflow detected → drain
[Sched] slot 0 firing
```

---

## 7. 注意事項・安全対策

- **setup() 冒頭でリレーを OFF 初期化**しています。電源投入時にバルブ・ポンプが動作することはありません。
- **緊急停止ボタン（BTN4）** は ISR で即座に `stop_flag` をセットし、ループ内でバルブ・ポンプを遮断します。
- タイムアウト超過時は全出力 OFF + 異常音を鳴らして停止します。
- SwitchBot API が 3 回リトライ後も失敗した場合、シーケンスを中断して安全停止します。
- 水漏れ対策として、`T_FILL_MAX`（給水上限時間）は余裕を持って設定してください。

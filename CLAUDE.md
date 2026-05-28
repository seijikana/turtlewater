# turtlewater — 亀水槽自動水替えシステム

ESP32-DevKitC を使った亀水槽の自動水替えシステム。Arduino IDE でビルドする。

---

## ファイル構成

| ファイル | 役割 |
|---|---|
| `turtlewater.ino` | メイン。シーケンス・WebServer・FreeRTOS タスク・ISR |
| `config.h` | ピン番号・デフォルト値・定数 |
| `switchbot.h/cpp` | SwitchBot API v1.1 クライアント（ドリルポンプ制御） |
| `webui.h` | WebUI の HTML/CSS/JS（PROGMEM 文字列） |

---

## ハードウェア構成

### ピン配置

| GPIO | 用途 |
|---|---|
| 16 | V1 給水バルブ（リレー、OFF=HIGH） |
| 17 | V2 上→下移送バルブ（リレー、OFF=HIGH） |
| 25 | 圧電ブザー+（LEDC/PWM）、−→GND |
| 32 | AH センサー（上タライ 上限） |
| 33 | AL センサー（上タライ 下限） |
| 23 | BH センサー（下タライ 上限） |
| 19 | BL センサー（下タライ 下限） |
| 14 | BTN1 自動水替え |
| 18 | BTN2 強制給水 |
| 22 | BTN3 強制排水 |
| 13 | BTN4 緊急停止（最優先） |
| 21 | BTN5 再起動 |

### 水位センサー（XKC-Y25-NPN）
- INPUT_PULLUP
- `water_detected = digitalRead(pin) == HIGH`（水あり → HIGH）

### ドリルポンプ
- SwitchBot プラグ経由で制御（HTTPS API）
- `switchbot.pumpOn()` / `switchbot.pumpOff()`

### ブザー
- ESP32 Arduino Core 3.x の LEDC API を使用
- `ledcAttach(pin, freq, res)` / `ledcWriteTone(pin, freq)` / `ledcWrite(pin, duty)`
- ※ Core 2.x の `ledcSetup` / `ledcAttachPin` は使用不可

---

## FreeRTOS タスク構成

| タスク | コア | 優先度 | 役割 |
|---|---|---|---|
| `seq_task` | Core 1 | 2 | シーケンスキューを処理（run_auto/fill/drain） |
| `watchdog_sched_task` | Core 0 | 1 | 水位監視（2秒周期）＋スケジューラ（1分周期） |
| `loop()` | Core 1 | — | WebServer・ボタン処理・センサー更新 |

---

## 自動水替えシーケンス（run_auto）

```
Step 1/4: drain_tracking(T_DRAIN_MAX + T_TRANSFER_MAX)
          ポンプON + V2をALに追従、AL&BL 両方OFFで完了

Step 2/4: drain_tracking(T_TRANSFER_MAX)
          残留水の再排水

Step 3/4: fill_core()
          V1 ON → AL&BL 両方ONになってから T_FILL_EXTRA 秒追加給水
          安全停止: AH or BH で即完了

Step 4/4: drain_tracking(T_DRAIN_MAX + T_TRANSFER_MAX)
          仕上げ排水
```

---

## 共通給水ルーティン（fill_core）

`run_auto` Step 3 / `run_fill`（手動）/ `run_fill_low`（watchdog）の3箇所で共用。

```
Phase 1: V1 ON → AL & BL 両方 ON まで待機
Phase 2: そのまま T_FILL_EXTRA 秒追加給水
安全停止: AH or BH 検知で即完了（正常扱い）
```

---

## センサーデバウンス（sensors_update）

- 各センサーの生値が `T_SENSOR_DB` 秒間継続して同じ場合のみ `stable` を更新
- `water_detected(pin)` は常に `stable` を返す
- `sensors_update()` を各ポーリングループ先頭・`loop()`・`watchdog_sched_task()` で呼ぶ
- `T_SENSOR_DB=0` で即時反映（デバウンスなし）

---

## ボタン ノイズ対策（全5ボタン共通）

ドリルポンプの EMI ノイズで誤動作した実績あり。全ボタン同じ方式：

1. ISR で `g_btn_flag[n] = true` のみセット（直接アクションしない）
2. `process_buttons()` でフラグを検出後、`vTaskDelay(20ms)`
3. `digitalRead` で再確認 → LOW なら本物、HIGH なら `noise (ignored)` ログ

---

## Preferences キー

| 名前空間 | キー | 内容 |
|---|---|---|
| `timers` | `drain` / `transfer` / `fill` / `fill_extra` / `sensor_db` | タイムアウト秒数・デバウンス秒数 |
| `sched` | `s0e`〜`s3e` / `s0d`〜`s3d` / `s0h`〜`s3h` / `s0m`〜`s3m` | スケジュール4スロット |
| `switchbot` | `token` / `secret` / `device_id` | SwitchBot 認証情報 |
| `wifi` | `ssid` / `pass` | Wi-Fi 接続情報 |

---

## Web API

| メソッド | パス | 内容 |
|---|---|---|
| GET | `/api/status` | 状態・センサー・履歴 JSON |
| POST | `/api/start` | 自動水替え開始 |
| POST | `/api/fill` | 手動給水 |
| POST | `/api/drain` | 手動排水 |
| POST | `/api/stop` | 緊急停止 |
| POST | `/api/reboot` | 再起動 |
| GET/POST | `/api/timers` | タイムアウト設定 |
| GET/POST | `/api/sched` | スケジュール設定 |
| GET/POST | `/api/config` | SwitchBot 設定 |
| POST | `/api/wifi` | Wi-Fi 設定・再起動 |
| POST | `/api/m/{v1\|v2\|pump}/{0\|1}` | 手動バルブ・ポンプ操作 |

WebUI は `http://turtle.local`（mDNS）または IP アドレスでアクセス。
外出先からは Tailscale 経由（サブネットルーティング 192.168.0.0/24）で接続可能。

---

## Watchdog 動作

- **溢れ検知**（AH or BH ON）→ 排水（`run_drain`）をキューに投入
- **低水位検知**（AL AND BL 両方OFF）→ `run_fill_low` をキューに投入
  - 完了後2分間はクールダウン（連続トリガー防止）
- シーケンス実行中・キュー待ち中はスキップ

---

## 既知の問題・注意事項

- ドリルポンプの EMI ノイズ対策: ソフトウェアデバウンス実装済み＋GPIO13-GND 間に100nF コンデンサ追加済み
- ESP32 Arduino Core **3.x** 必須（LEDC API が 2.x と非互換）
- BTN2 は GPIO18（12はストラッピングピン）、BTN3 は GPIO22（15はストラッピングピン）
- SwitchBot API は最大3回リトライ、失敗時はシーケンス中断

# turtlewater — 亀水槽自動水替えシステム

ESP32-DevKitC を使った亀水槽の自動水替えシステム。Arduino IDE でビルドする。

---

## ファイル構成

| ファイル | 役割 |
|---|---|
| `turtlewater.ino` | メイン。シーケンス・WebServer・FreeRTOS タスク・ISR |
| `config.h` | ピン番号・デフォルト値・定数 |
| `switchbot.h/cpp` | SwitchBot API v1.1 クライアント（V3 給水弁プラグ制御） |
| `webui.h` | WebUI の HTML/CSS/JS（PROGMEM 文字列） |

---

## ハードウェア構成

### ピン配置

| GPIO | 用途 |
|---|---|
| 16 | V1 給水バルブ（電磁弁、リレー、OFF=HIGH、分岐ホースで上下タライ共通） |
| 17 | V2 上タライ排水弁（電動弁 三線式、リレーNO=開信号・NC=閉信号） |
| 4 | ポンプA 上タライ排水（ドリルポンプ、リレー） |
| 26 | ポンプB 下タライ排水（ドリルポンプ、リレー） |
| 25 | 圧電ブザー+（LEDC/PWM）、−→GND |
| 32 | AH センサー（上タライ 上限） |
| 33 | AL センサー（上タライ 下限） |
| 23 | BH センサー（下タライ 上限） |
| 19 | BL センサー（下タライ 下限） |
| 14 | BTN1 自動水替え |
| 18 | BTN2 強制給水 |
| 22 | BTN3 強制排水 |
| 13 | BTN4 緊急停止（最優先） |
| 21 | BTN5 V3給水（2匹目用） |

### 水位センサー（XKC-Y25-NPN）
- INPUT_PULLUP
- `water_detected = digitalRead(pin) == HIGH`（水あり → HIGH）

### V2 電動ボールバルブ（U.S. Solid 三線式 9-24V）— 上タライ排水弁
- リレー1個で制御。リレーNO → 開信号線、リレーNC → 閉信号線
- リレーON（コイル励磁）→ NO閉 → 開信号通電 → バルブ開
- リレーOFF（コイル非励磁）→ NC閉 → 閉信号通電 → バルブ閉
- 内蔵リミットスイッチが全開/全閉で自動停止するため、常時通電・コード変更不要
- ポンプA（上タライ）の吐出側、ポンプB の配管と合流する手前に設置。ポンプA 稼働中のみ開く
- 目的: 上タライ → 合流部 → ポンプB → 下タライ へのサイフォン逆流防止
  （2026-10-25 実機テストで逆流を確認し、共通排水出口弁から変更）
- ベランダ床へのサイフォンは、排水口を高くする配管側の対策で防ぐ（V2 は関与しない）
- ポンプA 稼働中・ポンプB 停止中は、合流部からポンプB 配管へ押し戻される可能性が残る
  （ポンプB 側の逆止弁で対策可能）

### ドリルポンプ（上下独立）
- ポンプA（GPIO4）= 上タライ、ポンプB（GPIO26）= 下タライ。GPIO リレー直結
- 必ず `pumps_request(a, b)` / `pumps_service()` 経由で制御する（直接 digitalWrite しない）
  - ポンプA 起動要求 → V2 開 → `T_V2_TRAVEL` 秒（全開待ち）後にポンプA ON
  - ポンプA 停止 → 即 V2 閉
  - ポンプB は V2 と無関係に即時 ON/OFF（結果として A/B の起動は `T_V2_TRAVEL` 秒ずれる）
  - ポンプA 起動の遅延反映のため `pumps_service()` を排水ループと `loop()` から定期的に呼ぶ
- Wi-Fi 不要のため、溢れ排水はネット断でも動作する

### V3 給水バルブ（2匹目リクガメ用）
- SwitchBot プラグ経由（旧ポンプ用プラグを転用、HTTPS API）
- `switchbot.plugOn()` / `switchbot.plugOff()`、ラッパーは `set_v3()`
- センサーなし・時間指定給水（`T_V3_FILL`）。閉止失敗は溢れに直結するため:
  - OFF 失敗時は `g_v3_off_pending` を立て、`v3_retry_service()`（loop）が10秒ごとに再送
  - 起動時は状態不明として OFF を送る（Wi-Fi/NTP 確立後）

### ブザー
- ESP32 Arduino Core 3.x の LEDC API を使用
- `ledcAttach(pin, freq, res)` / `ledcWriteTone(pin, freq)` / `ledcWrite(pin, duty)`
- ※ Core 2.x の `ledcSetup` / `ledcAttachPin` は使用不可

---

## FreeRTOS タスク構成

| タスク | コア | 優先度 | 役割 |
|---|---|---|---|
| `seq_task` | Core 1 | 2 | シーケンスキューを処理（run_auto/fill/drain/fill_low/fill_v3/drain_ovf） |
| `watchdog_sched_task` | Core 0 | 1 | 水位監視（2秒周期）＋スケジューラ（1分周期） |
| `loop()` | Core 1 | — | WebServer・ボタン処理・センサー更新・pumps_service・V3 OFF 再送 |

---

## 自動水替えシーケンス（run_auto）

```
Step 1/4: drain_tubs(AL, BL)   上下並行排水、AL OFF でポンプA・BL OFF でポンプB を個別停止
Step 2/4: fill_core()          V1 ON → AH or BH ON で停止（満水）
Step 3/4: drain_tubs(AL, BL)   仕上げ排水
Step 4/4: fill_low_core()      watchdog と同じ低水位補給で通常水位まで給水
```

---

## 共通排水ルーティン（drain_tubs）

`drain_tubs(sense_a, sense_b, extra_s, timeout_s)`

- タライごとに停止センサーを指定（-1 = そのタライは排水しない）。開始時に既に OFF なら排水しない
- 停止センサー OFF から `extra_s` 秒後にそのポンプだけ停止。一度止めたら再起動しない
- 両ポンプ停止で完了。タイムアウトは `T_DRAIN_MAX + T_V2_TRAVEL`
- 全排水: `drain_tubs(PIN_AL, PIN_BL, 0, T_DRAIN_MAX)`（`run_auto` / `run_drain`）
- 溢れ排水: `drain_tubs(AH or -1, BH or -1, T_OVF_EXTRA, T_DRAIN_MAX)`（`run_drain_ovf`）

---

## 給水ルーティン

**fill_core**（`run_auto` Step 2 / `run_fill` 手動）: V1 ON → AH or BH ON で停止（満水）

**fill_low_core**（`run_auto` Step 4 / `run_fill_low` watchdog）:
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
2. `process_buttons()` でフラグを検出後、`btn_confirm(pin)` で判定
3. `BTN_CONFIRM_MS`（50ms）の間 `BTN_SAMPLE_MS`（5ms）ごとに `digitalRead` →
   全サンプル LOW なら本物、1回でも HIGH なら即 `noise (ignored)` ログ

GPIO リレー化したポンプ運転中は全ボタンに数十ms間隔でノイズが入る（2026-10-25 実機ログで確認）。

---

## Preferences キー

| 名前空間 | キー | 内容 |
|---|---|---|
| `timers` | `drain` / `fill` / `fill_extra` / `sensor_db` / `v3_fill` / `v2_travel` / `ovf_extra` | タイムアウト・待ち時間・デバウンス秒数（旧 `transfer` は未使用） |
| `sched` | `en0`〜`en3` / `dow0`〜`dow3` / `h0`〜`h3` / `m0`〜`m3` | 自動水替えスケジュール4スロット |
| `schedv3` | （`sched` と同じ） | V3 給水スケジュール4スロット |
| `switchbot` | `token` / `secret` / `deviceId` | SwitchBot 認証情報（V3 給水弁プラグ） |
| `wifi` | `ssid` / `pass` | Wi-Fi 接続情報 |

---

## Web API

| メソッド | パス | 内容 |
|---|---|---|
| GET | `/api/status` | 状態・センサー・出力（v1/v2/pump_a/pump_b/v3）・履歴 JSON |
| POST | `/api/start` | 自動水替え開始 |
| POST | `/api/fill` | 手動給水 |
| POST | `/api/drain` | 手動排水（上下並行） |
| POST | `/api/fill_v3` | V3 給水 |
| POST | `/api/stop` | 緊急停止 |
| POST | `/api/reboot` | 再起動 |
| GET/POST | `/api/timers` | タイムアウト設定 |
| GET/POST | `/api/sched` | スケジュール設定 |
| GET/POST | `/api/sched_v3` | V3 スケジュール設定 |
| GET/POST | `/api/config` | SwitchBot 設定 |
| POST | `/api/wifi` | Wi-Fi 設定・再起動 |
| POST | `/api/m/{v1\|v2\|pump_a\|pump_b\|v3}/{0\|1}` | 手動バルブ・ポンプ操作（pump_a ON は V2 全開待ち後、v2 OFF はポンプA 稼働中なら開のまま） |

WebUI は `http://turtle.local`（mDNS）または IP アドレスでアクセス。

### 外出先からのアクセス（Tailscale）
- raspizero2（rasberrypizero2whlite、IPカメラ兼用、`ipcamera/SETUP.md` 参照）がサブネットルーターとして動作（2026-07-14〜。旧デスクトップPC desktop-a2ii0o2 は電気代節約のため退役）
- 広告サブネット: `192.168.1.0/24`
- ESP32 ローカルIP: `192.168.1.5`
- 設定コマンド（raspizero2側、SSH経由）:
  ```bash
  sudo tailscale up --advertise-routes=192.168.1.0/24 --accept-routes
  ```
- 管理画面でルート承認: https://login.tailscale.com/admin/machines
- iPhone 側: Tailscale アプリ → rasberrypizero2whlite →「Use Subnets」ON
- アクセスURL: `http://192.168.1.5/`
- 動作確認済み（スマホ → raspizero2 → ESP32）

---

## Watchdog 動作

- **溢れ検知**（AH or BH ON）→ 溢れたタライのみ `run_drain_ovf`（H OFF から `T_OVF_EXTRA` 秒後まで排水）
- **低水位検知**（AL OR BL どちらかOFF）→ `run_fill_low` をキューに投入
  - V1 は上下共通のため、両方の L が ON になるまで給水（AH or BH で安全停止）
  - シーケンス完了後2分間はクールダウン（連続トリガー防止）
- シーケンス実行中・キュー待ち中はスキップ

---

## 既知の問題・注意事項

- ドリルポンプの EMI ノイズ対策: ソフトウェアデバウンス実装済み＋GPIO13-GND 間に100nF コンデンサ追加済み
- ESP32 Arduino Core **3.x** 必須（LEDC API が 2.x と非互換）
- BTN2 は GPIO18（12はストラッピングピン）、BTN3 は GPIO22（15はストラッピングピン）
- SwitchBot API（V3）は最大3回リトライ、ON 失敗時はシーケンス中断、OFF 失敗時は loop で再送継続
- GPIO4 はリセット時に弱プルダウン。電源投入・リセット時にポンプAが一瞬動かないか要確認

#pragma once
#include <Arduino.h>

// ── Relay output pins (OFF = HIGH, ON = LOW) ─────────────────────────────────
#define PIN_V1          16    // 給水バルブ (I2S競合回避のためGPIO16)
#define PIN_V2          17    // 上→下移送バルブ (同上)
#define RELAY_ON        LOW
#define RELAY_OFF       HIGH

// ── Piezo buzzer (LEDC/PWM) ───────────────────────────────────────────────────
#define PIN_BUZZER      25    // 圧電ブザー+ (旧I2S LRC)  ブザー− → GND
#define BUZZER_LEDC_CH   0    // LEDC チャンネル

// ── Water level sensors (INPUT_PULLUP; LOW = water_detected) ─────────────────
#define PIN_AH          32    // 上タライ 上限
#define PIN_AL          33    // 上タライ 下限
#define PIN_BH          23    // 下タライ 上限
#define PIN_BL          19    // 下タライ 下限

// ── Push buttons (INPUT_PULLUP, FALLING edge ISR) ────────────────────────────
#define PIN_BTN1        14    // ①自動水替え
#define PIN_BTN2        18    // ②強制給水H  (GPIO12はストラッピングピンのため変更)
#define PIN_BTN3        22    // ③強制排水   (GPIO15はストラッピングピンのため変更)
#define PIN_BTN4        13    // ④緊急停止 (最優先)
#define PIN_BTN5        21    // ⑤再起動

#define DEBOUNCE_MS     300

// ── Default timeout values (seconds) ─────────────────────────────────────────
#define DEFAULT_T_DRAIN_MAX    300
#define DEFAULT_T_TRANSFER_MAX 300
#define DEFAULT_T_FILL_MAX     600
#define DEFAULT_T_FILL_EXTRA    20  // 低水位補給: AL&BL ON確認後の追加給水時間(秒)
#define DEFAULT_T_SENSOR_DB      2  // 水位センサーデバウンス時間(秒): この秒数継続後に状態確定

// ── History ring buffer ───────────────────────────────────────────────────────
#define HISTORY_MAX     20

// ── Scheduler ────────────────────────────────────────────────────────────────
#define SCHED_SLOTS     4

// ── Networking ───────────────────────────────────────────────────────────────
#define NTP_SERVER      "ntp.nict.jp"
#define NTP_TZ          "JST-9"
#define MDNS_NAME       "turtle"
#define AP_SSID         "TurtleWater"
#define AP_PASS         ""

// ── SwitchBot API ─────────────────────────────────────────────────────────────
#define SWITCHBOT_HOST  "api.switch-bot.com"
#define SWITCHBOT_PORT  443

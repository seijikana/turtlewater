/*
 * turtlewater.ino — ESP32-DevKitC 亀水槽自動水替えシステム
 *
 * Core 0: watchdog/scheduler タスク
 * Core 1: sequence タスク + WebServer (loop)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>
#include "config.h"
#include "switchbot.h"
#include "webui.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  Audio モジュール (圧電ブザー / LEDC-PWM)
// ═══════════════════════════════════════════════════════════════════════════════
namespace Audio {

static SemaphoreHandle_t _mtx = nullptr;

void init() {
    _mtx = xSemaphoreCreateMutex();
    ledcAttach(PIN_BUZZER, 2000, 8);      // pin, 初期周波数, 8bit分解能 (Core 3.x API)
    ledcWrite(PIN_BUZZER, 0);             // 無音
}

// 単音出力（ミューテックス保持中に呼ぶ）
static void _t(int freq, int dur_ms) {
    ledcWriteTone(PIN_BUZZER, freq);      // 周波数変更 (Core 3.x API)
    ledcWrite(PIN_BUZZER, 128);           // 50% デューティ
    vTaskDelay(pdMS_TO_TICKS(dur_ms));
    ledcWrite(PIN_BUZZER, 0);             // 無音
}
static void _s(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void beep_start() {
    if (!_mtx || xSemaphoreTake(_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return;
    _t(880, 120); _s(60); _t(880, 80);
    xSemaphoreGive(_mtx);
}
void beep_complete() {
    if (!_mtx || xSemaphoreTake(_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return;
    _t(523, 100); _s(40); _t(659, 100); _s(40); _t(784, 200);
    xSemaphoreGive(_mtx);
}
void beep_error() {
    if (!_mtx || xSemaphoreTake(_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return;
    _t(220, 350); _s(120); _t(220, 350);
    xSemaphoreGive(_mtx);
}
void beep_stop() {
    if (!_mtx || xSemaphoreTake(_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return;
    for (int i = 0; i < 3; i++) { _t(1500, 100); _s(60); }
    _t(1500, 400);
    xSemaphoreGive(_mtx);
}
void beep_watchdog() {
    if (!_mtx || xSemaphoreTake(_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return;
    for (int i = 0; i < 3; i++) { _t(1000, 80); _s(80); }
    xSemaphoreGive(_mtx);
}

} // namespace Audio

// ═══════════════════════════════════════════════════════════════════════════════
//  グローバル状態
// ═══════════════════════════════════════════════════════════════════════════════

volatile bool     g_running   = false;
volatile bool     g_stop_flag = false;
volatile int      g_step      = 0;
volatile int      g_progress  = 0;     // 0‒100
char              g_status[16] = "idle"; // "idle" / "running" / "error"

// タイムアウト (秒, Preferences から起動時にロード)
volatile uint32_t T_DRAIN_MAX;
volatile uint32_t T_TRANSFER_MAX;
volatile uint32_t T_FILL_MAX;
volatile uint32_t T_FILL_EXTRA;   // 低水位補給: AL&BL ON確認後の追加給水時間
volatile uint32_t T_SENSOR_DB;    // 水位センサーデバウンス時間(秒)

// ─── 実行履歴リングバッファ ──────────────────────────────────────────────────
struct HistEntry {
    time_t   ts;
    char     result[12];   // "completed" / "failed" / "stopped"
    uint32_t duration_sec;
    char     trigger[12];  // "auto" / "fill" / "drain" / "web" / "sched" / "watchdog"
};
static HistEntry         g_history[HISTORY_MAX];
static int               g_hist_head  = 0;
static int               g_hist_count = 0;
static SemaphoreHandle_t g_hist_mtx;

// ─── スケジューラ ─────────────────────────────────────────────────────────────
struct SchedSlot {
    bool    enabled;
    uint8_t dow_mask;  // bit0=日…bit6=土
    uint8_t hour;
    uint8_t minute;
};
static SchedSlot g_sched[SCHED_SLOTS];

// ─── シーケンスキュー ─────────────────────────────────────────────────────────
struct SeqReq {
    uint8_t type;          // 0=auto, 1=fill, 2=drain
    char    trigger[12];
};
static QueueHandle_t g_seq_queue;

// ─── ボタンフラグ (ISR → loop) ───────────────────────────────────────────────
static volatile bool     g_btn_flag[5] = {};
static volatile uint32_t g_btn_last[5] = {};

// ─── Web サーバー ─────────────────────────────────────────────────────────────
static WebServer server(80);

// ═══════════════════════════════════════════════════════════════════════════════
//  ハードウェアヘルパー
// ═══════════════════════════════════════════════════════════════════════════════

// ─── 水位センサーデバウンス ───────────────────────────────────────────────────
// T_SENSOR_DB 秒間継続して同じ生値が続いた場合のみ stable_state を更新する。
// T_SENSOR_DB=0 なら即時反映（デバウンスなし）。
struct SensorState {
    int      pin;
    bool     raw;          // 最後の生読み値
    bool     stable;       // 確定済み状態 (外部コードはこちらを参照)
    uint32_t changed_at;   // raw が変化した時刻 (millis)
};
static SensorState g_snsr[4] = {
    {PIN_AH, false, false, 0},
    {PIN_AL, false, false, 0},
    {PIN_BH, false, false, 0},
    {PIN_BL, false, false, 0},
};
static const char* snsr_name(int pin) {
    if (pin == PIN_AH) return "AH";
    if (pin == PIN_AL) return "AL";
    if (pin == PIN_BH) return "BH";
    if (pin == PIN_BL) return "BL";
    return "??";
}
// ポーリングループの先頭で呼ぶ (100ms 周期で十分)
static void sensors_update() {
    uint32_t now = millis();
    uint32_t db_ms = T_SENSOR_DB * 1000UL;
    for (int i = 0; i < 4; i++) {
        bool raw = (digitalRead(g_snsr[i].pin) == HIGH);
        if (raw != g_snsr[i].raw) {
            g_snsr[i].raw        = raw;
            g_snsr[i].changed_at = now;
        }
        if (g_snsr[i].raw != g_snsr[i].stable &&
            (now - g_snsr[i].changed_at) >= db_ms) {
            g_snsr[i].stable = g_snsr[i].raw;
            Serial.printf("[Sensor] %s → %s  (db=%lus)\n",
                snsr_name(g_snsr[i].pin),
                g_snsr[i].stable ? "ON" : "OFF",
                (unsigned long)T_SENSOR_DB);
        }
    }
}
// 起動時: 現在の実測値を raw/stable 両方に設定（誤トリガー防止）
static void sensors_init() {
    for (int i = 0; i < 4; i++) {
        bool v = (digitalRead(g_snsr[i].pin) == HIGH);
        g_snsr[i].raw = g_snsr[i].stable = v;
        g_snsr[i].changed_at = millis();
    }
}
// 全コードから呼ぶインターフェース: 確定済み状態を返す
inline bool water_detected(int pin) {
    for (int i = 0; i < 4; i++) {
        if (g_snsr[i].pin == pin) return g_snsr[i].stable;
    }
    return (digitalRead(pin) == HIGH); // フォールバック（通常到達しない）
}

void set_v1(bool on) { digitalWrite(PIN_V1, on ? RELAY_ON : RELAY_OFF); }
void set_v2(bool on) { digitalWrite(PIN_V2, on ? RELAY_ON : RELAY_OFF); }

bool pump_on()  { return switchbot.pumpOn();  }
bool pump_off() { return switchbot.pumpOff(); }

void all_off() {
    set_v1(false);
    set_v2(false);
    pump_off();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  履歴
// ═══════════════════════════════════════════════════════════════════════════════

void history_add(const char* trigger, const char* result, uint32_t dur) {
    xSemaphoreTake(g_hist_mtx, portMAX_DELAY);
    HistEntry& e = g_history[g_hist_head];
    e.ts           = time(nullptr);
    e.duration_sec = dur;
    strncpy(e.result,  result,  11);  e.result[11]  = '\0';
    strncpy(e.trigger, trigger, 11);  e.trigger[11] = '\0';
    g_hist_head = (g_hist_head + 1) % HISTORY_MAX;
    if (g_hist_count < HISTORY_MAX) g_hist_count++;
    xSemaphoreGive(g_hist_mtx);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  シーケンス共通ユーティリティ
// ═══════════════════════════════════════════════════════════════════════════════

// pin が target の water_detected 状態になるまで待機
// 戻り値: true=条件達成, false=タイムアウトまたは stop_flag
static bool wait_sensor(int pin, bool target, uint32_t timeout_s) {
    uint32_t deadline = millis() + timeout_s * 1000UL;
    while (true) {
        sensors_update();
        if (water_detected(pin) == target) return true;
        if (g_stop_flag)         return false;
        if (millis() > deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// (pinA==tA) OR (pinB==tB) を待機
// 戻り値: 1=A達成, 2=B達成, 0=タイムアウト/stop
static int wait_sensor_or(int pinA, bool tA, int pinB, bool tB, uint32_t timeout_s) {
    uint32_t deadline = millis() + timeout_s * 1000UL;
    while (true) {
        sensors_update();
        if (g_stop_flag)              return 0;
        if (millis() > deadline)      return 0;
        if (water_detected(pinA)==tA) return 1;
        if (water_detected(pinB)==tB) return 2;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// (pinA==tA) AND (pinB==tB) を待機
static bool wait_sensor_and(int pinA, bool tA, int pinB, bool tB, uint32_t timeout_s) {
    uint32_t deadline = millis() + timeout_s * 1000UL;
    while (true) {
        sensors_update();
        if (g_stop_flag)                               return false;
        if (millis() > deadline)                       return false;
        if (water_detected(pinA)==tA && water_detected(pinB)==tB) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void set_progress(int step, int total) {
    g_step     = step;
    g_progress = (int)(100.0f * step / total);
}

// ─── stop_flag チェック (ステップ境界で呼ぶ) ─────────────────────────────────
static bool check_stop() {
    if (g_stop_flag) { all_off(); return true; }
    return false;
}

// ─── 共通排水ルーティン ───────────────────────────────────────────────────────
// ポンプON + V2をA_Lに追従（水あり→V2開、空→V2閉）
// A_L AND B_L が両方空になったら正常終了
static bool drain_tracking(uint32_t timeout_s) {
    if (!pump_on()) {
        Serial.println("[Drain] pump_on FAILED");
        return false;
    }

    bool     prev_al  = water_detected(PIN_AL);
    bool     prev_bl  = water_detected(PIN_BL);
    uint32_t deadline = millis() + timeout_s * 1000UL;

    Serial.printf("[Drain] start  AL=%d BL=%d  timeout=%lus\n",
                  prev_al, prev_bl, (unsigned long)timeout_s);

    while (true) {
        sensors_update();
        if (g_stop_flag) {
            Serial.println("[Drain] STOP_FLAG");
            all_off(); return false;
        }
        if (millis() > deadline) {
            Serial.printf("[Drain] TIMEOUT  AL=%d BL=%d\n",
                          water_detected(PIN_AL), water_detected(PIN_BL));
            all_off(); return false;
        }

        bool al = water_detected(PIN_AL);
        bool bl = water_detected(PIN_BL);

        // センサー変化時のみログ
        if (al != prev_al || bl != prev_bl) {
            Serial.printf("[Drain] AL=%d BL=%d\n", al, bl);
            prev_al = al;
            prev_bl = bl;
        }

        set_v2(al); // V2 を A_L センサーに追従

        if (!al && !bl) break; // 両タライ空 → 完了
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    set_v2(false);
    pump_off();
    Serial.println("[Drain] done  AL&BL both OFF");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  シーケンス本体
// ═══════════════════════════════════════════════════════════════════════════════

bool run_auto() {
    const int TOTAL = 4;

    // Step 1: ポンプON + V2をA_Lに追従しながら両タライ排水
    set_progress(1, TOTAL);
    Serial.println("[Auto] Step 1/4 drain");
    if (!drain_tracking(T_DRAIN_MAX + T_TRANSFER_MAX)) return false;
    if (check_stop()) return false;

    // Step 2: ポンプON + V2追従で残留水を再排水 (AL&BL 両方OFF)
    set_progress(2, TOTAL);
    Serial.println("[Auto] Step 2/4 drain (residual)");
    if (!drain_tracking(T_TRANSFER_MAX)) return false;
    if (check_stop()) return false;

    // Step 3: V1 ON → AL&BL ON 後 T_FILL_EXTRA 秒給水 (安全停止: AH or BH)
    set_progress(3, TOTAL);
    Serial.println("[Auto] Step 3/4 fill");
    if (!fill_core()) { all_off(); return false; }
    if (check_stop()) return false;

    // Step 4: ポンプON + V2追従で仕上げ排水
    set_progress(4, TOTAL);
    Serial.println("[Auto] Step 4/4 drain (final)");
    if (!drain_tracking(T_DRAIN_MAX + T_TRANSFER_MAX)) return false;

    set_progress(0, TOTAL);
    return true;
}

bool run_fill() {
    g_step = 1; g_progress = 10;
    Serial.println("[Fill] manual start");
    bool ok = fill_core();
    g_progress = 100;
    return ok;
}

bool run_drain() {
    // AL & BL 両方OFF になるまでポンプ継続
    g_step = 1; g_progress = 10;
    Serial.println("[Drain] manual start");
    bool ok = drain_tracking(T_DRAIN_MAX + T_TRANSFER_MAX);
    g_progress = 100;
    return ok;
}

// ─── 共通給水ルーティン ───────────────────────────────────────────────────────
// Phase 1: V1 ON → AL & BL 両方 ON になるまで給水
// Phase 2: そのまま T_FILL_EXTRA 秒追加給水
// 安全停止: AH or BH 検知で即停止（上限に達したら水が十分なので正常完了扱い）
// 戻り値: true=正常完了, false=タイムアウト or stop_flag
static bool fill_core() {
    set_v1(true);
    uint32_t deadline       = millis() + T_FILL_MAX * 1000UL;
    bool     target_reached = false;
    uint32_t target_since   = 0;

    Serial.printf("[Fill] start  AH=%d BH=%d AL=%d BL=%d  extra=%lus\n",
        water_detected(PIN_AH), water_detected(PIN_BH),
        water_detected(PIN_AL), water_detected(PIN_BL),
        (unsigned long)T_FILL_EXTRA);

    while (true) {
        sensors_update();
        if (g_stop_flag)                                       { set_v1(false); return false; }
        if (millis() > deadline)                               { set_v1(false); return false; }
        if (water_detected(PIN_AH) || water_detected(PIN_BH)) { set_v1(false); return true;  } // 上限安全停止

        // Phase 1→2: AL & BL 両方ON で追加給水タイマー開始
        if (!target_reached && water_detected(PIN_AL) && water_detected(PIN_BL)) {
            target_reached = true;
            target_since   = millis();
            Serial.printf("[Fill] AL&BL ON → extra fill %lus\n", (unsigned long)T_FILL_EXTRA);
        }

        // Phase 2: 追加給水時間が経過したら完了
        if (target_reached && millis() - target_since >= T_FILL_EXTRA * 1000UL) break;

        vTaskDelay(pdMS_TO_TICKS(100));
    }
    set_v1(false);
    Serial.printf("[Fill] done  AH=%d BH=%d AL=%d BL=%d\n",
        water_detected(PIN_AH), water_detected(PIN_BH),
        water_detected(PIN_AL), water_detected(PIN_BL));
    return true;
}

bool run_fill_low() {
    g_step = 1; g_progress = 10;
    bool ok = fill_core();
    g_progress = 100;
    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  FreeRTOS タスク
// ═══════════════════════════════════════════════════════════════════════════════

// Core 1 — シーケンスキューを処理
void seq_task(void* pv) {
    SeqReq req;
    while (true) {
        if (xQueueReceive(g_seq_queue, &req, portMAX_DELAY) != pdTRUE) continue;

        g_running   = true;
        g_stop_flag = false;
        g_step      = 0;
        g_progress  = 0;
        strncpy(g_status, "running", 15);

        uint32_t t0 = millis();
        Audio::beep_start();

        bool ok;
        switch (req.type) {
            case 0: ok = run_auto();      break;
            case 1: ok = run_fill();      break;
            case 2: ok = run_drain();     break;
            default:ok = run_fill_low();  break; // 3: watchdog低水位補給
        }
        all_off(); // 安全フォールバック

        uint32_t dur = (millis() - t0) / 1000;
        const char* result = ok             ? "completed"
                           : g_stop_flag   ? "stopped"
                                           : "failed";
        history_add(req.trigger, result, dur);

        if      (ok)          Audio::beep_complete();
        else if (g_stop_flag) Audio::beep_stop();
        else                  Audio::beep_error();
        strncpy(g_status, ok ? "idle" : "error", 15);
        g_running  = false;
        g_step     = 0;
        g_progress = 0;
    }
}

// Core 0 — watchdog (2秒) + scheduler (1分)
void watchdog_sched_task(void* pv) {
    TickType_t last_sched = xTaskGetTickCount();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        sensors_update();

        // ── watchdog: 水位監視 ────────────────────────────────────────────────
        // fill_low 完了後、V1閉止で水面がセンサー付近まで下がる場合があるため
        // クールダウン期間中は低水位トリガーをスキップする
        static uint32_t fill_low_cd = 0; // クールダウン終了時刻 (millis)

        if (!g_running && uxQueueMessagesWaiting(g_seq_queue) == 0) {
            if (water_detected(PIN_AH) || water_detected(PIN_BH)) {
                // 溢れ優先 → 排水 (クールダウン解除)
                fill_low_cd = 0;
                Serial.println("[WD] Overflow detected → drain");
                Audio::beep_watchdog();
                SeqReq req; req.type = 2;
                strncpy(req.trigger, "watchdog", 11);
                xQueueSend(g_seq_queue, &req, 0);
            } else if (!water_detected(PIN_AL) && !water_detected(PIN_BL)) {
                if (millis() >= fill_low_cd) {
                    // 低水位 → 補給水 (A_L AND B_L 両方空)
                    fill_low_cd = millis() + 120000UL; // 完了後2分間はスキップ
                    Serial.println("[WD] Low water detected → fill_low");
                    Audio::beep_watchdog();
                    SeqReq req; req.type = 3;
                    strncpy(req.trigger, "watchdog", 11);
                    xQueueSend(g_seq_queue, &req, 0);
                } else {
                    Serial.printf("[WD] Low water (cooldown %lus left)\n",
                                  (unsigned long)((fill_low_cd - millis()) / 1000));
                }
            }
        }

        // ── scheduler: 1分ごとチェック ────────────────────────────────────────
        TickType_t now = xTaskGetTickCount();
        if ((uint32_t)(now - last_sched) < pdMS_TO_TICKS(60000)) continue;
        last_sched = now;

        if (g_running) continue;

        struct tm ti;
        if (!getLocalTime(&ti, 100)) continue;

        for (int i = 0; i < SCHED_SLOTS; i++) {
            SchedSlot& s = g_sched[i];
            if (!s.enabled)                          continue;
            if (ti.tm_hour   != s.hour)              continue;
            if (ti.tm_min    != s.minute)            continue;
            if (!((s.dow_mask >> ti.tm_wday) & 1))  continue;

            Serial.printf("[Sched] slot %d firing\n", i);
            SeqReq req;
            req.type = 0; // auto
            strncpy(req.trigger, "sched", 11);
            xQueueSend(g_seq_queue, &req, 0);
            break; // 1分に1スロットのみ
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  Web API ハンドラ
// ═══════════════════════════════════════════════════════════════════════════════

// シーケンスをキューに投入 (Web ハンドラ専用; 実行中は 409)
static void enqueue_from_web(uint8_t type, const char* trig) {
    if (g_running) {
        server.send(409, "application/json", "{\"error\":\"busy\"}");
        return;
    }
    SeqReq req;
    req.type = type;
    strncpy(req.trigger, trig, 11);
    xQueueSend(g_seq_queue, &req, 0);
    server.send(200, "application/json", "{\"ok\":true}");
}

void handle_root() {
    server.send_P(200, "text/html; charset=UTF-8", HTML_PAGE);
}

void handle_status() {
    String j = "{";
    j += "\"status\":\"";   j += g_status;    j += "\",";
    j += "\"step\":";       j += g_step;       j += ",";
    j += "\"progress\":";   j += g_progress;   j += ",";
    j += "\"running\":";    j += g_running  ? "true":"false"; j += ",";
    j += "\"watchdog_enabled\":true,";
    j += "\"sensors\":{";
    j += "\"ah\":"; j += water_detected(PIN_AH)?"true":"false"; j += ",";
    j += "\"al\":"; j += water_detected(PIN_AL)?"true":"false"; j += ",";
    j += "\"bh\":"; j += water_detected(PIN_BH)?"true":"false"; j += ",";
    j += "\"bl\":"; j += water_detected(PIN_BL)?"true":"false"; j += "},";
    j += "\"history\":[";
    xSemaphoreTake(g_hist_mtx, portMAX_DELAY);
    for (int i = 0; i < g_hist_count; i++) {
        int idx = (g_hist_head - 1 - i + HISTORY_MAX) % HISTORY_MAX;
        if (i) j += ",";
        j += "{\"ts\":" ;        j += (long)g_history[idx].ts;
        j += ",\"result\":\"";   j += g_history[idx].result;  j += "\"";
        j += ",\"duration\":";   j += g_history[idx].duration_sec;
        j += ",\"trigger\":\"";  j += g_history[idx].trigger; j += "\"}";
    }
    xSemaphoreGive(g_hist_mtx);
    j += "]}";
    server.send(200, "application/json", j);
}

void handle_start()  { enqueue_from_web(0, "web"); }
void handle_fill()   { enqueue_from_web(1, "web"); }
void handle_drain()  { enqueue_from_web(2, "web"); }

void handle_stop() {
    Serial.println("[STOP] Web UI");
    g_stop_flag = true;
    set_v1(false);
    set_v2(false);
    pump_off();
    server.send(200, "application/json", "{\"ok\":true}");
}

void handle_reboot() {
    Serial.println("[REBOOT] Web UI");
    all_off();
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.flush();
    delay(300);
    esp_restart();
}

void handle_get_timers() {
    String j = "{\"drain_max\":"    + String(T_DRAIN_MAX)
             + ",\"transfer_max\":" + String(T_TRANSFER_MAX)
             + ",\"fill_max\":"     + String(T_FILL_MAX)
             + ",\"fill_extra\":"   + String(T_FILL_EXTRA)
             + ",\"sensor_db\":"    + String(T_SENSOR_DB) + "}";
    server.send(200, "application/json", j);
}

void handle_post_timers() {
    String body = server.arg("plain");
    auto getInt = [&](const char* key) -> int {
        String k = String("\"") + key + "\":";
        int p = body.indexOf(k);
        return (p >= 0) ? body.substring(p + k.length()).toInt() : -1;
    };
    int d   = getInt("drain_max"), tr = getInt("transfer_max");
    int f   = getInt("fill_max"),  fe = getInt("fill_extra");
    int sdb = getInt("sensor_db");
    Preferences prefs;
    prefs.begin("timers", false);
    if (d   > 0)  { T_DRAIN_MAX    = d;   prefs.putUInt("drain",      d);   }
    if (tr  > 0)  { T_TRANSFER_MAX = tr;  prefs.putUInt("transfer",   tr);  }
    if (f   > 0)  { T_FILL_MAX     = f;   prefs.putUInt("fill",       f);   }
    if (fe  > 0)  { T_FILL_EXTRA   = fe;  prefs.putUInt("fill_extra", fe);  }
    if (sdb >= 0) { T_SENSOR_DB    = sdb; prefs.putUInt("sensor_db",  sdb); }
    prefs.end();
    server.send(200, "application/json", "{\"ok\":true}");
}

void handle_get_sched() {
    String j = "[";
    for (int i = 0; i < SCHED_SLOTS; i++) {
        if (i) j += ",";
        j += "{\"enabled\":"  + String(g_sched[i].enabled  ? "true":"false");
        j += ",\"dow\":"      + String(g_sched[i].dow_mask);
        j += ",\"hour\":"     + String(g_sched[i].hour);
        j += ",\"minute\":"   + String(g_sched[i].minute) + "}";
    }
    j += "]";
    server.send(200, "application/json", j);
}

static void sched_save_prefs() {
    Preferences prefs;
    prefs.begin("sched", false);
    char key[8];
    for (int i = 0; i < SCHED_SLOTS; i++) {
        snprintf(key, sizeof(key), "en%d",  i); prefs.putBool (key, g_sched[i].enabled);
        snprintf(key, sizeof(key), "dow%d", i); prefs.putUChar(key, g_sched[i].dow_mask);
        snprintf(key, sizeof(key), "h%d",   i); prefs.putUChar(key, g_sched[i].hour);
        snprintf(key, sizeof(key), "m%d",   i); prefs.putUChar(key, g_sched[i].minute);
    }
    prefs.end();
}

void handle_post_sched() {
    String body = server.arg("plain");
    int slot = 0;
    int pos  = 0;
    while (slot < SCHED_SLOTS && pos < (int)body.length()) {
        int ob = body.indexOf('{', pos);
        int cb = body.indexOf('}', ob);
        if (ob < 0 || cb < 0) break;
        String obj = body.substring(ob, cb + 1);
        auto getVal = [&](const char* key) -> int {
            String k = String("\"") + key + "\":";
            int p = obj.indexOf(k);
            if (p < 0) return -1;
            String v = obj.substring(p + k.length());
            if (v.startsWith("true"))  return 1;
            if (v.startsWith("false")) return 0;
            return v.toInt();
        };
        int en = getVal("enabled"), dow = getVal("dow");
        int h  = getVal("hour"),    m   = getVal("minute");
        if (en  >= 0) g_sched[slot].enabled  = (en == 1);
        if (dow >= 0) g_sched[slot].dow_mask = (uint8_t)dow;
        if (h   >= 0) g_sched[slot].hour     = (uint8_t)h;
        if (m   >= 0) g_sched[slot].minute   = (uint8_t)m;
        pos = cb + 1;
        slot++;
    }
    sched_save_prefs();
    server.send(200, "application/json", "{\"ok\":true}");
}

void handle_get_config() {
    String j = "{\"switchbot_token\":\""     + switchbot.tokenMasked()
             + "\",\"switchbot_secret\":\"****"
             + "\",\"switchbot_device_id\":\"" + switchbot.deviceId() + "\"}";
    server.send(200, "application/json", j);
}

void handle_post_config() {
    String body = server.arg("plain");
    auto getStr = [&](const char* key) -> String {
        String k = String("\"") + key + "\":\"";
        int p = body.indexOf(k);
        if (p < 0) return "";
        int s = p + k.length();
        int e = body.indexOf('"', s);
        return (e > s) ? body.substring(s, e) : "";
    };
    String tok = getStr("switchbot_token");
    String sec = getStr("switchbot_secret");
    String dev = getStr("switchbot_device_id");
    Preferences prefs;
    prefs.begin("switchbot", false);
    if (tok.length() > 0) prefs.putString("token",    tok);
    if (sec.length() > 0) prefs.putString("secret",   sec);
    if (dev.length() > 0) prefs.putString("deviceId", dev);
    prefs.end();
    switchbot.begin(); // 認証情報を再ロード
    server.send(200, "application/json", "{\"ok\":true}");
}

void handle_post_wifi() {
    String body = server.arg("plain");
    auto getStr = [&](const char* key) -> String {
        String k = String("\"") + key + "\":\"";
        int p = body.indexOf(k);
        if (p < 0) return "";
        int s = p + k.length();
        int e = body.indexOf('"', s);
        return (e > s) ? body.substring(s, e) : "";
    };
    String ssid = getStr("ssid");
    String pass = getStr("pass");
    if (ssid.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"ssid required\"}");
        return;
    }
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.println("[REBOOT] WiFi saved");
    Serial.flush();
    delay(400);
    esp_restart();
}

// /api/m/<dev>/<val>  POST  — 手動制御 (onNotFound から委譲)
static void handle_manual(const String& path) {
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"error\":\"POST required\"}");
        return;
    }
    // path は "/api/m/v1/1" 形式
    String rest = path.substring(7); // "v1/1"
    int sl = rest.indexOf('/');
    if (sl < 1) { server.send(400, "application/json", "{\"error\":\"bad path\"}"); return; }
    String dev = rest.substring(0, sl);
    bool   on  = rest.substring(sl + 1) != "0";

    if (dev == "v1") {
        set_v1(on);
        server.send(200, "application/json", "{\"ok\":true}");
    } else if (dev == "v2") {
        set_v2(on);
        server.send(200, "application/json", "{\"ok\":true}");
    } else if (dev == "pump") {
        bool ok = on ? pump_on() : pump_off();
        server.send(ok ? 200 : 500, "application/json",
                    ok ? "{\"ok\":true}" : "{\"error\":\"switchbot failed\"}");
    } else {
        server.send(404, "application/json", "{\"error\":\"unknown device\"}");
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  ISR ハンドラ
// ═══════════════════════════════════════════════════════════════════════════════

static void IRAM_ATTR isr_generic(int idx) {
    uint32_t now = millis();
    if (now - g_btn_last[idx] > DEBOUNCE_MS) {
        g_btn_last[idx] = now;
        g_btn_flag[idx] = true;
    }
}

void IRAM_ATTR isr_btn1() { isr_generic(0); }
void IRAM_ATTR isr_btn2() { isr_generic(1); }
void IRAM_ATTR isr_btn3() { isr_generic(2); }
void IRAM_ATTR isr_btn4() {                 // 緊急停止: ISR はフラグのみ
    uint32_t now = millis();                // g_stop_flag は process_buttons() でピン再読後にセット
    if (now - g_btn_last[3] > DEBOUNCE_MS) {
        g_btn_last[3] = now;
        g_btn_flag[3] = true;              // ← g_stop_flag はここでセットしない
    }
}
void IRAM_ATTR isr_btn5() {                 // 再起動: ISR はフラグのみ (BTN4 と同方式)
    uint32_t now = millis();
    if (now - g_btn_last[4] > DEBOUNCE_MS) {
        g_btn_last[4] = now;
        g_btn_flag[4] = true;
    }
}

// loop() から呼ぶボタン処理
static void process_buttons() {
    if (g_btn_flag[0]) {                    // ①自動水替え: ピン再読でノイズか本物か判定
        g_btn_flag[0] = false;
        vTaskDelay(pdMS_TO_TICKS(20));
        if (digitalRead(PIN_BTN1) == LOW) {
            if (!g_running && uxQueueMessagesWaiting(g_seq_queue) == 0) {
                Serial.println("[BTN] BTN1 auto");
                SeqReq req; req.type = 0; strncpy(req.trigger, "auto", 11);
                xQueueSend(g_seq_queue, &req, 0);
            }
        } else { Serial.println("[BTN] BTN1 noise (ignored)"); }
    }
    if (g_btn_flag[1]) {                    // ②強制給水: ピン再読でノイズか本物か判定
        g_btn_flag[1] = false;
        vTaskDelay(pdMS_TO_TICKS(20));
        if (digitalRead(PIN_BTN2) == LOW) {
            if (!g_running && uxQueueMessagesWaiting(g_seq_queue) == 0) {
                Serial.println("[BTN] BTN2 fill");
                SeqReq req; req.type = 1; strncpy(req.trigger, "fill", 11);
                xQueueSend(g_seq_queue, &req, 0);
            }
        } else { Serial.println("[BTN] BTN2 noise (ignored)"); }
    }
    if (g_btn_flag[2]) {                    // ③強制排水: ピン再読でノイズか本物か判定
        g_btn_flag[2] = false;
        vTaskDelay(pdMS_TO_TICKS(20));
        if (digitalRead(PIN_BTN3) == LOW) {
            if (!g_running && uxQueueMessagesWaiting(g_seq_queue) == 0) {
                Serial.println("[BTN] BTN3 drain");
                SeqReq req; req.type = 2; strncpy(req.trigger, "drain", 11);
                xQueueSend(g_seq_queue, &req, 0);
            }
        } else { Serial.println("[BTN] BTN3 noise (ignored)"); }
    }
    if (g_btn_flag[3]) {                    // ④緊急停止: ピン再読でノイズか本物か判定
        g_btn_flag[3] = false;
        vTaskDelay(pdMS_TO_TICKS(20));      // ノイズが収まるまで待つ
        if (digitalRead(PIN_BTN4) == LOW) { // まだ LOW = 本物の押下
            Serial.println("[STOP] BTN4");
            g_stop_flag = true;
            set_v1(false); set_v2(false); pump_off();
        } else {
            Serial.println("[STOP] BTN4 noise (ignored)");
        }
    }
    if (g_btn_flag[4]) {                    // ⑤再起動: ピン再読でノイズか本物か判定
        g_btn_flag[4] = false;
        vTaskDelay(pdMS_TO_TICKS(20));
        if (digitalRead(PIN_BTN5) == LOW) { // まだ LOW = 本物の押下
            Serial.println("[REBOOT] BTN5");
            all_off();
            Serial.flush();
            delay(200);
            esp_restart();
        } else {
            Serial.println("[REBOOT] BTN5 noise (ignored)");
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  WiFi
// ═══════════════════════════════════════════════════════════════════════════════

static void wifi_init() {
    Preferences prefs;
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    if (ssid.length() > 0) {
        Serial.printf("[WiFi] Connecting to '%s'...\n", ssid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
            delay(400); Serial.print('.');
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
            return;
        }
        Serial.println("\n[WiFi] Failed → AP mode");
    } else {
        Serial.println("[WiFi] No credentials → AP mode");
    }

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : nullptr);
    Serial.printf("[WiFi] AP SSID: %s  IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());
}

static void wifi_reconnect_check() {
    static uint32_t last = 0;
    if (millis() - last < 30000) return;
    last = millis();
    if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Reconnecting…");
        WiFi.reconnect();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  Preferences ロード
// ═══════════════════════════════════════════════════════════════════════════════

static void prefs_load() {
    Preferences prefs;

    prefs.begin("timers", true);
    T_DRAIN_MAX    = prefs.getUInt("drain",      DEFAULT_T_DRAIN_MAX);
    T_TRANSFER_MAX = prefs.getUInt("transfer",   DEFAULT_T_TRANSFER_MAX);
    T_FILL_MAX     = prefs.getUInt("fill",       DEFAULT_T_FILL_MAX);
    T_FILL_EXTRA   = prefs.getUInt("fill_extra", DEFAULT_T_FILL_EXTRA);
    T_SENSOR_DB    = prefs.getUInt("sensor_db",  DEFAULT_T_SENSOR_DB);
    prefs.end();

    prefs.begin("sched", true);
    char key[8];
    for (int i = 0; i < SCHED_SLOTS; i++) {
        snprintf(key, sizeof(key), "en%d",  i); g_sched[i].enabled  = prefs.getBool (key, false);
        snprintf(key, sizeof(key), "dow%d", i); g_sched[i].dow_mask = prefs.getUChar(key, 0);
        snprintf(key, sizeof(key), "h%d",   i); g_sched[i].hour     = prefs.getUChar(key, 8);
        snprintf(key, sizeof(key), "m%d",   i); g_sched[i].minute   = prefs.getUChar(key, 0);
    }
    prefs.end();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  setup / loop
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);

    // ── 安全最優先: 全リレーを OFF 状態に設定 ─────────────────────────────────
    pinMode(PIN_V1, OUTPUT); digitalWrite(PIN_V1, RELAY_OFF);
    pinMode(PIN_V2, OUTPUT); digitalWrite(PIN_V2, RELAY_OFF);

    // ── センサー入力 ───────────────────────────────────────────────────────────
    pinMode(PIN_AH, INPUT_PULLUP);
    pinMode(PIN_AL, INPUT_PULLUP);
    pinMode(PIN_BH, INPUT_PULLUP);
    pinMode(PIN_BL, INPUT_PULLUP);

    // ── ボタン入力 + ISR ───────────────────────────────────────────────────────
    int btn_pins[] = {PIN_BTN1, PIN_BTN2, PIN_BTN3, PIN_BTN4, PIN_BTN5};
    for (int p : btn_pins) pinMode(p, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN1), isr_btn1, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN2), isr_btn2, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN3), isr_btn3, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN4), isr_btn4, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN5), isr_btn5, FALLING);

    // ── Audio ──────────────────────────────────────────────────────────────────
    Audio::init();
    Audio::beep_start();

    // ── Preferences ───────────────────────────────────────────────────────────
    prefs_load();
    sensors_init(); // Preferences ロード後に初期化（T_SENSOR_DB 参照のため後置）

    // ── WiFi / NTP / mDNS ─────────────────────────────────────────────────────
    wifi_init();
    configTzTime(NTP_TZ, NTP_SERVER);
    if (MDNS.begin(MDNS_NAME))
        Serial.printf("[mDNS] http://%s.local\n", MDNS_NAME);

    // ── SwitchBot ──────────────────────────────────────────────────────────────
    switchbot.begin();

    // ── FreeRTOS プリミティブ ──────────────────────────────────────────────────
    g_hist_mtx = xSemaphoreCreateMutex();
    g_seq_queue = xQueueCreate(4, sizeof(SeqReq));

    xTaskCreatePinnedToCore(seq_task,           "seq",    8192, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(watchdog_sched_task,"wdSched",4096, nullptr, 1, nullptr, 0);

    // ── Web サーバールート登録 ─────────────────────────────────────────────────
    server.on("/",             HTTP_GET,  handle_root);
    server.on("/api/status",   HTTP_GET,  handle_status);
    server.on("/api/start",    HTTP_POST, handle_start);
    server.on("/api/fill",     HTTP_POST, handle_fill);
    server.on("/api/drain",    HTTP_POST, handle_drain);
    server.on("/api/stop",     HTTP_POST, handle_stop);
    server.on("/api/reboot",   HTTP_POST, handle_reboot);
    server.on("/api/timers",   HTTP_GET,  handle_get_timers);
    server.on("/api/timers",   HTTP_POST, handle_post_timers);
    server.on("/api/sched",    HTTP_GET,  handle_get_sched);
    server.on("/api/sched",    HTTP_POST, handle_post_sched);
    server.on("/api/config",   HTTP_GET,  handle_get_config);
    server.on("/api/config",   HTTP_POST, handle_post_config);
    server.on("/api/wifi",     HTTP_POST, handle_post_wifi);
    server.onNotFound([](){
        String uri = server.uri();
        if (uri.startsWith("/api/m/")) { handle_manual(uri); return; }
        server.send(404, "text/plain", "Not found");
    });

    server.begin();
    Serial.println("[HTTP] Server started on port 80");
}

void loop() {
    sensors_update();
    server.handleClient();
    process_buttons();
    wifi_reconnect_check();
    delay(2);
}

#pragma once
#include <Arduino.h>

// SwitchBot API v1.1 (HTTPS + HMAC-SHA256)
// 認証情報は Preferences("switchbot") から読み込む。
// pumpOn()/pumpOff() はそれぞれ最大3回リトライし、失敗時は false を返す。
class SwitchBot {
public:
    void begin();                    // Preferences から認証情報をロード
    bool pumpOn();
    bool pumpOff();

    // 設定値のゲッター（WebUI マスク表示用）
    String tokenMasked()  const;
    String deviceId()     const { return _deviceId; }

private:
    String _token;
    String _secret;
    String _deviceId;

    String buildSign(const String& t, const String& nonce) const;
    bool   sendCommand(const char* cmd);
};

extern SwitchBot switchbot;

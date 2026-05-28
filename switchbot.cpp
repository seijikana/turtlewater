#include "switchbot.h"
#include "config.h"
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <time.h>

SwitchBot switchbot;

// ─────────────────────────────────────────────────────────────────────────────

void SwitchBot::begin() {
    Preferences prefs;
    prefs.begin("switchbot", true);
    _token    = prefs.getString("token",    "");
    _secret   = prefs.getString("secret",   "");
    _deviceId = prefs.getString("deviceId", "");
    prefs.end();
}

String SwitchBot::tokenMasked() const {
    if (_token.length() < 4) return "****";
    return _token.substring(0, 4) + "****";
}

// sign = base64(HMAC-SHA256( token+t+nonce, secret )).toUpperCase()
String SwitchBot::buildSign(const String& t, const String& nonce) const {
    String data = _token + t + nonce;

    uint8_t hmac[32];
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 1 /* hmac */);
    mbedtls_md_hmac_starts(&ctx,
        reinterpret_cast<const uint8_t*>(_secret.c_str()), _secret.length());
    mbedtls_md_hmac_update(&ctx,
        reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);

    uint8_t b64[64] = {};
    size_t  olen    = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, hmac, sizeof(hmac));

    String sign = String(reinterpret_cast<char*>(b64)).substring(0, (int)olen);
    sign.toUpperCase();
    return sign;
}

bool SwitchBot::sendCommand(const char* cmd) {
    if (_token.isEmpty() || _deviceId.isEmpty()) {
        Serial.println("[SwitchBot] credentials not set");
        return false;
    }

    // Unix epoch ms timestamp
    String t     = String((long long)time(nullptr) * 1000LL);
    String nonce = String(esp_random() % 900000 + 100000);
    String sign  = buildSign(t, nonce);

    String url  = "https://" + String(SWITCHBOT_HOST)
                + "/v1.1/devices/" + _deviceId + "/commands";
    String body = "{\"command\":\"" + String(cmd)
                + "\",\"parameter\":\"default\",\"commandType\":\"command\"}";

    for (int attempt = 1; attempt <= 3; attempt++) {
        WiFiClientSecure client;
        client.setInsecure(); // embedded: skip cert chain verification

        HTTPClient http;
        if (!http.begin(client, url)) {
            Serial.printf("[SwitchBot] http.begin failed (attempt %d)\n", attempt);
            delay(1000);
            continue;
        }

        http.addHeader("Content-Type",  "application/json; charset=utf8");
        http.addHeader("Authorization", _token);
        http.addHeader("sign",          sign);
        http.addHeader("t",             t);
        http.addHeader("nonce",         nonce);

        int code = http.POST(body);
        String resp = http.getString();
        http.end();

        Serial.printf("[SwitchBot] %s attempt %d → %d\n", cmd, attempt, code);

        if (code == 200) return true;
        delay(1000);
    }
    return false;
}

bool SwitchBot::pumpOn()  { return sendCommand("turnOn");  }
bool SwitchBot::pumpOff() { return sendCommand("turnOff"); }

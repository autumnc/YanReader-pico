#pragma once

#include <string>
#include <esp_err.h>

class WifiManager {
public:
    bool begin();
    bool connect(const char *ssid, const char *password);
    bool isConnected();
    void disconnect();
    std::string getIp();
    // 最近一次连接失败的人话原因（断开 reason 码的翻译），成功时为 "OK"。
    const char *lastReasonText();

private:
    bool _inited = false;
    bool _started = false;  // wifi radio started (station active)
};

extern WifiManager g_wifi;

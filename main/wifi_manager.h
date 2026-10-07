#pragma once

#include <string>
#include <esp_err.h>

class WifiManager {
public:
    // 连接进度的可选回调：connect() 是阻塞的（最长 10 秒），这期间除了这里没有任何
    // 机会重画屏幕；没有它，用户看到的就是一个冻住的界面（"是不是死机了"）。
    // ctx 原样透传，elapsed 是已等待的毫秒数。每 ~250ms 一次（重画本身要几百毫秒，
    // 实际间隔更长）。默认 nullptr = 老行为。
    // / Optional progress callback for the blocking connect(), invoked every ~250 ms
    // with the elapsed wait in milliseconds so the caller can repaint.
    typedef void (*ConnectProgress)(void *ctx, unsigned elapsedMs);

    bool begin();
    bool connect(const char *ssid, const char *password, ConnectProgress progress = nullptr, void *ctx = nullptr);
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

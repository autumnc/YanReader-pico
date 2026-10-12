#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <esp_err.h>

// 一条扫描结果（去重后按信号强 → 弱）。同名 SSID 由多个 AP/频段广播时只留最强的那条。
struct WifiScanItem {
    std::string ssid;
    int rssi = 0;
    uint8_t authmode = 0;   // wifi_auth_mode_t（WIFI_AUTH_OPEN = 开放网络）
    bool saved = false;     // 是不是在已保存名单里（由 WifiManager 查 g_wifiStore 填）
};

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
    // 当前（最近一次）配置/连接的网络名。没连过/已断开时为空串。
    const char *connectedSsid();

    // ── 射频总开关（界面上那一行药丸）──────────────────────────────────
    // radioUp = 射频此刻真的起着（station started）。enabled = 持久化的开关意图，
    // 默认开；它只是"用户有没有手动把射频收起来"，按需联网的路径不读它。
    bool radioUp();
    bool enabled();
    // 持久化开关并立即起/停射频。**不**自动连接：那个 10 秒阻塞放在"拨一下开关"上是
    // 最差的第一次反馈，界面改成随后起一次扫描，让用户自己点。
    void setEnabled(bool on);

    // ── 异步扫描 ──────────────────────────────────────────────────────
    // scanStart 只发起（非阻塞），结果在 WIFI_EVENT_SCAN_DONE 之后由 scanPoll 取回
    // （取回放 UI 线程，事件处理器里不分配内存）。scanPoll 返回 true = 这一次真的
    // 落地了新结果，界面该重绘。radio 没起时 scanStart 会先把它起起来。
    // scanStop 取消并清空（离开界面 / 关射频时调）。
    bool scanStart();
    bool scanBusy();
    bool scanPoll();
    void scanStop();
    int scanCount();
    const WifiScanItem &scanResult(int i);

    // ── 已保存网络（委托 g_wifiStore；成功后镜像旧键，见 .cpp）────────────
    // savedPassword: 找到返回 true，密码写进 outPass，*outOpen = 开放网络（无密码）。
    bool savedPassword(const char *ssid, std::string &outPass, bool *outOpen);
    bool savedHas(const char *ssid);
    bool savedRemember(const char *ssid, const char *pass);
    bool savedForget(const char *ssid);

private:
    bool _inited = false;
    bool _started = false;  // wifi radio started (station active)
};

extern WifiManager g_wifi;

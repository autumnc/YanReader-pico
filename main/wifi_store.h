#pragma once
//
// 已保存的 WiFi 网络（数据层）。安卓式 WiFi 管理要能存**多个**网络、每个都能连/忘记，
// 而设置里原来只有一对 wifi_ssid/wifi_pass（单网络）。
//
// 存储沿用阅读器那两张表的惯例（reader_progress.txt / reader_notes.txt）：SD 上一个
// 纯文本文件、一行一条记录、字段用 \x1f 分隔、整文件走 safeWriteFile 原子写。
// 之所以不复用 g_settings 的键值对：这里是"表"，条数会变、还要按顺序（最近用过的在前），
// 而 settings 的 key=一个文件 的模型适合单值；把整张表塞进一个键还要额外定义转义，
// 等于把同一个格式再造一遍。
//
// **纯逻辑与文件 IO 分开**：serialize/parse/escapeField/unescapeField 是不碰文件系统的
// 静态函数，主机测试直接对拍（见 tests/host/wifi_store/）。
//
// / Saved WiFi networks, stored as one record per line in /sdcard/wifi_networks.txt.
//

#include <cstdint>
#include <string>
#include <vector>

struct WifiSavedNet {
    std::string ssid;
    std::string pass;       // 开放网络为空串
    bool open = false;      // 无密码（连接时 authmode = WIFI_AUTH_OPEN）
    int64_t lastUsed = 0;   // 最近一次成功连接的 epoch 秒；0 = 从没连过 / 机器时间没校准
};

class WifiStore {
public:
    // 上限。超了丢末尾（= 最久没用过的）。32 条 × 最长约 120 字节 ≈ 4KB，整文件一次写完。
    static const int kMax = 32;

    // 读盘（懒加载，幂等）。文件不存在/损坏 → 空表。首次若表为空而旧键 wifi_ssid 非空，
    // 把那一对收进内存里的表（**不写盘**：迁移本身不该动 SD 卡，等第一次 remember 再落盘）。
    // const 是为了让只读的访问器（has/at/find/count）也能按需触发加载——成员因此是 mutable。
    void load() const;

    // 原子落盘。失败返回 false（调用方只在日志里记一笔，内存里的表照旧可用）。
    bool save() const;

    // 注意也调 load()：表是懒加载的，不先读盘的话"还没人碰过表"时这里恒返回 0，
    // 界面会把已保存的网当成没有。
    int count() const {
        load();
        return static_cast<int>(nets_.size());
    }
    const WifiSavedNet *at(int i) const;                 // 顺序 = 最近用过的在前
    bool has(const char *ssid) const;
    const WifiSavedNet *find(const char *ssid) const;

    // 记住/更新一个网络：有则更新密码与时间戳并挪到最前，无则插到最前，然后落盘。
    // ssid 超 32 字节、密码超 63 字节会被截断（wifi_config_t 本来也会截）。
    bool remember(const char *ssid, const char *pass);

    // 只刷新时间戳并挪到最前（连接成功且无需改密码时用）。不落盘——时间戳丢掉无所谓。
    void touch(const char *ssid);

    // 忘记一个网络。返回是否真的删掉了（表里本来就没有 → false）。落盘。
    bool forget(const char *ssid);

    // 最近用过的那个网络的名字/密码（空 = 没有）。用于连接失败后重指旧键。
    std::string mostRecentSsid() const;
    std::string mostRecentPass() const;

    // ── 纯函数：主机测试直接对拍 ────────────────────────────────────────
    static std::string serialize(const std::vector<WifiSavedNet> &v);
    static std::vector<WifiSavedNet> parse(const std::string &content);
    static std::string escapeField(const std::string &s);
    static std::string unescapeField(const std::string &s);

private:
    mutable std::vector<WifiSavedNet> nets_;   // 最近用过的在前
    mutable bool loaded_ = false;
};

extern WifiStore g_wifiStore;

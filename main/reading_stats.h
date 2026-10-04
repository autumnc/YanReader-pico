#pragma once
//
// 阅读统计：数据层。
//
// 从 crossmux 的 ReadingStatsStore + ReadingStatsAnalytics 移植，砍掉了这里用不上的
// 依赖（BookIdentity/MD5 稳定书号、封面缓存路径、账目导出导入、成就系统）。书用 path
// 当唯一键——这个阅读器本来就是按 path 找书的（reader_progress.txt 也一样）。
//
// 记录口径与 crossmux 逐条对齐，常量一个不改（见 .cpp 顶部）。本地日界直接用
// localtime_r()：main.cpp 开机就 setenv("TZ", 设置里的时区) + tzset()，所以拿到的
// 就是真实本地墙钟，不需要 crossmux 那套 15 分钟刻度的固定偏移。
//
// Reading statistics data layer, ported from crossmux's ReadingStatsStore. Books are
// keyed by path; the local day boundary comes from localtime_r() because main.cpp sets
// TZ from settings at boot.
//

#include <cstdint>
#include <string>
#include <vector>

struct ReadingDayStats {
    uint32_t dayOrdinal = 0;
    uint64_t readingMs = 0;
};

struct ReadingBookStats {
    std::string path;
    std::string title;
    std::string author;
    std::string chapterTitle;
    std::vector<ReadingDayStats> readingDays;
    uint64_t totalReadingMs = 0;
    uint32_t sessions = 0;
    uint32_t lastSessionMs = 0;
    uint32_t firstReadAt = 0;
    uint32_t lastReadAt = 0;
    uint32_t completedAt = 0;
    uint8_t lastProgressPercent = 0;
    uint8_t chapterProgressPercent = 0;
    bool completed = false;
};

struct ReadingSessionSnapshot {
    bool valid = false;
    uint32_t serial = 0;
    std::string path;
    uint32_t sessionMs = 0;
    bool counted = false;
    bool completedThisSession = false;
    uint8_t startProgressPercent = 0;
    uint8_t endProgressPercent = 0;
};

struct ReadingSessionLogEntry {
    uint32_t dayOrdinal = 0;
    uint32_t sessionMs = 0;
};

// 本地时间工具。时钟不可信时（早于 2024-01-01）epoch 返回 0、日序号返回 0，
// 调用方按"没有可信时间"处理。总时长照记，日桶/热力图/连续天数会空着。
// Local-time helpers. An untrustworthy clock yields 0 epoch / 0 day ordinal.
namespace RdTime {
bool clockValid();                        // 现在的时钟可信吗
bool clockValid(uint32_t epochSeconds);   // 某个时间戳可信吗
uint32_t nowEpoch();                      // 可信则 now，否则 0
uint32_t dayOrdinal(uint32_t epochSeconds);   // 本地日序号（1970-01-01 起的天数），失效为 0
uint32_t todayOrdinal();                      // 今天的本地日序号
uint32_t ordinalForDate(int year, unsigned month, unsigned day);
bool dateFromOrdinal(uint32_t dayOrdinal, int &year, unsigned &month, unsigned &day);
unsigned daysInMonth(int year, unsigned month);
void formatOrdinal(uint32_t dayOrdinal, char *buf, size_t bufSize);  // "2026-10-03"
void formatMonth(int year, unsigned month, char *buf, size_t bufSize);  // "2026-10"
}  // namespace RdTime

namespace ReadingStats {

// 生命周期
void load();               // 开机读一次
bool save();               // 强制落盘（改了设置、离开阅读器、退出时）
bool shouldCheckpoint();   // 有脏数据 + 会话进行中 + 距上次落盘 ≥10 分钟

// 会话
void beginSession(const std::string &path, const std::string &title, const std::string &author, uint8_t progressPercent,
                  const std::string &chapterTitle, uint8_t chapterProgressPercent);
void noteActivity();          // 一次交互：把距上次交互的时间（截到 30 分钟）记进当天
void tickActiveSession();     // 空闲帧心跳：距上次交互 ≥60s 才补一笔
void resumeSession();         // 跳出子界面回来：丢弃空档，不补记
void updateProgress(uint8_t progressPercent, bool completed, const std::string &chapterTitle,
                    uint8_t chapterProgressPercent);
void endSession();
bool hasActiveSession();
const std::string &activePath();

// 查询
const std::vector<ReadingBookStats> &books();
const std::vector<ReadingDayStats> &readingDays();
const std::vector<ReadingSessionLogEntry> &sessionLog();
const ReadingSessionSnapshot &lastSessionSnapshot();
const ReadingBookStats *findBook(const std::string &path);

uint32_t booksStarted();
uint32_t booksFinished();
uint64_t totalReadingMs();
uint64_t todayReadingMs();
uint64_t recentReadingMs(uint32_t days);
uint32_t currentStreakDays();
uint32_t maxStreakDays();
uint32_t readDaysCount();     // 有阅读记录的天数
uint64_t goalMs();            // 每日目标（来自设置 daily_goal，分钟）

// 改动
bool adjustBookReadingTime(const std::string &path, uint32_t dayOrdinal, int32_t deltaMs);
bool removeBook(const std::string &path);

// 展示用格式化："45m" / "2h 5m"
std::string formatDurationHm(uint64_t totalMs);

}  // namespace ReadingStats

#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <unordered_set>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct JournalEntry {
    std::string filename;   // YYYY-MM-DD_HHMMSS.txt
    std::string date;       // YYYY-MM-DD
    std::string title;      // prompt or "自由写作"
    std::string preview;    // first line of body
    std::string full_text;  // full file content
};

struct JournalHistoryVersion {
    std::string filename;   // history snapshot filename
    time_t mtime = 0;
    size_t size = 0;
};

class JournalStorage {
public:
    bool begin();
    void deinit();

    // Save a new entry
    bool saveEntry(const std::string &text);

    // Save entry with explicit filename (for sync downloads)
    bool saveEntryRaw(const std::string &filename, const std::string &content, bool createHistory = true);

    // Write a recovery draft outside the normal journal index.
    bool saveRecoveryDraft(const std::string &content, const std::string &meta);
    bool loadRecoveryDraft(std::string &content, std::string &meta);
    void clearRecoveryDraft();

    std::vector<JournalHistoryVersion> listHistoryVersions(const std::string &filename);
    std::string readHistoryVersion(const std::string &filename, const std::string &historyFilename);
    bool restoreHistoryVersion(const std::string &filename, const std::string &historyFilename);
    bool deleteHistoryVersion(const std::string &filename, const std::string &historyFilename);

    // List all entries, newest first
    std::vector<JournalEntry> listEntries();

    // List filenames with modification times (for sync)
    std::vector<std::pair<std::string, time_t>> listFileMtimes();

    // Read entry content by filename
    std::string readEntry(const std::string &filename);

    // Delete entry by filename
    bool deleteEntry(const std::string &filename);

    // Check if date has entry
    bool hasEntry(const std::string &date);

    // 首页那三个数一次取全（总篇数 / 今日篇数 / 连续天数）。三个都要遍历索引，而首页
    // **每帧**都要（屏幕就停在那儿不动），所以内部按 (索引版本, 今天) 备忘：索引没动、
    // 还是同一天，答案必然一样。跨零点由日期串自己抓到，索引改动由 m_revision 抓到。
    // 下面三个单一取数口都走它，口径自然一致。
    struct Stats {
        int total = 0;
        int todayCount = 0;
        int streak = 0;
    };
    Stats stats();

    // Count entries for today
    int countToday();

    // Calculate streak
    int getStreak();

    // Total entries
    int totalEntries();

    // SD card status
    bool isMounted() const { return mounted_; }

    // Get SD card mutex for thread-safe access from external callers
    static SemaphoreHandle_t sdMutex();

private:
    std::string basePath();
    void ensureDir();

    // In-memory index of journal filenames (newest first) + distinct dates.
    // Avoids repeated full-directory scans during boot and main screen draw.
    void scanIndex();                  // rebuild both index structures
    void ensureIndex();                // lazy rebuild if invalid
    void indexAddFile(const std::string &fn);
    void indexRemoveFile(const std::string &fn);
    std::vector<std::string> m_fileIndex;      // newest first
    std::unordered_set<std::string> m_dateSet; // distinct YYYY-MM-DD dates
    bool m_indexValid = false;

    // stats() 的备忘。m_revision 每次索引变动 ++（scanIndex/indexAddFile/indexRemoveFile），
    // 它就是"索引有没有动过"的唯一判据。m_* 只在持有 s_sd_mutex 时读写。
    int m_revision = 0;
    bool m_statsValid = false;
    int m_statsRev = -1;
    char m_statsDay[16] = {0};   // 备忘针对哪一天（跨零点即失效）
    int m_statsTotal = 0;
    int m_statsToday = 0;
    int m_statsStreak = 0;

    bool mounted_ = false;
};

extern JournalStorage g_journal;

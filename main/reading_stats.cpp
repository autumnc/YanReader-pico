//
// 阅读统计：数据层实现。行为逐条对齐 crossmux 的 ReadingStatsStore（常量不改），
// 但持久化换成 main/json_parser.h 的 JsonValue（原子写走 safeWriteFile），
// 时间基换成 esp_timer_get_time()（int64 单调，不会像 millis() 那样 49 天回绕）。
//
// Reading statistics implementation. Same recording rules as crossmux's
// ReadingStatsStore; persistence uses JsonValue and the clock is
// esp_timer_get_time() (monotonic int64 ms).
//

#include "reading_stats.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "esp_timer.h"
#include "json_parser.h"
#include "settings_manager.h"

namespace {

constexpr const char *kStatsPath = "/sdcard/reader_stats.json";

// —— 与 crossmux 完全一致的记录口径 ——
constexpr int64_t MAX_READING_GAP_MS = 30LL * 60LL * 1000LL;       // 两次交互之间最多补记 30 分钟
constexpr int64_t SESSION_HEARTBEAT_MS = 60LL * 1000LL;           // 空闲心跳节拍
constexpr int64_t CHECKPOINT_INTERVAL_MS = 10LL * 60LL * 1000LL;  // 有脏数据且会话中，每 10 分钟落一次盘
constexpr int64_t CHECKPOINT_RETRY_INTERVAL_MS = 30LL * 1000LL;   // 上次落盘失败后 30 秒再试
constexpr uint64_t MIN_SESSION_READING_MS = 3ULL * 60ULL * 1000ULL;  // 短于这个的会话不计"次数"（时长照记）
constexpr size_t MAX_SESSION_LOG_ENTRIES = 256;
constexpr size_t MAX_BOOKS = 200;  // 体积红线：见 reading_stats.h 的说明

// 2024-01-01（UTC+14 的最早那一刻）之前一律当成时钟没同步。
constexpr uint32_t VALID_CLOCK_THRESHOLD = 1704016800UL;

constexpr int kFormatVersion = 8;  // 与 crossmux v6 同名字段，去掉 legacyReadingDays/knownPaths

int64_t nowMs() { return esp_timer_get_time() / 1000; }

uint8_t clampPercent(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 100 ? 100 : v)); }

// ── 民历换算（Howard Hinnant 的 days_from_civil / civil_from_days）─────────
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int &y, unsigned &m, unsigned &d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int yy = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = mp < 10u ? mp + 3u : mp - 9u;
    y = yy + (m <= 2u ? 1 : 0);
}

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// ── 数据 ─────────────────────────────────────────────────────────────────
std::vector<ReadingBookStats> s_books;
std::vector<ReadingDayStats> s_readingDays;
std::vector<ReadingSessionLogEntry> s_sessionLog;
uint32_t s_lastKnownAt = 0;  // 最后一次见到的可信时间，作时钟失效时的兜底
uint32_t s_sessionSerial = 0;

struct SessionState {
    bool active = false;
    int64_t lastInteractionMs = 0;
    uint64_t accumulatedMs = 0;
    uint8_t startProgressPercent = 0;
    bool startCompleted = false;
};
SessionState s_session;
std::string s_activePath;
ReadingSessionSnapshot s_lastSnapshot;

bool s_dirty = false;
int64_t s_dirtySinceMs = 0;
int64_t s_lastSaveAttemptMs = 0;

struct SummaryCache {
    bool valid = false;
    uint32_t referenceDayOrdinal = 0;
    uint64_t goalReadingMs = 0;
    uint32_t booksFinishedCount = 0;
    uint64_t totalReadingMs = 0;
    uint64_t todayReadingMs = 0;
    uint64_t recent7ReadingMs = 0;
    uint64_t recent30ReadingMs = 0;
    uint32_t currentStreakDays = 0;
    uint32_t maxStreakDays = 0;
};
SummaryCache s_summary;

void invalidateSummary() { s_summary.valid = false; }

void markDirty() {
    if (!s_dirty) s_dirtySinceMs = nowMs();
    s_dirty = true;
    invalidateSummary();
}

// ── 日桶维护 ─────────────────────────────────────────────────────────────
void normalizeReadingDays(std::vector<ReadingDayStats> &days) {
    std::sort(days.begin(), days.end(),
              [](const ReadingDayStats &a, const ReadingDayStats &b) { return a.dayOrdinal < b.dayOrdinal; });
    std::vector<ReadingDayStats> merged;
    merged.reserve(days.size());
    for (const auto &day : days) {
        if (!merged.empty() && merged.back().dayOrdinal == day.dayOrdinal) {
            merged.back().readingMs += day.readingMs;
        } else {
            merged.push_back(day);
        }
    }
    days = std::move(merged);
}

void addReadingToDays(std::vector<ReadingDayStats> &days, uint32_t dayOrdinal, uint64_t readingMs) {
    if (dayOrdinal == 0 || readingMs == 0) return;
    auto it = std::lower_bound(days.begin(), days.end(), dayOrdinal,
                               [](const ReadingDayStats &day, uint32_t ordinal) { return day.dayOrdinal < ordinal; });
    if (it == days.end() || it->dayOrdinal != dayOrdinal) {
        days.insert(it, ReadingDayStats{dayOrdinal, readingMs});
    } else {
        it->readingMs += readingMs;
    }
}

ReadingDayStats &getOrCreateDay(std::vector<ReadingDayStats> &days, uint32_t dayOrdinal) {
    auto it = std::lower_bound(days.begin(), days.end(), dayOrdinal,
                               [](const ReadingDayStats &day, uint32_t ordinal) { return day.dayOrdinal < ordinal; });
    if (it == days.end() || it->dayOrdinal != dayOrdinal) {
        it = days.insert(it, ReadingDayStats{dayOrdinal, 0});
    }
    return *it;
}

// 聚合日表 = 所有书那天的时长之和（crossmux 是"每本书的日桶 + 老档期日桶"，这里没有老档期）。
void rebuildAggregatedDays() {
    s_readingDays.clear();
    for (const auto &book : s_books) {
        for (const auto &day : book.readingDays) {
            addReadingToDays(s_readingDays, day.dayOrdinal, day.readingMs);
        }
    }
}

// ── 书 ───────────────────────────────────────────────────────────────────
size_t findBookIndex(const std::string &path) {
    if (path.empty()) return s_books.size();
    for (size_t i = 0; i < s_books.size(); ++i) {
        if (s_books[i].path == path) return i;
    }
    return s_books.size();
}

// 最近读的排最前（界面直接按这个顺序列）。索引变了，会话指针要跟着挪。
void touchBook(size_t index) {
    if (index == 0 || index >= s_books.size()) return;
    ReadingBookStats book = s_books[index];
    s_books.erase(s_books.begin() + static_cast<std::ptrdiff_t>(index));
    s_books.insert(s_books.begin(), std::move(book));
}

size_t getOrCreateBookIndex(const std::string &path, const std::string &title, const std::string &author) {
    size_t index = findBookIndex(path);
    if (index == s_books.size()) {
        ReadingBookStats book;
        book.path = path;
        book.title = title;
        book.author = author;
        s_books.insert(s_books.begin(), std::move(book));
        return 0;
    }
    auto &book = s_books[index];
    if (!title.empty()) book.title = title;
    if (!author.empty()) book.author = author;
    return index;
}

constexpr size_t kMinReadingDayBookMs = 3u * 60u * 1000u;

}  // namespace

// ── 时间工具 ─────────────────────────────────────────────────────────────
namespace RdTime {

bool clockValid(uint32_t epochSeconds) { return epochSeconds >= VALID_CLOCK_THRESHOLD; }

bool clockValid() {
    const time_t now = time(nullptr);
    return now > 0 && static_cast<uint32_t>(now) >= VALID_CLOCK_THRESHOLD;
}

uint32_t nowEpoch() {
    const time_t now = time(nullptr);
    if (now <= 0) return 0;
    const uint32_t t = static_cast<uint32_t>(now);
    return clockValid(t) ? t : 0;
}

uint32_t dayOrdinal(uint32_t epochSeconds) {
    if (!clockValid(epochSeconds)) return 0;
    const time_t t = static_cast<time_t>(epochSeconds);
    struct tm lt;
    if (localtime_r(&t, &lt) == nullptr) return 0;
    const int64_t ordinal = daysFromCivil(lt.tm_year + 1900, static_cast<unsigned>(lt.tm_mon + 1),
                                          static_cast<unsigned>(lt.tm_mday));
    return ordinal > 0 ? static_cast<uint32_t>(ordinal) : 0;
}

uint32_t todayOrdinal() { return dayOrdinal(nowEpoch()); }

uint32_t ordinalForDate(int year, unsigned month, unsigned day) {
    const int64_t ordinal = daysFromCivil(year, month, day);
    return ordinal > 0 ? static_cast<uint32_t>(ordinal) : 0;
}

bool dateFromOrdinal(uint32_t dayOrdinal_, int &year, unsigned &month, unsigned &day) {
    if (dayOrdinal_ == 0) return false;
    civilFromDays(static_cast<int64_t>(dayOrdinal_), year, month, day);
    return true;
}

unsigned daysInMonth(int year, unsigned month) {
    static const unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2 && isLeap(year)) return 29;
    return kDays[month - 1];
}

void formatOrdinal(uint32_t dayOrdinal_, char *buf, size_t bufSize) {
    int y = 0;
    unsigned m = 0, d = 0;
    if (!dateFromOrdinal(dayOrdinal_, y, m, d)) {
        snprintf(buf, bufSize, "--");
        return;
    }
    snprintf(buf, bufSize, "%04d-%02u-%02u", y, m, d);
}

void formatMonth(int year, unsigned month, char *buf, size_t bufSize) {
    snprintf(buf, bufSize, "%04d-%02u", year, month);
}

}  // namespace RdTime

// ── 参考时间：时钟不可信时用"最后一次见到的时间"兜底 ─────────────────────
namespace {

uint32_t latestKnownFromBooks() {
    uint32_t latest = s_lastKnownAt;
    for (const auto &book : s_books) {
        if (RdTime::clockValid(book.lastReadAt)) latest = std::max(latest, book.lastReadAt);
        if (RdTime::clockValid(book.completedAt)) latest = std::max(latest, book.completedAt);
        if (RdTime::clockValid(book.firstReadAt)) latest = std::max(latest, book.firstReadAt);
    }
    return latest;
}

uint32_t referenceTimestamp(uint32_t preferred, uint32_t bookTimestamp) {
    if (RdTime::clockValid(preferred)) {
        s_lastKnownAt = std::max(s_lastKnownAt, preferred);
        return preferred;
    }
    const uint32_t latest = latestKnownFromBooks();
    if (RdTime::clockValid(latest)) return latest;
    return RdTime::clockValid(bookTimestamp) ? bookTimestamp : 0;
}

void updateBookReadTimestamp(ReadingBookStats &book, uint32_t preferred) {
    const uint32_t ref = referenceTimestamp(preferred, book.lastReadAt);
    if (!RdTime::clockValid(ref)) return;
    if (book.firstReadAt == 0) book.firstReadAt = ref;
    book.lastReadAt = ref;
}

void recordReadingTime(ReadingBookStats &book, uint32_t epochSeconds, uint64_t readingMs) {
    if (!RdTime::clockValid(epochSeconds) || readingMs == 0) return;
    const uint32_t ordinal = RdTime::dayOrdinal(epochSeconds);
    if (ordinal == 0) return;
    getOrCreateDay(book.readingDays, ordinal).readingMs += readingMs;
    getOrCreateDay(s_readingDays, ordinal).readingMs += readingMs;
}

void appendSessionLogEntry(uint32_t dayOrdinal, uint32_t sessionMs) {
    if (dayOrdinal == 0 || sessionMs == 0) return;
    s_sessionLog.push_back(ReadingSessionLogEntry{dayOrdinal, sessionMs});
    if (s_sessionLog.size() > MAX_SESSION_LOG_ENTRIES) {
        s_sessionLog.erase(s_sessionLog.begin(),
                           s_sessionLog.begin() +
                               static_cast<std::ptrdiff_t>(s_sessionLog.size() - MAX_SESSION_LOG_ENTRIES));
    }
}

bool countsForStreak(const ReadingDayStats &day) { return day.readingMs >= ReadingStats::goalMs(); }

uint32_t referenceDayOrdinal() {
    const uint32_t today = RdTime::todayOrdinal();
    if (today != 0) return today;
    if (!s_readingDays.empty()) return s_readingDays.back().dayOrdinal;
    return 0;
}

void rebuildSummaryCache() {
    SummaryCache cache;
    cache.referenceDayOrdinal = referenceDayOrdinal();
    cache.goalReadingMs = ReadingStats::goalMs();

    for (const auto &book : s_books) {
        cache.totalReadingMs += book.totalReadingMs;
        if (book.completed) cache.booksFinishedCount++;
    }

    if (cache.referenceDayOrdinal != 0) {
        const uint32_t start7 = (cache.referenceDayOrdinal >= 6) ? cache.referenceDayOrdinal - 6 : 0;
        const uint32_t start30 = (cache.referenceDayOrdinal >= 29) ? cache.referenceDayOrdinal - 29 : 0;

        std::vector<uint32_t> eligibleDays;
        eligibleDays.reserve(s_readingDays.size());

        for (const auto &day : s_readingDays) {
            if (day.dayOrdinal == cache.referenceDayOrdinal) cache.todayReadingMs = day.readingMs;
            if (day.dayOrdinal >= start7 && day.dayOrdinal <= cache.referenceDayOrdinal) {
                cache.recent7ReadingMs += day.readingMs;
            }
            if (day.dayOrdinal >= start30 && day.dayOrdinal <= cache.referenceDayOrdinal) {
                cache.recent30ReadingMs += day.readingMs;
            }
            if (countsForStreak(day)) eligibleDays.push_back(day.dayOrdinal);
        }

        if (!eligibleDays.empty()) {
            cache.maxStreakDays = 1;
            uint32_t run = 1;
            for (size_t i = 1; i < eligibleDays.size(); ++i) {
                run = (eligibleDays[i] == eligibleDays[i - 1] + 1) ? run + 1 : 1;
                cache.maxStreakDays = std::max(cache.maxStreakDays, run);
            }
            // "还活着" = 最后达标的一天是今天或昨天；否则当前连续为 0。
            const uint32_t latestEligible = eligibleDays.back();
            const bool alive = latestEligible == cache.referenceDayOrdinal ||
                               (cache.referenceDayOrdinal > 0 && latestEligible + 1 == cache.referenceDayOrdinal);
            if (alive) {
                cache.currentStreakDays = 1;
                for (size_t i = eligibleDays.size() - 1; i > 0; --i) {
                    if (eligibleDays[i] != eligibleDays[i - 1] + 1) break;
                    cache.currentStreakDays++;
                }
            }
        }
    }

    cache.valid = true;
    s_summary = cache;
}

void ensureSummary() {
    if (!s_summary.valid || s_summary.referenceDayOrdinal != referenceDayOrdinal() ||
        s_summary.goalReadingMs != ReadingStats::goalMs()) {
        rebuildSummaryCache();
    }
}

// 超限时按 lastReadAt 丢最旧的。丢掉的书连它的日桶一起从聚合表里消失，所以要重建。
void enforceBookLimit() {
    if (s_books.size() <= MAX_BOOKS) return;
    std::sort(s_books.begin(), s_books.end(),
              [](const ReadingBookStats &a, const ReadingBookStats &b) { return a.lastReadAt > b.lastReadAt; });
    s_books.resize(MAX_BOOKS);
    rebuildAggregatedDays();
}

}  // namespace

// ── 持久化 ───────────────────────────────────────────────────────────────
namespace {

JsonValue dayToJson(const ReadingDayStats &day) {
    JsonValue o = JsonValue::object();
    o.set("dayOrdinal", JsonValue(static_cast<double>(day.dayOrdinal)));
    o.set("readingMs", JsonValue(static_cast<double>(day.readingMs)));
    return o;
}

JsonValue bookToJson(const ReadingBookStats &book) {
    JsonValue o = JsonValue::object();
    o.set("path", JsonValue(book.path));
    o.set("title", JsonValue(book.title));
    o.set("author", JsonValue(book.author));
    o.set("chapterTitle", JsonValue(book.chapterTitle));
    o.set("totalReadingMs", JsonValue(static_cast<double>(book.totalReadingMs)));
    o.set("sessions", JsonValue(static_cast<double>(book.sessions)));
    o.set("lastSessionMs", JsonValue(static_cast<double>(book.lastSessionMs)));
    o.set("firstReadAt", JsonValue(static_cast<double>(book.firstReadAt)));
    o.set("lastReadAt", JsonValue(static_cast<double>(book.lastReadAt)));
    o.set("completedAt", JsonValue(static_cast<double>(book.completedAt)));
    o.set("lastProgressPercent", JsonValue(static_cast<double>(book.lastProgressPercent)));
    o.set("chapterProgressPercent", JsonValue(static_cast<double>(book.chapterProgressPercent)));
    o.set("completed", JsonValue(book.completed));
    JsonValue days = JsonValue::array();
    for (const auto &day : book.readingDays) days.pushBack(dayToJson(day));
    o.set("readingDays", days);
    return o;
}

std::vector<ReadingDayStats> daysFromJson(const JsonValue &arr) {
    std::vector<ReadingDayStats> days;
    if (!arr.isArray()) return days;
    days.reserve(arr.elements.size());
    for (const auto &e : arr.elements) {
        ReadingDayStats day;
        day.dayOrdinal = static_cast<uint32_t>(e["dayOrdinal"].asNumber(0));
        day.readingMs = static_cast<uint64_t>(e["readingMs"].asNumber(0));
        if (day.dayOrdinal != 0 && day.readingMs != 0) days.push_back(day);
    }
    return days;
}

}  // namespace

namespace ReadingStats {

uint64_t goalMs() {
    int minutes = g_settings.dailyGoalMinutes();
    if (minutes <= 0) minutes = 30;
    return static_cast<uint64_t>(minutes) * 60ULL * 1000ULL;
}

void load() {
    s_books.clear();
    s_readingDays.clear();
    s_sessionLog.clear();
    s_session = {};
    s_activePath.clear();
    s_lastSnapshot = {};
    s_sessionSerial = 0;
    s_lastKnownAt = 0;
    s_dirty = false;
    invalidateSummary();

    JsonValue root = JsonValue::loadFromFile(kStatsPath);
    if (root.isNull() || !root.isObject()) return;

    s_lastKnownAt = static_cast<uint32_t>(root["lastKnownAt"].asNumber(0));

    if (root["books"].isArray()) {
        s_books.reserve(root["books"].elements.size());
        for (const auto &b : root["books"].elements) {
            ReadingBookStats book;
            book.path = b["path"].asString();
            if (book.path.empty()) continue;
            book.title = b["title"].asString();
            book.author = b["author"].asString();
            book.chapterTitle = b["chapterTitle"].asString();
            book.totalReadingMs = static_cast<uint64_t>(b["totalReadingMs"].asNumber(0));
            book.sessions = static_cast<uint32_t>(b["sessions"].asNumber(0));
            book.lastSessionMs = static_cast<uint32_t>(b["lastSessionMs"].asNumber(0));
            book.firstReadAt = static_cast<uint32_t>(b["firstReadAt"].asNumber(0));
            book.lastReadAt = static_cast<uint32_t>(b["lastReadAt"].asNumber(0));
            book.completedAt = static_cast<uint32_t>(b["completedAt"].asNumber(0));
            book.lastProgressPercent = clampPercent(static_cast<int>(b["lastProgressPercent"].asNumber(0)));
            book.chapterProgressPercent = clampPercent(static_cast<int>(b["chapterProgressPercent"].asNumber(0)));
            book.completed = b["completed"].asBool(false);
            book.readingDays = daysFromJson(b["readingDays"]);
            normalizeReadingDays(book.readingDays);
            s_books.push_back(std::move(book));
        }
    }

    s_sessionLog.reserve(root["sessionLog"].isArray() ? root["sessionLog"].elements.size() : 0);
    if (root["sessionLog"].isArray()) {
        for (const auto &e : root["sessionLog"].elements) {
            ReadingSessionLogEntry entry;
            entry.dayOrdinal = static_cast<uint32_t>(e["dayOrdinal"].asNumber(0));
            entry.sessionMs = static_cast<uint32_t>(e["sessionMs"].asNumber(0));
            if (entry.dayOrdinal != 0 && entry.sessionMs != 0) s_sessionLog.push_back(entry);
        }
    }
    if (s_sessionLog.size() > MAX_SESSION_LOG_ENTRIES) {
        s_sessionLog.erase(s_sessionLog.begin(),
                           s_sessionLog.begin() +
                               static_cast<std::ptrdiff_t>(s_sessionLog.size() - MAX_SESSION_LOG_ENTRIES));
    }

    // 聚合日表一律从各本书重算，不信文件里的那一份（防止手改/半截写）。
    rebuildAggregatedDays();
    s_lastKnownAt = std::max(s_lastKnownAt, latestKnownFromBooks());
    enforceBookLimit();
    invalidateSummary();
}

bool save() {
    JsonValue root = JsonValue::object();
    root.set("formatVersion", JsonValue(static_cast<double>(kFormatVersion)));
    root.set("lastKnownAt", JsonValue(static_cast<double>(s_lastKnownAt)));

    JsonValue days = JsonValue::array();
    for (const auto &day : s_readingDays) days.pushBack(dayToJson(day));
    root.set("readingDays", days);

    JsonValue sessions = JsonValue::array();
    for (const auto &entry : s_sessionLog) {
        JsonValue o = JsonValue::object();
        o.set("dayOrdinal", JsonValue(static_cast<double>(entry.dayOrdinal)));
        o.set("sessionMs", JsonValue(static_cast<double>(entry.sessionMs)));
        sessions.pushBack(o);
    }
    root.set("sessionLog", sessions);

    JsonValue books = JsonValue::array();
    for (const auto &book : s_books) books.pushBack(bookToJson(book));
    root.set("books", books);

    s_lastSaveAttemptMs = nowMs();
    const bool ok = JsonValue::saveToFile(kStatsPath, root);
    if (ok) {
        s_dirty = false;
        s_dirtySinceMs = 0;
    }
    return ok;
}

bool shouldCheckpoint() {
    if (!s_dirty || !s_session.active) return false;
    const int64_t now = nowMs();
    return (now - s_dirtySinceMs) >= CHECKPOINT_INTERVAL_MS &&
           (s_lastSaveAttemptMs == 0 || (now - s_lastSaveAttemptMs) >= CHECKPOINT_RETRY_INTERVAL_MS);
}

void beginSession(const std::string &path, const std::string &title, const std::string &author, uint8_t progressPercent,
                  const std::string &chapterTitle, uint8_t chapterProgressPercent) {
    if (path.empty()) return;
    if (s_session.active) endSession();

    const size_t index = getOrCreateBookIndex(path, title, author);
    touchBook(index);

    auto &book = s_books[0];
    s_session.startProgressPercent = book.lastProgressPercent;
    s_session.startCompleted = book.completed;
    book.lastProgressPercent = clampPercent(progressPercent);
    book.chapterTitle = chapterTitle;
    book.chapterProgressPercent = clampPercent(chapterProgressPercent);
    if (book.lastProgressPercent >= 100) book.completed = true;

    updateBookReadTimestamp(book, RdTime::nowEpoch());

    s_session.active = true;
    s_session.lastInteractionMs = nowMs();
    s_session.accumulatedMs = 0;
    s_activePath = path;

    markDirty();
}

void noteActivity() {
    if (!s_session.active) return;

    const int64_t now = nowMs();
    const int64_t elapsed = now - s_session.lastInteractionMs;
    const int64_t credited = std::min(elapsed, MAX_READING_GAP_MS);

    if (credited > 0) {
        auto &book = s_books[0];
        book.totalReadingMs += static_cast<uint64_t>(credited);
        s_session.accumulatedMs += static_cast<uint64_t>(credited);
        const uint32_t ref = referenceTimestamp(RdTime::nowEpoch(), book.lastReadAt);
        recordReadingTime(book, ref, static_cast<uint64_t>(credited));
        updateBookReadTimestamp(book, ref);
        markDirty();
    }

    s_session.lastInteractionMs = now;
}

void tickActiveSession() {
    if (!s_session.active) return;
    if ((nowMs() - s_session.lastInteractionMs) < SESSION_HEARTBEAT_MS) return;
    noteActivity();
}

void resumeSession() {
    if (!s_session.active) return;
    s_session.lastInteractionMs = nowMs();
}

void updateProgress(uint8_t progressPercent, bool completed, const std::string &chapterTitle,
                    uint8_t chapterProgressPercent) {
    if (!s_session.active) return;

    auto &book = s_books[0];
    const uint8_t newBookProgress = clampPercent(progressPercent);
    const uint8_t newChapterProgress = clampPercent(chapterProgressPercent);
    const bool completionChanged = !book.completed && (completed || newBookProgress >= 100);

    if (book.lastProgressPercent == newBookProgress && book.chapterTitle == chapterTitle &&
        book.chapterProgressPercent == newChapterProgress && !completionChanged) {
        return;
    }

    book.lastProgressPercent = newBookProgress;
    book.chapterTitle = chapterTitle;
    book.chapterProgressPercent = newChapterProgress;
    if (completed || newBookProgress >= 100) book.completed = true;

    updateBookReadTimestamp(book, RdTime::nowEpoch());
    if (completionChanged && book.completedAt == 0) book.completedAt = book.lastReadAt;

    markDirty();
}

void endSession() {
    if (!s_session.active) {
        s_lastSnapshot = {};
        s_activePath.clear();
        return;
    }

    noteActivity();

    auto &book = s_books[0];
    const bool counted = s_session.accumulatedMs >= MIN_SESSION_READING_MS;
    const uint32_t sessionMs = (s_session.accumulatedMs > 0xFFFFFFFFULL)
                                   ? 0xFFFFFFFFu
                                   : static_cast<uint32_t>(s_session.accumulatedMs);
    if (counted) {
        book.sessions++;
        book.lastSessionMs = sessionMs;
        const uint32_t stamp = referenceTimestamp(RdTime::nowEpoch(), book.lastReadAt);
        const uint32_t ordinal = RdTime::dayOrdinal(stamp);
        if (ordinal != 0) appendSessionLogEntry(ordinal, sessionMs);
        markDirty();
    }

    s_lastSnapshot.valid = true;
    s_lastSnapshot.serial = ++s_sessionSerial;
    s_lastSnapshot.path = book.path;
    s_lastSnapshot.sessionMs = sessionMs;
    s_lastSnapshot.counted = counted;
    s_lastSnapshot.completedThisSession = !s_session.startCompleted && book.completed;
    s_lastSnapshot.startProgressPercent = s_session.startProgressPercent;
    s_lastSnapshot.endProgressPercent = book.lastProgressPercent;

    s_session = {};
    s_activePath.clear();
}

bool hasActiveSession() { return s_session.active; }

const std::string &activePath() { return s_activePath; }

// ── 查询 ─────────────────────────────────────────────────────────────────
const std::vector<ReadingBookStats> &books() { return s_books; }
const std::vector<ReadingDayStats> &readingDays() { return s_readingDays; }
const std::vector<ReadingSessionLogEntry> &sessionLog() { return s_sessionLog; }
const ReadingSessionSnapshot &lastSessionSnapshot() { return s_lastSnapshot; }

const ReadingBookStats *findBook(const std::string &path) {
    const size_t index = findBookIndex(path);
    return index < s_books.size() ? &s_books[index] : nullptr;
}

uint32_t booksStarted() { return static_cast<uint32_t>(s_books.size()); }

uint32_t booksFinished() {
    ensureSummary();
    return s_summary.booksFinishedCount;
}

uint64_t totalReadingMs() {
    ensureSummary();
    return s_summary.totalReadingMs;
}

uint64_t todayReadingMs() {
    ensureSummary();
    return s_summary.todayReadingMs;
}

uint64_t recentReadingMs(uint32_t days) {
    if (days == 0) return 0;
    ensureSummary();
    if (days <= 7) return s_summary.recent7ReadingMs;
    if (days <= 30) return s_summary.recent30ReadingMs;

    if (s_summary.referenceDayOrdinal == 0) return 0;
    const uint32_t start = (s_summary.referenceDayOrdinal >= days - 1) ? s_summary.referenceDayOrdinal - (days - 1) : 0;
    uint64_t total = 0;
    for (const auto &day : s_readingDays) {
        if (day.dayOrdinal >= start && day.dayOrdinal <= s_summary.referenceDayOrdinal) total += day.readingMs;
    }
    return total;
}

uint32_t currentStreakDays() {
    ensureSummary();
    return s_summary.currentStreakDays;
}

uint32_t maxStreakDays() {
    ensureSummary();
    return s_summary.maxStreakDays;
}

uint32_t readDaysCount() {
    uint32_t count = 0;
    for (const auto &day : s_readingDays) {
        if (day.readingMs > 0) count++;
    }
    return count;
}

// ── 改动 ─────────────────────────────────────────────────────────────────
bool adjustBookReadingTime(const std::string &path, uint32_t dayOrdinal, int32_t deltaMs) {
    if (dayOrdinal == 0 || deltaMs == 0) return false;
    const size_t index = findBookIndex(path);
    if (index >= s_books.size()) return false;

    auto &book = s_books[index];
    if (deltaMs > 0) {
        const uint64_t added = static_cast<uint64_t>(deltaMs);
        addReadingToDays(book.readingDays, dayOrdinal, added);
        book.totalReadingMs += added;
    } else {
        const uint64_t requested = static_cast<uint64_t>(-static_cast<int64_t>(deltaMs));
        auto it = std::lower_bound(book.readingDays.begin(), book.readingDays.end(), dayOrdinal,
                                   [](const ReadingDayStats &day, uint32_t ordinal) {
                                       return day.dayOrdinal < ordinal;
                                   });
        if (it == book.readingDays.end() || it->dayOrdinal != dayOrdinal || it->readingMs < requested ||
            book.totalReadingMs < requested) {
            return false;
        }
        it->readingMs -= requested;
        if (it->readingMs == 0) book.readingDays.erase(it);
        book.totalReadingMs -= requested;
    }

    rebuildAggregatedDays();
    markDirty();
    return save();
}

bool removeBook(const std::string &path) {
    const size_t index = findBookIndex(path);
    if (index >= s_books.size()) return false;

    const bool hadDays = !s_books[index].readingDays.empty();
    s_books.erase(s_books.begin() + static_cast<std::ptrdiff_t>(index));

    if (s_session.active && s_activePath == path) {
        s_session = {};
        s_activePath.clear();
    }
    if (hadDays) rebuildAggregatedDays();
    markDirty();
    return save();
}

std::string formatDurationHm(uint64_t totalMs) {
    const uint64_t totalMinutes = totalMs / 60000ULL;
    const uint64_t hours = totalMinutes / 60ULL;
    const uint64_t minutes = totalMinutes % 60ULL;
    if (hours == 0) return std::to_string(minutes) + "m";
    return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
}

}  // namespace ReadingStats

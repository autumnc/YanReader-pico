// 已保存 WiFi 网络表（wifi_store.cpp）的纯函数对拍：序列化 / 解析 / 转义 的往返。
//
// 为什么值得对拍：这张表存的是**用户的 WiFi 密码**，而且格式里有三层"容易悄悄吃字符"
// 的地方 —— ① 字段分隔符 \x1f 与转义、② SSID 首尾空格（手机热点真有）、③ 密码里可能
// 出现反斜杠 / 制表符 / 换行（从别处粘贴）。任何一处写歪，症状都是"明明密码打对了，
// 就是连不上"，而在设备上根本看不出来是数据层的错。
//
// 这些断言在主机上跑得动，是因为 serialize/parse/escapeField/unescapeField 是**不碰
// 文件系统**的静态函数（见 wifi_store.h 的说明）。load/save 那两个引用读盘写盘的符号
// 由 stubs.cpp 提供空实现。
//
//   tests/host/wifi_store/run.sh        # 应全绿
//
// 依赖：只要 g++（不需要 IDF 环境）。

#include "wifi_store.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_pass = 0;

static std::string hexOf(const std::string &s) {
    std::string out;
    char buf[8];
    for (unsigned char c : s) {
        std::snprintf(buf, sizeof(buf), "%02x ", c);
        out += buf;
    }
    if (!out.empty()) out.pop_back();
    return out;
}

static void ok(bool cond, const char *what) {
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        std::printf("FAIL: %s\n", what);
    }
}

static void eqStr(const std::string &got, const std::string &want, const char *what) {
    if (got == want) {
        g_pass++;
        return;
    }
    g_fail++;
    std::printf("FAIL: %s\n   want=[%s] (%s)\n   got =[%s] (%s)\n", what, want.c_str(),
                hexOf(want).c_str(), got.c_str(), hexOf(got).c_str());
}

// ── 转义 / 反转义 ────────────────────────────────────────────────────────
static void testEscape() {
    // 往返：任意字节串转义再反转义都必须一模一样。这里逐个把"可疑字节"都点一遍。
    const char *cases[] = {
        "",
        "hello",
        "My WiFi",                      // 普通空格
        "  padded ssid  ",              // 首尾空格（手机热点真的有）
        "back\\slash",                  // 反斜杠
        "tab\there",
        "new\nline",                    // 密码里真出现换行（粘贴来的）
        "cr\rhere",
        "del\177char",
        "\037",                         // 就是分隔符本身
        "a\037b",
        "\\x41",                        // 长得像转义序列的**字面量**
        "\\",                           // 单个反斜杠
        "\\\\",                         // 两个
        "密码中文",                      // 多字节 UTF-8（每个字节 >= 0x80，不该被转义）
    };
    for (const char *c : cases) {
        const std::string s(c);
        const std::string e = WifiStore::escapeField(s);
        eqStr(WifiStore::unescapeField(e), s,
              (std::string("escape/unescape round-trip: [") + hexOf(s) + "]").c_str());
        // 转义之后**不能再出现裸的 \x1f 或裸换行**，否则按行/按字段切分就碎了。
        ok(e.find('\037') == std::string::npos, "escaped field must not contain raw 0x1f");
        ok(e.find('\n') == std::string::npos, "escaped field must not contain raw newline");
    }

    // 具体形态：反斜杠两倍、控制字节 \xHH
    eqStr(WifiStore::escapeField("\\"), "\\\\", "backslash escapes to double");
    eqStr(WifiStore::escapeField("\037"), "\\x1f", "0x1f escapes to \\x1f");
    eqStr(WifiStore::escapeField("\n"), "\\x0a", "newline escapes to \\x0a");
    eqStr(WifiStore::escapeField("a\\b"), "a\\\\b", "mixed");
    // 可打印 ASCII 原样
    eqStr(WifiStore::escapeField("WPA2-psk_1!@#"), "WPA2-psk_1!@#", "printable untouched");

    // 容错：非法/未知转义**原样保留**（不吞字符）—— 手改过文件或旧版本写坏的行
    // 不该把后面半行吃掉。
    eqStr(WifiStore::unescapeField("\\xzz"), "\\xzz", "invalid \\xHH kept literally");
    eqStr(WifiStore::unescapeField("\\q"), "\\q", "unknown escape kept literally");
    eqStr(WifiStore::unescapeField("trailing\\"), "trailing\\", "dangling backslash kept");
}

// ── 序列化 / 解析往返 ────────────────────────────────────────────────────
static std::vector<WifiSavedNet> sampleTable() {
    std::vector<WifiSavedNet> v;
    {
        WifiSavedNet n;
        n.ssid = "Home-5G";
        n.pass = "p@ss\\word";
        n.open = false;
        n.lastUsed = 1700000000;
        v.push_back(n);
    }
    {
        WifiSavedNet n;
        n.ssid = "  Guest  ";   // 首尾空格
        n.pass = "";
        n.open = true;
        n.lastUsed = 1699999999;
        v.push_back(n);
    }
    {
        WifiSavedNet n;
        n.ssid = "分隔\037符";
        n.pass = "换\n行";
        n.open = false;
        n.lastUsed = 0;   // 从没连过 / 时钟没校准
        v.push_back(n);
    }
    return v;
}

static void testSerializeParse() {
    const std::vector<WifiSavedNet> in = sampleTable();
    const std::string blob = WifiStore::serialize(in);

    // 一行一条
    int lines = 0;
    for (char c : blob)
        if (c == '\n') lines++;
    ok(lines == 3, "serialize emits one line per network");

    const std::vector<WifiSavedNet> out = WifiStore::parse(blob);
    ok(out.size() == in.size(), "parse returns the same count");
    if (out.size() != in.size()) return;
    for (size_t i = 0; i < in.size(); i++) {
        eqStr(out[i].ssid, in[i].ssid, "ssid survives round-trip");
        eqStr(out[i].pass, in[i].pass, "password survives round-trip");
        ok(out[i].open == in[i].open, "open flag survives round-trip");
        ok(out[i].lastUsed == in[i].lastUsed, "lastUsed survives round-trip");
    }
    // 顺序 = 最近用过的在前（序列化不重排，load 后由 remember/touch 维护）
    ok(out[0].ssid == "Home-5G", "order preserved (row 0)");
    ok(out[2].ssid == "分隔\037符", "order preserved (row 2)");

    // 空表 ↔ 空串
    eqStr(WifiStore::serialize({}), "", "empty table serializes to empty string");
    ok(WifiStore::parse("").empty(), "empty content parses to empty table");
}

static void testOpenFlag() {
    // 密码为空 → 开放网络（哪怕 flags 写的是 0）
    const std::string forced = std::string("NoPass\037\0370\0371\n");
    const auto v1 = WifiStore::parse(forced);
    ok(v1.size() == 1 && v1[0].open, "empty password implies open even if flags=0");

    // 密码非空但 flags 写着 1（手改过）→ 认 flags
    const std::string flagged = std::string("Weird\037secret\0371\0371\n");
    const auto v2 = WifiStore::parse(flagged);
    ok(v2.size() == 1 && v2[0].open, "flags bit0 wins when password is non-empty");

    // flags 只有 bit0 有意义，别的位忽略
    const std::string extra = std::string("Bits\037pw\0373\0371\n");
    const auto v3 = WifiStore::parse(extra);
    ok(v3.size() == 1 && v3[0].open, "only bit0 of flags is read");
}

static void testParseTolerance() {
    // 空行 / CRLF / 最后一行没有换行 / 段数不足的坏行：**只丢坏行，不整表作废**。
    std::string blob;
    blob += "\r\n";                                   // 空行
    blob += "Good1\037pw1\0370\03711\r\n";            // CRLF 结尾
    blob += "broken\037only-two\n";                   // 段数不足 → 跳过
    blob += "\n";                                     // 又是空行
    blob += "\037pw\0370\03722\n";                    // 空 SSID → 跳过
    blob += "Good2\037pw2\0370\03733";                // 末行没有 \n
    const auto v = WifiStore::parse(blob);
    ok(v.size() == 2, "malformed rows are skipped, good rows kept");
    if (v.size() == 2) {
        eqStr(v[0].ssid, "Good1", "CRLF line parsed");
        ok(v[0].lastUsed == 11, "CRLF line's lastUsed parsed");
        eqStr(v[1].ssid, "Good2", "unterminated last line parsed");
        ok(v[1].lastUsed == 33, "unterminated last line's lastUsed parsed");
    }

    // 垃圾 lastUsed（非数字）→ atoll 给 0，不炸
    const auto v2 = WifiStore::parse(std::string("X\037p\0370\037NotANumber\n"));
    ok(v2.size() == 1 && v2[0].lastUsed == 0, "non-numeric lastUsed degrades to 0");

    // 少一列（只有 3 段）也跳过
    ok(WifiStore::parse(std::string("A\037B\0370\n")).empty(), "3-field row skipped");
}

// ── 转义与切分是一对：正文里的分隔符不该把一行切成两行 ──────────────────
static void testEscapeProtectsStructure() {
    std::vector<WifiSavedNet> v;
    WifiSavedNet n;
    n.ssid = "a\037b";
    n.pass = "c\nd";
    n.open = false;
    n.lastUsed = 7;
    v.push_back(n);
    const std::string blob = WifiStore::serialize(v);
    ok(blob.find('\n') == blob.size() - 1, "serialized row has exactly one trailing newline");
    const auto out = WifiStore::parse(blob);
    ok(out.size() == 1, "one row in, one row out despite embedded separators");
    if (out.size() == 1) {
        eqStr(out[0].ssid, "a\037b", "embedded 0x1f survives");
        eqStr(out[0].pass, "c\nd", "embedded newline survives");
    }
}

int main() {
    testEscape();
    testSerializeParse();
    testOpenFlag();
    testParseTolerance();
    testEscapeProtectsStructure();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

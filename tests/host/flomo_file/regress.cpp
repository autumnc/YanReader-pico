// flomo 本地库「每次进入都是暂无笔记」的根因回归测试（主机端，无 IDF、无设备）。
//
// 症状：进 flomo 不按同步就是空的；按了同步（从服务器拉）才有内容，退出再进又空了。
// 根因（已由 IDF/FatFS 源码核实）：flomoEnsureDir 的走目录循环要求 mkdir 返回 0 或
// EEXIST，而**第一段就是挂载点 /sdcard** —— ESP-IDF 的 mkdir 对已存在的目录返回的是
// EINVAL(22)，不是 EEXIST：
//   1) VFS 把挂载点前缀剥掉，剩空名交给 vfs_fat_mkdir；
//   2) f_mkdir("0:") → follow_path → create_name 见空名直接 FR_INVALID_NAME（ff.c:2946）；
//   3) fresult_to_errno 把 FR_INVALID_NAME 映射成 EINVAL（vfs_fat.c:346）。
// 于是老写法在第一段就 return false，/sdcard/flomo 从来没被建出来：
//   MemoDb::load  → fopen 打不开（目录不存在）→ 空串 → 「暂无笔记」；
//   MemoDb::save  → flomoSafeWriteFile 里 `if (!flomoEnsureDir(...)) return false`
//                   → **一条也写不进去**。两处返回值都被调用方忽略，屏上一点动静都没有。
//
// Linux 的 mkdir 对已存在目录一律 EEXIST（/sys、/proc、/dev/shm 全试过，都是 17），
// 直接在主机上跑复现不出来，所以用 -Dmkdir=test_mkdir 把 flomo_file.cpp 里的 mkdir
// 换成"ESP-IDF 语义"的桩。run.sh --verify-fix 再从 git HEAD 取**修复前**的
// flomo_file.cpp 重跑同一套测例，断言它挂（证明测例抓得住这个 bug，而不是永远绿）。
//
// 退出码 0 = 全部通过。

#include "flomo_db.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// ── 把 mkdir 换成 ESP-IDF 语义 ───────────────────────────────────────────────
// 本文件是用 -Dmkdir=test_mkdir 编的：**先 undef 再用**，否则下面桩里那句"真 mkdir"
// 会被替换成它自己 → 无限递归。
#undef mkdir
extern "C" int mkdir(const char *path, mode_t mode) noexcept;

extern "C" int test_mkdir(const char *p, mode_t m) noexcept {
    struct stat st;
    if (stat(p, &st) == 0) {
        // 已存在（挂载点就是这个情形）：ESP-IDF 给的是 EINVAL，不是 EEXIST。
        errno = EINVAL;
        return -1;
    }
    return mkdir(p, m);
}

// ── flomo_api.cpp 的最小桩 ──────────────────────────────────────────────────
// 那份 TU 拖着 net/http + md5 + FreeRTOS，编不进主机端。这里只补它导出的三个符号。
// htmlToText 用直通：MemoDb::load 会用文件里的 content_text 覆盖它，往返不受影响。
std::string htmlToText(const std::string &html) { return html; }

std::vector<std::string> extractTags(const std::string &text) {
    std::vector<std::string> tags;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '#') continue;
        size_t j = i + 1;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n' && text[j] != '#') ++j;
        if (j > i + 1) tags.push_back(text.substr(i + 1, j - i - 1));
        i = j;
    }
    return tags;
}

Memo memoFromJson(const JsonValue &v) {
    Memo m;
    m.slug = v["slug"].asString();
    m.contentHtml = v["content"].asString();
    m.contentText = htmlToText(m.contentHtml);
    m.createdAt = v["created_at"].asString();
    m.updatedAt = v["updated_at"].asString();
    m.deleted = !v["deleted_at"].isNull() && !v["deleted_at"].asString().empty();
    const JsonValue &tags = v["tags"];
    if (tags.isArray())
        for (size_t i = 0; i < tags.size(); ++i) m.tags.push_back(tags[i].asString());
    if (m.tags.empty()) m.tags = extractTags(m.contentText);
    return m;
}

// ── 被测的 ensureDir ────────────────────────────────────────────────────────
#include "flomo_file.h"

// ── 断言 ────────────────────────────────────────────────────────────────────
static int g_fail = 0, g_total = 0;
static void check(const char *what, bool ok) {
    g_total++;
    if (!ok) g_fail++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static std::string g_root;

static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

// 每次跑都从干净目录开始：把上一轮的产物删掉。
static void rmrf(const std::string &p) {
    std::string cmd = "rm -rf '" + p + "'";
    if (system(cmd.c_str()) != 0) { /* 首次本来就没有 */ }
}

int main() {
    const char *tmp = getenv("TMPDIR");
    g_root = std::string(tmp && *tmp ? tmp : "/tmp") + "/flomo_host_test";
    // 固定用同一个根（而不是 mkdtemp）：--verify-fix 要能在**同一套输入**上跑。
    rmrf(g_root);
    mkdir(g_root.c_str(), 0777);

    printf("== flomo 本地库 ==\n");

    // ── 1. 深路径建目录：主机上这串的第一段（/tmp）已存在 = 设备上的 /sdcard ──
    {
        printf("[1] 从零建出 <tmp>/a/b/c\n");
        std::string leaf = g_root + "/a/b/c";
        bool ok = flomoEnsureDir(leaf);
        check("flomoEnsureDir 返回 true", ok);
        check("叶子目录真的建出来了", exists(leaf));
        check("重复调用仍返回 true（幂等）", flomoEnsureDir(leaf));
    }

    // ── 2. 落库 → 退出重进（整条用户可见链路）──────────────────────────────
    {
        printf("[2] 保存整库 → 全新 Store 重新加载\n");
        std::string dir = g_root + "/a/b/c/flomo";
        {
            Store s;
            s.token = "tok-abc";
            s.lastSync = "2026-10-05 12:00:00";
            Memo remote;
            remote.slug = "slug-remote";
            remote.contentHtml = "<p>远端笔记</p>";
            remote.contentText = "第一行\n第二行 带\"引号\"和\\反斜杠 中文";
            remote.createdAt = "2026-10-01 10:00:00";
            remote.updatedAt = "2026-10-02 11:00:00";
            remote.tags = {"读书"};
            Memo local;
            local.slug = "local-1759600000";
            local.contentText = "本地新建·还没上传";
            local.updatedAt = "2026-10-05 09:00:00";
            local.dirty = true;
            local.pendingOp = "create";
            s.memos.push_back(remote);
            s.memos.push_back(local);
            MemoDb db(dir);
            check("MemoDb::save 返回 true", db.save(s));
        }
        check("memos.json 真的写出来了", exists(dir + "/memos.json"));
        {
            Store s2;
            MemoDb db(dir);
            check("MemoDb::load 返回 true", db.load(s2));
            check("重新加载拿到 2 条", s2.memos.size() == 2);
            check("token 留住", s2.token == "tok-abc");
            check("last_sync 留住", s2.lastSync == "2026-10-05 12:00:00");
            bool found = false, dirtyKept = false;
            for (auto &m : s2.memos) {
                if (m.slug == "slug-remote" && m.contentText == "第一行\n第二行 带\"引号\"和\\反斜杠 中文" &&
                    m.tags.size() == 1 && m.tags[0] == "读书")
                    found = true;
                if (m.slug == "local-1759600000" && m.dirty && m.pendingOp == "create")
                    dirtyKept = true;
            }
            check("正文/引号/换行/标签逐字节一致", found);
            check("本地未上传标记（dirty + pending_op）留住", dirtyKept);
        }
    }

    // ── 3. 库还不存在时（第一次用）：空库，不炸 ─────────────────────────────
    {
        printf("[3] 第一次进入（还没有 memos.json）\n");
        Store s;
        MemoDb db(g_root + "/a/b/c/none");
        check("load 返回 true", db.load(s));
        check("空库 = 0 条（就是屏上的「暂无笔记」）", s.memos.empty());
    }

    // ── 4. 掉电残留：memos.json 丢了但有 .bak → 自动找回 ────────────────────
    {
        printf("[4] 只有 .bak（保存中途掉电）→ 读的时候自动找回\n");
        std::string dir = g_root + "/a/b/c/flomo";
        std::string f = dir + "/memos.json", bak = f + ".bak";
        check("rename → .bak", rename(f.c_str(), bak.c_str()) == 0);
        check("memos.json 确实没了", !exists(f));
        Store s;
        MemoDb db(dir);
        db.load(s);
        check("从 .bak 恢复出 2 条", s.memos.size() == 2);
        check("memos.json 又回来了", exists(f));
    }

    printf("\n%s（通过 %d/%d）\n", g_fail ? "有失败" : "全部通过", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}

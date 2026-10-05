#include "safe_file.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int g_fail = 0;

void ok(bool cond, const char *name) {
    if (cond) {
        std::printf("ok   %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++g_fail;
    }
}

std::string tempDir() {
    char tmpl[] = "/tmp/yanreader-safe-file-XXXXXX";
    char *p = mkdtemp(tmpl);
    return p ? std::string(p) : std::string();
}

bool writeRaw(const std::string &path, const std::string &body) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t n = std::fwrite(body.data(), 1, body.size(), f);
    return std::fclose(f) == 0 && n == body.size();
}

void removeTree(const std::string &dir) {
    if (dir.empty()) return;
    std::remove((dir + "/missing.txt.tmp").c_str());
    std::remove((dir + "/missing.txt.bak").c_str());
    std::remove((dir + "/small.txt").c_str());
    std::remove((dir + "/big.txt").c_str());
    std::remove((dir + "/missing.txt").c_str());
    rmdir(dir.c_str());
}

}  // namespace

int main() {
    const std::string dir = tempDir();
    ok(!dir.empty(), "created temp dir");
    if (dir.empty()) return 1;

    const std::string small = dir + "/small.txt";
    bool truncated = true;
    ok(writeRaw(small, "hello"), "wrote small file");
    ok(readWholeFile(small, 16, &truncated) == "hello", "small file read");
    ok(!truncated, "small file not truncated");

    const std::string big = dir + "/big.txt";
    ok(writeRaw(big, std::string(1500, 'x')), "wrote big file");
    truncated = false;
    ok(readWholeFile(big, 1024, &truncated).empty(), "regular file over limit is skipped");
    ok(truncated, "regular file over limit reports truncated");

    truncated = false;
    ok(readWholeFile(big, 0, &truncated).size() == 1500, "zero max reads without limit");
    ok(!truncated, "zero max does not report truncated");

    const std::string missing = dir + "/missing.txt";
    ok(writeRaw(missing + ".bak", "backup"), "wrote backup file");
    truncated = true;
    ok(readWholeFile(missing, 16, &truncated) == "backup", "bak is repaired before read");
    ok(!truncated, "repaired backup not truncated");
    ok(fileExists(missing), "repaired final exists");

    removeTree(dir);
    if (g_fail != 0) {
        std::printf("FAIL: %d safe_file checks failed\n", g_fail);
        return 1;
    }
    std::puts("PASS: safe_file");
    return 0;
}


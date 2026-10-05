#include "net/http_cap.h"

#include <cstdio>

static int fails = 0;

static void check(const char* name, size_t got, size_t chunk, size_t cap, bool hard,
                  size_t keep, bool overflow, bool truncated) {
    const net::CapDecision d = net::decideCapKeep(got, chunk, cap, hard);
    if (d.keep == keep && d.overflow == overflow && d.truncated == truncated) {
        std::printf("ok   %s\n", name);
        return;
    }
    std::printf("FAIL %s: got keep=%zu overflow=%d truncated=%d\n",
                name, d.keep, (int)d.overflow, (int)d.truncated);
    fails++;
}

int main() {
    check("no cap keeps whole chunk", 0, 1024, 0, false, 1024, false, false);
    check("soft cap exact fit", 512, 512, 1024, false, 512, false, false);
    check("soft cap clips crossing chunk", 900, 256, 1024, false, 124, false, true);
    check("soft cap discards after cap", 1024, 256, 1024, false, 0, false, true);
    check("hard cap exact fit", 512, 512, 1024, true, 512, false, false);
    check("hard cap rejects crossing chunk", 900, 256, 1024, true, 0, true, false);
    check("hard cap rejects after cap", 1024, 1, 1024, true, 0, true, false);
    if (fails == 0) {
        std::puts("PASS: http cap decisions");
        return 0;
    }
    return 1;
}

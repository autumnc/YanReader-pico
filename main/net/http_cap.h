#pragma once

#include <cstddef>

namespace net {

struct CapDecision {
    size_t keep = 0;
    bool overflow = false;
    bool truncated = false;
};

inline CapDecision decideCapKeep(size_t got, size_t chunk, size_t cap, bool capHard) {
    CapDecision d{chunk, false, false};
    if (cap == 0) return d;
    if (got >= cap) {
        d.keep = 0;
        if (capHard) d.overflow = true;
        else d.truncated = true;
        return d;
    }
    const size_t room = cap - got;
    if (chunk <= room) return d;
    if (capHard) {
        d.keep = 0;
        d.overflow = true;
        return d;
    }
    d.keep = room;
    d.truncated = true;
    return d;
}

}  // namespace net

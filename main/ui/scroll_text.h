#pragma once

#include <algorithm>

#include "pjournal_app.h"

inline bool scrollTextKey(int key, int &scroll, int visible, int maxScroll, bool vertical = false) {
    if (visible < 1) visible = 1;
    if (maxScroll < 0) maxScroll = 0;
    const int downKey = vertical ? KEY_LEFT : KEY_DOWN;
    const int upKey = vertical ? KEY_RIGHT : KEY_UP;
    if (key == 'j' || key == downKey) {
        scroll++;
    } else if (key == 'k' || key == upKey) {
        scroll--;
    } else if (key == KEY_PAGE_DOWN) {
        scroll += visible;
    } else if (key == KEY_PAGE_UP) {
        scroll -= visible;
    } else {
        return false;
    }
    scroll = std::max(0, std::min(scroll, maxScroll));
    return true;
}

// Host-test shim for main/pjournal_app.h.
//
// IME.cpp includes pjournal_app.h only for KEY_META_TAP (the dedicated modifier-tap
// keycode). The real header drags in ui_helpers.h -> font_renderer.h -> ESP-IDF/LVGL
// headers, which the host harness cannot compile. -I puts this directory first, so
// this enum-only shim shadows it. Value must match main/pjournal_app.h.
#pragma once

#define KEY_META_TAP 0xB3

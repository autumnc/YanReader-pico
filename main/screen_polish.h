#pragma once

#include "pjournal_app.h"

// AI polish panel: editor BOOT double-click (whole text) or Ctrl+O (selection)
// → DeepSeek text polish. Result page with confirm / re-polish / cancel.
enum PolishScope { POLISH_WHOLE, POLISH_SELECTION };

// Select which editor text the next panel session polishes (set before the
// panel is entered; read by screen_polish_init).
void screen_polish_set_scope(PolishScope scope);

void screen_polish_init();
AppState screen_polish_handle(int key, ScreenContext &ctx);

#pragma once

#include <cstdint>

void ui_feedback_message(const char *message, uint32_t ms = 1800);
void ui_feedback_titled_message(const char *title, const char *message, uint32_t ms = 1800);

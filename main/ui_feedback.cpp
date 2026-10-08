#include "ui_feedback.h"

#include "font_renderer.h"
#include "ui_helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void ui_feedback_message(const char *message, uint32_t ms) {
    ui_clear();
    ui_show_message_centered(message ? message : "");
    ui_commit();
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
}

void ui_feedback_titled_message(const char *title, const char *message, uint32_t ms) {
    ui_clear();
    if (title && title[0]) ui_draw_text_centered(FONT_H, title, false, true);
    ui_show_message_centered(message ? message : "");
    ui_commit();
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
}

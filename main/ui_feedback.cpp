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
    // 顺序要紧：ui_show_message_centered() 内部会 ui_clear()（它自己就是个"先清屏再画框"
    // 的对话框）。原来先画标题再调它 → 标题被那一下清屏抹掉，带标题的版本和普通版本
    // 长得一模一样。改成"先消息框、后标题"：两者不重叠（标题钉在顶部 FONT_H，消息框在
    // 垂直居中处），标题就留住了。ui_clear() 也不必再自己调。
    ui_show_message_centered(message ? message : "");
    if (title && title[0]) ui_draw_text_centered(FONT_H, title, false, true);
    ui_commit();
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
}

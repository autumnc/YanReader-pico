// esp_err 的主机端桩：main/net/http.h 只用到 esp_err_t / ESP_OK，这里给最小集，
// 让 weread_skill_client.cpp 能在主机上编过（网络路径本身不参与测例）。
// esp_err_to_name 只出现在失败分支的日志里，同样不参与测例，给了空实现。
#pragma once

typedef int esp_err_t;

#ifndef ESP_OK
#define ESP_OK 0
#endif

inline const char *esp_err_to_name(esp_err_t) { return "ESP_OK"; }

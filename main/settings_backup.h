#pragma once

#include <string>

#include "esp_err.h"

// 配置与阅读记录备份到 TF 卡（设计说明见 settings_backup.cpp 顶部）。
// 备份落在 <SD>/settings_backup/，保存与恢复都是"目录树复制"，不做序列化。
//
// 保存：参数无，成功返回 ESP_OK；卡没挂上 ESP_ERR_INVALID_STATE；写失败 ESP_FAIL。
// 恢复：备份不存在 ESP_ERR_NOT_FOUND；内容不可用 ESP_ERR_INVALID_RESPONSE。
//
// 恢复**只管文件**：设置的内存缓存（SettingsManager 的 s_cache）与各模块从设置派生的
// 状态不会跟着变，所以调用方恢复成功后应当重启（见 screen_settings.cpp 的菜单项）。
esp_err_t settings_backup_save();
esp_err_t settings_backup_restore();

// 卡上有没有一份可用的备份（菜单项显示用）。卡不在 / 目录不在都返回 false。
bool settings_backup_exists();

// 备份时间的那一行文本（"2026-10-07 14:03" / "时钟未同步"），给菜单显示用。
// 没有备份时返回空串。写进备份目录的 backup_info.txt，纯记录，不参与校验。
std::string settings_backup_stamp();

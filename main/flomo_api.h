#pragma once

// Flomo 客户端 API。移植自 ../Flomo/src/api.{h,cpp}（C1 Flomo 客户端），
// 逻辑逐字保留，只把签名用的 md5 换成本仓已经在用的 esp_rom_md5（见 flomo_client.cpp）。
// 一条笔记 = 一个 Memo：远端字段（slug/时间戳/tags）+ 本地缓存字段
// （contentText 便于编辑与搜索、dirty/pendingOp 是"待同步的本地改动"标记）。
//
// 认证沿用设置里的 flomo_email/flomo_pass：token 由 login() 换得后存回设置
// （g_settings.setFlomoToken），与「发送到 Flomo」共用同一份登录信息。

#include <map>
#include <string>
#include <vector>

#include "flomo_json.h"

struct Memo {
    std::string slug;
    std::string contentHtml;
    std::string contentText;
    std::vector<std::string> tags;
    std::string createdAt;
    std::string updatedAt;
    bool deleted = false;
    bool dirty = false;
    std::string pendingOp;   // "" | "create" | "update" | "delete"
};

struct ApiResult {
    bool ok = false;
    std::string message;
    JsonValue data;
};

class FlomoApi {
public:
    explicit FlomoApi(std::string token) : token_(std::move(token)) {}

    // 邮箱/密码登录，成功时 data.access_token 是令牌。
    static ApiResult login(const std::string &email, const std::string &password);

    // 分页拉取（latest_updated_desc）。首屏传空串，翻页时带上上一页最后一条的
    // slug/updatedAt（服务端用它当游标）。
    ApiResult listPage(const std::string &latestSlug, const std::string &latestUpdatedAt);
    ApiResult createMemo(const std::string &content);
    ApiResult updateMemo(const std::string &slug, const std::string &content);
    ApiResult deleteMemo(const std::string &slug);

private:
    std::string token_;
    ApiResult request(const std::string &method, const std::string &path,
                      const std::map<std::string, std::string> &params);
};

// 富文本 ↔ 纯文本互转（编辑器里是纯文本，Flomo 存的是那套 <p>/<ul>/<strong>… HTML）。
std::string htmlToText(const std::string &html);
std::string textToHtml(const std::string &text);
std::vector<std::string> extractTags(const std::string &text);

// 把接口返回的一条 memo 转成本地 Memo（已把 content 的 HTML 转成纯文本）。
Memo memoFromJson(const JsonValue &v);

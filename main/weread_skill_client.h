#pragma once

// 微信读书官方 Skill/Gateway 客户端（阶段二新增）。
//
// 与 components/crossmux 里那套 Web Reader 链路（cookie + `_e()` 签名）**互相独立**：
// 这里走的是官方 API Key，一次一个 POST，服务端 skip_cookie，不碰登录态。接口规格
// （端点、头布局、body 摊平方式）是协议事实，实现独立写，未抄任何上游代码。
//
//   POST https://i.weread.qq.com/api/agent/gateway
//   Authorization: Bearer wrk-…
//   Content-Type: application/json;charset=UTF-8
//   Origin: https://weread.qq.com    Referer: https://weread.qq.com/
//   body: {"api_name":…,"skill_version":"1.0.5", …params 摊平到顶层}
//
// 响应体就是数据本体（没有 code/msg 外壳）。API Key 只从设置键 `weread_api_key`
// 读，**不编译进固件、不提交**。

#include <string>
#include <vector>

#include "flomo_json.h"

// 一个摊平进 gateway body 的参数。`numeric` 决定加不加引号：count / maxIdx / scope
// 这些要发成 JSON number，keyword / bookId 要发成 JSON string。
struct WereadGwParam {
    std::string key;
    std::string value;
    bool numeric = false;
};

class WereadSkillClient {
public:
    struct SearchItem {
        std::string bookId;
        std::string title;
        std::string author;
        std::string category;
        std::string intro;
        float rating = 0;       // newRating / 10
        int readingCount = 0;
    };
    struct RecommendItem {
        std::string bookId;
        std::string title;
        std::string author;
        std::string category;
        std::string intro;
        std::string reason;
        float rating = 0;
        int readingCount = 0;
    };

    struct Result {
        bool success = false;
        bool no_key = false;        // 未配置 API Key（UI 引导去设置）
        bool unauthorized = false;  // 401/403：Key 无效或过期
        std::string error;          // 可直接上屏的中文
        std::vector<SearchItem> search;
        std::vector<RecommendItem> recommend;
    };

    // —— 网络入口（照 deepseek_client 的骨架，在主任务上同步跑）——
    Result search(const std::string &keyword, int count = 20);
    Result recommend(int count = 20);

    // —— 纯函数（给主机测）——
    // 拼 gateway 请求体。api_name 与 params 按插入顺序摊平。
    static std::string buildGatewayBody(const std::string &apiName,
                                        const std::vector<WereadGwParam> &params);
    static std::vector<SearchItem> parseSearch(const JsonValue &root);
    static std::vector<RecommendItem> parseRecommend(const JsonValue &root);

private:
    // 一次 gateway 调用：拼体、发请求、把响应 parse 成 JsonValue。
    // 返回 false 时 result.error 已填好可直接上屏的文案。
    bool gateway(const std::string &apiName, const std::vector<WereadGwParam> &params,
                 JsonValue &out, Result &result);
};

extern WereadSkillClient g_weread_skill;

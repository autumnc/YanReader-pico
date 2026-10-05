#include "deepseek_client.h"
#include "settings_manager.h"
#include "json_utils.h"
#include "net/http.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>

static const char *TAG = "Deepseek";
DeepseekClient g_deepseek;

#define DEEPSEEK_API_URL "https://api.deepseek.com/chat/completions"

// Extract the first "content":"..." string value from a JSON response,
// decoding the JSON escapes (incl. \n). DeepSeek returns UTF-8 directly.
static std::string extractContent(const std::string &response) {
    auto contentKey = response.find("\"content\":\"");
    if (contentKey == std::string::npos) return "";
    contentKey += 11; // skip past "content":"
    std::string content;
    bool escaped = false;
    for (auto i = contentKey; i < response.size(); i++) {
        char c = response[i];
        if (escaped) {
            switch (c) {
                case '"': content += '"'; break;
                case '\\': content += '\\'; break;
                case '/': content += '/'; break;
                case 'b': content += '\b'; break;
                case 'f': content += '\f'; break;
                case 'n': content += '\n'; break;
                case 'r': content += '\r'; break;
                case 't': content += '\t'; break;
                case 'u': {
                    // \uXXXX — skip the digits; DeepSeek doesn't emit these for
                    // UTF-8 responses, so a placeholder is a safe fallback.
                    if (i + 4 < response.size()) {
                        i += 4;
                        content += '?';
                    }
                    break;
                }
                default:
                    content += c; // Unknown escape, keep as-is
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            break;
        } else {
            content += c;
        }
    }
    return content;
}

// POST the already-built request body to the DeepSeek chat API and return the
// assistant content. Shared by generatePrompt and polishText.
static DeepseekResult runChat(const std::string &body, volatile bool *cancel = nullptr) {
    net::Request req;
    req.url = DEEPSEEK_API_URL;
    req.method = net::Method::Post;
    req.headers = {{"Content-Type", "application/json"},
                   {"User-Agent", "pjournal-esp32/1.0"},
                   {"Authorization", "Bearer " + g_settings.deepseekKey()}};
    req.body = body;
    req.timeout_ms = 30000;
    req.deadline_ms = 30000;  // 整个请求的墙钟上限（对付"每 29 秒挤一个字节"的对端）
    req.cap = 32768;          // 软上限：响应最多收这么多
    req.cancel = cancel;      // 非空时按 1s 粒度轮询

    ESP_LOGI(TAG, "Request body: %d bytes", (int)body.size());
    net::Response resp = net::request(req);
    DeepseekResult result = {false, "API请求失败"};

    if (resp.cancelled) {
        result.content = "已取消";
    } else if (resp.timed_out) {
        result.content = "API响应超时";
    } else if (resp.ok) {
        std::string content = extractContent(resp.body);
        if (!content.empty()) {
            result.success = true;
            result.content = content;
        }
    } else if (resp.status != 0) {
        ESP_LOGW(TAG, "API returned status %d, response: %s", resp.status, resp.body.c_str());
        result.content = "API返回错误";
    } else {
        ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(resp.err));
        result.content = "网络请求失败";
    }
    return result;
}

DeepseekResult DeepseekClient::generatePrompt(const std::string &userContext) {
    std::string apiKey = g_settings.deepseekKey();
    if (apiKey.empty()) {
        return {false, "请先在设置中配置Deepseek Key"};
    }

    // Build request body with proper JSON escaping
    std::string escapedContext = json_escape(userContext);
    char body[1024];
    int n = snprintf(body, sizeof(body),
        "{\"model\":\"deepseek-chat\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是一个日记写作助手。根据用户的背景和爱好，"
        "运用这些爱好领域内的专业知识、概念、理论和思维方式，生成一个富有洞见和启发性的"
        "日记写作提示（不超过56字），引导用户用该领域的视角观察和记录今天的生活。\"},"
        "{\"role\":\"user\",\"content\":\"我的背景：%s。请生成一个写作提示。\"}],"
        "\"max_tokens\":100,\"temperature\":0.9}",
        escapedContext.c_str());
    if (n >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "Request body truncated (%d >= %d), userContext too long", n, (int)sizeof(body));
        return {false, "背景信息过长"};
    }
    return runChat(body);
}

DeepseekResult DeepseekClient::polishText(const std::string &text, const std::string &customInstr,
                                          volatile bool *cancel) {
    std::string apiKey = g_settings.deepseekKey();
    if (apiKey.empty()) {
        return {false, "请先在设置中配置Deepseek Key"};
    }
    if (text.size() > 8192) {
        return {false, "文本过长,请分段润色"};
    }

    static const char *DEFAULT_SYSTEM =
        "你是一个中文文本润色助手。请对用户提供的文本做轻度润色：主要修正语义不通顺、"
        "断句不合理之处；尽量避免大幅改动，保留原文的行文风格和叙事内容，不增删事实信息；"
        "去除明显口语化的表达，使文句通顺自然；保持原文段落结构。只输出润色后的文本，"
        "不要任何解释、称呼或前后缀。";
    // 用户可在设置里自定义润色提示词;未设置时用内置默认原则。
    std::string system = g_settings.polishPrompt();
    if (system.empty()) system = DEFAULT_SYSTEM;
    if (!customInstr.empty()) {
        system += "。另外，用户额外要求：";
        system += customInstr;
    }

    std::string escSys = json_escape(system);
    std::string escText = json_escape(text);
    std::string body = "{\"model\":\"deepseek-chat\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + escSys + "\"},"
        "{\"role\":\"user\",\"content\":\"" + escText + "\"}],"
        "\"max_tokens\":2000,\"temperature\":0.3}";
    return runChat(body, cancel);
}

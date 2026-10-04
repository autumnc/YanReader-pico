#include "deepseek_client.h"
#include "settings_manager.h"
#include "json_utils.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>

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
    esp_http_client_config_t cfg = {};
    cfg.url = DEEPSEEK_API_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return {false, "HTTP客户端初始化失败"};

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");
    std::string auth = "Bearer " + g_settings.deepseekKey();
    esp_http_client_set_header(client, "Authorization", auth.c_str());

    std::string response;
    DeepseekResult result = {false, "API请求失败"};
    bool cancelled = (cancel && *cancel);
    bool timedOut = false;

    esp_err_t err = cancelled ? ESP_ERR_INVALID_STATE
                              : esp_http_client_open(client, (int)body.size());
    ESP_LOGI(TAG, "Request body: %d bytes", (int)body.size());
    if (err == ESP_OK) {
        esp_http_client_write(client, body.c_str(), (int)body.size());
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[512];
            // Cancellable read: shorten the per-read timeout so the loop can
            // poll the flag every second; the 30s overall deadline still bounds
            // a hung server. Reads return -ESP_ERR_HTTP_EAGAIN on a per-read
            // timeout, 0/negative on end-of-body or error.
            if (cancel) esp_http_client_set_timeout_ms(client, 1000);
            int64_t deadline = esp_timer_get_time() + 30000000;
            while (true) {
                if (cancel && *cancel) { cancelled = true; break; }
                if (esp_timer_get_time() > deadline) { timedOut = true; break; }
                int len = esp_http_client_read(client, buf, sizeof(buf) - 1);
                if (len > 0) {
                    buf[len] = 0;
                    response += buf;
                    if (response.size() > 32768) break;
                } else if (len == -ESP_ERR_HTTP_EAGAIN) {
                    continue;   // no data yet, retry
                } else {
                    break;      // 0 = body complete; <0 = transport error
                }
            }
            if (!timedOut) {
                std::string content = extractContent(response);
                if (!content.empty()) {
                    result.success = true;
                    result.content = content;
                }
            } else {
                result.content = "API响应超时";
            }
        } else {
            // Read error response body for debugging
            char buf[512];
            int len;
            std::string errorResponse;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                errorResponse += buf;
                if (errorResponse.size() > 1024) break;
            }
            ESP_LOGW(TAG, "API returned status %d, response: %s", status, errorResponse.c_str());
            result.content = "API返回错误";
        }
    } else {
        ESP_LOGW(TAG, "HTTP request failed: %d", err);
        result.content = "网络请求失败";
    }

    esp_http_client_cleanup(client);
    if (cancelled) result.content = "已取消";
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

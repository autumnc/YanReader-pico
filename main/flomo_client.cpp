#include "flomo_client.h"
#include "settings_manager.h"
#include "json_utils.h"
#include <cstring>
#include <cstdio>
#include <ctime>
#include <vector>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_rom_md5.h"

static const char *TAG = "Flomo";
FlomoClient g_flomo;
static const size_t MAX_FLOMO_RESPONSE_SIZE = 32 * 1024;
// 登录被限流后重试前要等多久：服务端令牌桶约每 11 秒回一个令牌，等 12s 刚够跨过窗口。
static const int kLoginRetryWaitMs = 12000;

static std::string htmlEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// Apply inline formatting: **bold**, __underline__, ==highlight==
static std::string applyInlineFormats(const std::string &text) {
    std::string result = text;

    // **bold** -> <strong>bold</strong>
    size_t pos = 0;
    while ((pos = result.find("**", pos)) != std::string::npos) {
        size_t end = result.find("**", pos + 2);
        if (end != std::string::npos) {
            std::string bold = result.substr(pos + 2, end - pos - 2);
            result.replace(pos, end - pos + 2, "<strong>" + bold + "</strong>");
            pos += 8 + bold.length(); // skip past </strong>
        } else {
            break;
        }
    }

    // __underline__ -> <u>underline</u>
    pos = 0;
    while ((pos = result.find("__", pos)) != std::string::npos) {
        size_t end = result.find("__", pos + 2);
        if (end != std::string::npos) {
            std::string underline = result.substr(pos + 2, end - pos - 2);
            result.replace(pos, end - pos + 2, "<u>" + underline + "</u>");
            pos += 7 + underline.length(); // skip past </u>
        } else {
            break;
        }
    }

    // ==highlight== -> <mark>highlight</mark>
    pos = 0;
    while ((pos = result.find("==", pos)) != std::string::npos) {
        size_t end = result.find("==", pos + 2);
        if (end != std::string::npos) {
            std::string highlight = result.substr(pos + 2, end - pos - 2);
            result.replace(pos, end - pos + 2, "<mark>" + highlight + "</mark>");
            pos += 12 + highlight.length(); // skip past </mark>
        } else {
            break;
        }
    }

    return result;
}

// Convert plain text to HTML format for Flomo
// Supports: **bold**, __underline__, ==highlight==, - list items
static std::string textToHtml(const std::string &text) {
    if (text.empty()) return "";

    std::string result;
    std::vector<std::string> lines;

    // Split text into lines
    size_t start = 0;
    for (size_t i = 0; i <= text.length(); i++) {
        if (i == text.length() || text[i] == '\n') {
            if (i > start) {
                lines.push_back(text.substr(start, i - start));
            } else {
                lines.push_back("");
            }
            start = i + 1;
        }
    }

    bool inList = false;
    for (const auto &line : lines) {
        // Trim leading/trailing spaces for processing
        size_t trimStart = line.find_first_not_of(" \t");
        size_t trimEnd = line.find_last_not_of(" \t");
        std::string trimmed = (trimStart == std::string::npos) ? "" :
            line.substr(trimStart, trimEnd == std::string::npos ? std::string::npos : trimEnd - trimStart + 1);

        // Check for list items (- or * followed by space)
        bool isListItem = (trimmed.length() >= 2 && trimmed[0] == '-' && trimmed[1] == ' ') ||
                         (trimmed.length() >= 2 && trimmed[0] == '*' && trimmed[1] == ' ');

        if (isListItem) {
            if (!inList) {
                result += "<ul>";
                inList = true;
            }
            std::string content = htmlEscape(trimmed.substr(2));
            content = applyInlineFormats(content);
            result += "<li>" + content + "</li>";
        } else {
            if (inList) {
                result += "</ul>";
                inList = false;
            }
            if (trimmed.empty()) {
                result += "<p><br></p>";
            } else {
                std::string content = applyInlineFormats(htmlEscape(trimmed));
                result += "<p>" + content + "</p>";
            }
        }
    }

    if (inList) {
        result += "</ul>";
    }

    return result;
}

// Flomo constants
#define FLOMO_API_BASE    "https://flomoapp.com/api/v1"
#define FLOMO_API_KEY     "flomo_web"
#define FLOMO_APP_VERSION "4.1"
#define FLOMO_PLATFORM    "web"
#define FLOMO_SIGN_SECRET "dbbc3dd73364b4084c3a69346e0ce2b2"

FlomoClient::FlomoClient() {}

void FlomoClient::configure(const std::string &email, const std::string &password) {
    email_ = email;
    password_ = password;
}

bool FlomoClient::isConfigured() const {
    return !email_.empty() && !password_.empty();
}

std::string FlomoClient::generateSign(const std::string &sortedParams) {
    std::string raw = sortedParams + FLOMO_SIGN_SECRET;
    unsigned char md5[16];
    md5_context_t md5_ctx;
    esp_rom_md5_init(&md5_ctx);
    esp_rom_md5_update(&md5_ctx, raw.data(), raw.size());
    esp_rom_md5_final(md5, &md5_ctx);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", md5[i]);
    return std::string(hex, 32);
}

// 从响应里抠出 access_token（老式手写扫描，响应格式 {"code":0,"data":{"access_token":".."}}）。
static std::string extractToken(const std::string &response) {
    auto grab = [&](size_t from) -> std::string {
        auto tokPos = response.find("\"access_token\"", from);
        if (tokPos == std::string::npos) return "";
        auto valStart = response.find('"', tokPos + 14);
        if (valStart == std::string::npos) return "";
        valStart++;
        auto valEnd = response.find('"', valStart);
        if (valEnd == std::string::npos) return "";
        return response.substr(valStart, valEnd - valStart);
    };
    auto dataPos = response.find("\"data\"");
    if (dataPos != std::string::npos) {
        std::string t = grab(dataPos);
        if (!t.empty()) return t;
    }
    return grab(0);  // 老接口把 access_token 放顶层
}

// 服务端对 user/login_by_email 有很强的限流（实测：连发约 2 次后开始回 429，body 是一句
// 网关谜语 "gogogo"，令牌大约每 11 秒才回一个）。原实现撞上就只打一条日志、返回空 token，
// 界面于是统一说"Flomo登录失败"，用户看到的是"只点了一次却说请求过于频繁" —— 分不清是
// 限流还是密码错。这里改成：429 等过窗口（12s）再试一次；失败原因存进 lastError_ 交给界面。
std::string FlomoClient::login() {
    lastError_.clear();
    if (!isConfigured()) {
        lastError_ = "未配置邮箱或密码";
        return "";
    }

    std::string token;
    for (int attempt = 0; attempt < 2; attempt++) {
        time_t now;
        time(&now);
        char ts[16];
        snprintf(ts, sizeof(ts), "%lld", (long long)now);

        // Build params for signing
        std::string params = "api_key=" + std::string(FLOMO_API_KEY)
            + "&app_version=" + std::string(FLOMO_APP_VERSION)
            + "&email=" + email_
            + "&password=" + password_
            + "&platform=" + std::string(FLOMO_PLATFORM)
            + "&timestamp=" + ts
            + "&webp=1";
        std::string sign = generateSign(params);

        // Build JSON body
        std::string body = "{\"email\":\"" + json_escape(email_) +
            "\",\"password\":\"" + json_escape(password_) +
            "\",\"wechat_union_id\":\"\",\"wechat_oa_open_id\":\"\",\"timestamp\":\"" + ts +
            "\",\"api_key\":\"" + FLOMO_API_KEY +
            "\",\"app_version\":\"" + FLOMO_APP_VERSION +
            "\",\"platform\":\"" + FLOMO_PLATFORM +
            "\",\"webp\":\"1\",\"sign\":\"" + sign + "\"}";

        esp_http_client_config_t cfg = {};
        cfg.url = FLOMO_API_BASE "/user/login_by_email";
        cfg.method = HTTP_METHOD_POST;
        cfg.timeout_ms = 30000;
        cfg.skip_cert_common_name_check = true;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            lastError_ = "系统繁忙";
            return "";
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");

        std::string response;
        int status = 0;
        esp_err_t err = esp_http_client_open(client, (int)body.size());
        if (err == ESP_OK) {
            esp_http_client_write(client, body.c_str(), (int)body.size());
            esp_http_client_fetch_headers(client);
            status = esp_http_client_get_status_code(client);
            char buf[512];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                response.append(buf, len);
                if (response.size() > MAX_FLOMO_RESPONSE_SIZE) break;
            }
        } else {
            ESP_LOGW(TAG, "Login open failed: %d", err);
            lastError_ = "网络连接失败";
        }
        esp_http_client_cleanup(client);

        if (status == 200) {
            token = extractToken(response);
            if (!token.empty()) {
                lastError_.clear();
                return token;
            }
            // 200 但没有令牌：多数是"账号或密码错误"（code=-1），偶发是风控塞回来的 HTML 页。
            auto msgPos = response.find("\"message\":\"");
            if (msgPos != std::string::npos) {
                msgPos += 11;
                auto endPos = response.find('"', msgPos);
                if (endPos != std::string::npos) lastError_ = response.substr(msgPos, endPos - msgPos);
            }
            if (lastError_.empty()) lastError_ = "服务端返回异常（可能被风控拦截）";
            ESP_LOGW(TAG, "Login 200 but no token: %.*s", (int)response.size(), response.c_str());
            break;
        }

        if (status == 429) {
            ESP_LOGW(TAG, "Login 429 (第%d次): %.*s", attempt + 1, (int)response.size(), response.c_str());
            lastError_ = "请求过于频繁，请稍后重试";
            if (attempt == 0) {
                vTaskDelay(pdMS_TO_TICKS(kLoginRetryWaitMs));
                continue;
            }
            break;
        }

        if (status != 0) lastError_ = "登录失败(HTTP " + std::to_string(status) + ")";
        if (!response.empty())
            ESP_LOGW(TAG, "Login HTTP %d: %.*s", status, (int)response.size(), response.c_str());
        break;
    }
    return token;
}

bool FlomoClient::createMemo(const std::string &token, const std::string &content) {
    if (token.empty()) return false;

    time_t now;
    time(&now);
    char ts[16];
    snprintf(ts, sizeof(ts), "%lld", (long long)now);

    std::string params = "api_key=" + std::string(FLOMO_API_KEY)
        + "&app_version=" + std::string(FLOMO_APP_VERSION)
        + "&content=" + content
        + "&platform=" + std::string(FLOMO_PLATFORM)
        + "&source=web"
        + "&timestamp=" + ts
        + "&tz=8:0"
        + "&webp=1";
    std::string sign = generateSign(params);

    char body[2048];
    std::string escapedContent = json_escape(content);
    int needed = snprintf(nullptr, 0,
        "{\"timestamp\":\"%s\",\"api_key\":\"%s\",\"app_version\":\"%s\","
        "\"platform\":\"%s\",\"webp\":\"1\",\"content\":\"%s\","
        "\"source\":\"web\",\"tz\":\"8:0\",\"sign\":\"%s\"}",
        ts, FLOMO_API_KEY, FLOMO_APP_VERSION, FLOMO_PLATFORM,
        escapedContent.c_str(), sign.c_str());
    if (needed >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "Content too long for Flomo (need %d bytes, have %d)", needed, (int)sizeof(body));
        return false;
    }
    snprintf(body, sizeof(body),
        "{\"timestamp\":\"%s\",\"api_key\":\"%s\",\"app_version\":\"%s\","
        "\"platform\":\"%s\",\"webp\":\"1\",\"content\":\"%s\","
        "\"source\":\"web\",\"tz\":\"8:0\",\"sign\":\"%s\"}",
        ts, FLOMO_API_KEY, FLOMO_APP_VERSION, FLOMO_PLATFORM,
        escapedContent.c_str(), sign.c_str());

    esp_http_client_config_t cfg = {};
    cfg.url = FLOMO_API_BASE "/memo";
    cfg.method = HTTP_METHOD_PUT;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");
    std::string bearer = "Bearer " + token;
    esp_http_client_set_header(client, "Authorization", bearer.c_str());

    bool ok = false;
    std::string response;
    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    if (err == ESP_OK) {
        esp_http_client_write(client, body, (int)strlen(body));
        int content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[256];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                response += buf;
                if (response.size() > MAX_FLOMO_RESPONSE_SIZE) break;
            }
            // Parse code field from JSON response
            auto codePos = response.find("\"code\":0");
            if (codePos != std::string::npos) {
                ok = true;
            } else {
                ESP_LOGW(TAG, "Memo 200 but code not 0: %.*s", (int)response.size(), response.c_str());
            }
        } else {
            ESP_LOGW(TAG, "Memo returned HTTP %d (content-length: %d)", status, content_length);
            char buf[256];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                response += buf;
                if (response.size() > MAX_FLOMO_RESPONSE_SIZE) break;
            }
            if (!response.empty())
                ESP_LOGW(TAG, "Memo response: %.*s", (int)response.size(), response.c_str());
        }
    } else {
        ESP_LOGW(TAG, "Memo open failed: %d", err);
    }

    esp_http_client_cleanup(client);
    return ok;
}

FlomoResult FlomoClient::send(const std::string &text) {
    if (!isConfigured()) {
        std::string email = g_settings.flomoEmail();
        std::string pass = g_settings.flomoPassword();
        if (!email.empty() && !pass.empty()) configure(email, pass);
    }
    if (!isConfigured()) {
        return {false, "请先在设置中配置Flomo账号"};
    }

    // Split text into chunks of <= 5000 chars (Flomo limit)
    const size_t MAX_CHUNK = 5000;
    std::vector<std::string> chunks;
    if (text.length() <= MAX_CHUNK) {
        chunks.push_back(text);
    } else {
        for (size_t i = 0; i < text.length(); i += MAX_CHUNK) {
            chunks.push_back(text.substr(i, MAX_CHUNK));
        }
    }

    int success = 0;
    int failed = 0;

    // Try cached token first
    std::string token = getCachedToken();
    if (!token.empty()) {
        for (auto &chunk : chunks) {
            std::string htmlContent = textToHtml(chunk);
            htmlContent += "\n\n<p>#日记</p>";
            if (createMemo(token, htmlContent)) {
                success++;
            } else {
                // Cached token failed, try re-login
                token = "";
                break;
            }
        }
        if (success == (int)chunks.size()) {
            return {true, chunks.size() == 1 ? "已发送到Flomo ✓" : ("已发送" + std::to_string(success) + "条到Flomo ✓")};
        }
    }

    // Re-login if needed
    if (token.empty()) {
        token = login();
        if (!token.empty()) {
            setCachedToken(token);
            for (auto &chunk : chunks) {
                std::string htmlContent = textToHtml(chunk);
                htmlContent += "\n\n<p>#日记</p>";
                if (createMemo(token, htmlContent)) {
                    success++;
                } else {
                    failed++;
                }
            }
        } else {
            setCachedToken("");
            std::string why = lastError();
            if (why.empty() || why == "未配置邮箱或密码") why = "请检查账号密码";
            return {false, "Flomo登录失败: " + why};
        }
    }

    if (failed == 0 && success > 0) {
        return {true, chunks.size() == 1 ? "已发送到Flomo ✓" : ("已发送" + std::to_string(success) + "条到Flomo ✓")};
    } else if (success > 0) {
        return {false, "部分发送成功（" + std::to_string(success) + "/" + std::to_string(chunks.size()) + "）"};
    }
    return {false, "发送到Flomo失败"};
}

std::string FlomoClient::getCachedToken() {
    return g_settings.flomoToken();
}

void FlomoClient::setCachedToken(const std::string &token) {
    g_settings.setFlomoToken(token);
    ESP_LOGI(TAG, "Token saved to settings");
}

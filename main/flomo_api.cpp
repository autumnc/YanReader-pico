#include "flomo_api.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_rom_md5.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "FlomoApi";

static constexpr const char *kBase = "https://flomoapp.com/api/v1";
static constexpr const char *kSecret = "dbbc3dd73364b4084c3a69346e0ce2b2";

static std::string nowString() {
    return std::to_string((long long)std::time(nullptr));
}

static std::map<std::string, std::string> baseParams() {
    return {{"timestamp", nowString()}, {"api_key", "flomo_web"}, {"app_version", "4.1"},
            {"platform", "web"}, {"webp", "1"}};
}

static std::string md5hex(const std::string &s) {
    unsigned char out[16];
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, s.data(), s.size());
    esp_rom_md5_final(out, &ctx);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", out[i]);
    return std::string(hex, 32);
}

// 签名 = md5(按 key 排序、跳过空值的 "k=v" 用 & 连接 + 密钥)。std::map 天然按 key
// 排序，所以这里直接顺序拼即可（与 Flomo 原实现一致）。
static std::string sign(const std::map<std::string, std::string> &p) {
    std::string raw;
    bool first = true;
    for (auto &kv : p) {
        if (kv.second.empty()) continue;
        if (!first) raw += '&';
        first = false;
        raw += kv.first + "=" + kv.second;
    }
    return md5hex(raw + kSecret);
}

static std::string urlenc(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static std::string jsonEscape(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

static std::string query(std::map<std::string, std::string> p) {
    std::string q;
    bool first = true;
    for (auto &kv : p) {
        if (!first) q += '&';
        first = false;
        q += urlenc(kv.first) + "=" + urlenc(kv.second);
    }
    return q;
}

static ApiResult parseResponse(int status, const std::string &body) {
    auto snippet = [](const std::string &s) {
        std::string out = s.substr(0, 80);
        for (char &c : out) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        return out;
    };
    if (status < 200 || status >= 300) return {false, "HTTP " + std::to_string(status) + " " + snippet(body), {}};
    if (body.empty()) return {true, "", {}};
    JsonValue root = JsonValue::parse(body);
    int code = -1;
    if (root["code"].isNumber()) code = root["code"].asInt(-1);
    else if (root["code"].isString()) code = std::atoi(root["code"].asString().c_str());
    if (code == 0) return {true, "", root["data"]};
    std::string msg = root["message"].asString(root["msg"].asString());
    if (msg.empty()) msg = "API code " + std::to_string(code) + " " + snippet(body);
    return {false, msg, root};
}

// ── 服务端限流（HTTP 429）防护 ────────────────────────────────────────────
// 拉取分页是"一页接一页"连发的，一次同步几十个请求打出去最容易撞限流；撞上之后
// 服务端只回一句 API 网关的谜语（实测是 "gogogo"），直接摆给用户看等于没提示。
// 这里做三件事：① 两次请求之间留最小间隔；② 429 退避 3 秒重试一次；③ 连续 429 后
// 进 60 秒冷却，冷却期内的请求直接本地拒绝，不再白打网络（用户连点同步时尤其重要）。
static int64_t s_last_req_us = 0;
static int64_t s_cooldown_until_us = 0;
static const int64_t MIN_REQ_GAP_US = 800 * 1000;
// 限流窗口实测：user/login_by_email 连发约 2 次后开始回 429（body 是网关谜语
// "gogogo"），令牌约每 11 秒才回一个；其它接口（memo/…）连发 8 次都没事。所以重试
// 必须等过窗口才有意义 —— 原来等 3 秒再试是一定再撞 429，白费一个令牌。
static const int64_t RETRY_WAIT_MS = 12000;
// 本地冷却也跟着窗口走：原来是 60 秒，用户连点两下就被本地拒 1 分钟，还以为是"只点
// 了一次却说频繁"。现在 15 秒，够服务端回一个令牌。
static const int64_t COOLDOWN_US = 15LL * 1000 * 1000;

ApiResult FlomoApi::request(const std::string &method, const std::string &path,
                            const std::map<std::string, std::string> &extra) {
    if (s_cooldown_until_us > 0 && esp_timer_get_time() < s_cooldown_until_us) {
        const int secs = static_cast<int>((s_cooldown_until_us - esp_timer_get_time()) / 1000000) + 1;
        return {false, "请求过于频繁，请等 " + std::to_string(secs) + " 秒再试", {}};
    }

    auto params = baseParams();
    for (auto &kv : extra) params[kv.first] = kv.second;
    params["sign"] = sign(params);

    std::string url = std::string(kBase) + "/" + path;
    std::string body;
    esp_http_client_method_t m = HTTP_METHOD_GET;
    if (method == "GET" || method == "DELETE") {
        url += "?" + query(params);
        m = method == "GET" ? HTTP_METHOD_GET : HTTP_METHOD_DELETE;
    } else {
        m = method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_PUT;
        body = "{";
        bool first = true;
        for (auto &kv : params) {
            if (!first) body += ",";
            first = false;
            body += "\"" + jsonEscape(kv.first) + "\":\"" + jsonEscape(kv.second) + "\"";
        }
        body += "}";
    }

    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.method = m;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    ApiResult result{false, "网络连接失败", {}};
    for (int attempt = 0; attempt < 2; attempt++) {
        // 最小请求间隔（第 0 次也要等，把"连点同步"的密集请求摊开）
        if (s_last_req_us > 0) {
            int64_t gap = esp_timer_get_time() - s_last_req_us;
            if (gap >= 0 && gap < MIN_REQ_GAP_US)
                vTaskDelay(pdMS_TO_TICKS((MIN_REQ_GAP_US - gap) / 1000 + 1));
        }

        auto *c = esp_http_client_init(&cfg);
        if (!c) return {false, "HTTP init failed", {}};
        esp_http_client_set_header(c, "User-Agent", "pjournal-pico/1.0");
        if (!token_.empty()) {
            std::string bearer = "Bearer " + token_;
            esp_http_client_set_header(c, "Authorization", bearer.c_str());
        }
        if (!body.empty()) esp_http_client_set_header(c, "Content-Type", "application/json");

        int status = 0;
        std::string resp;
        if (esp_http_client_open(c, body.size()) == 0) {
            if (!body.empty()) esp_http_client_write(c, body.c_str(), body.size());
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            char buf[1024];
            int n = 0;
            while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0 && resp.size() < 1024 * 1024)
                resp.append(buf, n);
            result = parseResponse(status, resp);
        } else {
            result = {false, "网络连接失败", {}};
        }
        esp_http_client_cleanup(c);
        s_last_req_us = esp_timer_get_time();   // 成功失败都算一次"打过网络"

        if (status != 429) break;
        ESP_LOGW(TAG, "%s 限流(429) 第%d次: %s", path.c_str(), attempt + 1, resp.substr(0, 120).c_str());
        if (attempt == 0) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_WAIT_MS));
            continue;
        }
        s_cooldown_until_us = esp_timer_get_time() + COOLDOWN_US;
        result.message = "flomo 限流中，请等 15 秒再试";
    }
    return result;
}

ApiResult FlomoApi::login(const std::string &email, const std::string &password) {
    FlomoApi api("");
    return api.request("POST", "user/login_by_email",
                       {{"email", email}, {"password", password},
                        {"wechat_union_id", ""}, {"wechat_oa_open_id", ""}});
}

ApiResult FlomoApi::listPage(const std::string &slug, const std::string &updated) {
    std::map<std::string, std::string> p{{"limit", "200"}};
    if (!slug.empty()) p["latest_slug"] = slug;
    if (!updated.empty()) p["latest_updated_at"] = updated;
    return request("GET", "memo/latest_updated_desc", p);
}

ApiResult FlomoApi::createMemo(const std::string &content) {
    return request("PUT", "memo", {{"content", textToHtml(content)}, {"source", "web"}, {"tz", "8:0"}});
}

ApiResult FlomoApi::updateMemo(const std::string &slug, const std::string &content) {
    return request("PUT", "memo/" + slug, {{"content", textToHtml(content)}, {"source", "web"}, {"tz", "8:0"}});
}

ApiResult FlomoApi::deleteMemo(const std::string &slug) {
    return request("DELETE", "memo/" + slug, {});
}

// ── 纯文本 ↔ HTML ──────────────────────────────────────────────────────────
// 规则与 Flomo 原实现逐字一致：空行是 <p><br></p>；"- " 起头是 <ul>，"1. "/"1、"
// 起头是 <ol>；**粗**/__下划线__/==高亮== 转成 <strong>/<u>/<mark>。反过来
// htmlToText 把这些标记还原，这样"编辑 → 回写"能来回不掉格式。

static std::string htmlEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c; break;
        }
    }
    return out;
}

static std::string applyInlineFormats(std::string s) {
    static const struct { const char *marker; const char *open; const char *close; } kFormats[] = {
        {"**", "<strong>", "</strong>"},
        {"__", "<u>", "</u>"},
        {"==", "<mark>", "</mark>"},
    };
    for (const auto &f : kFormats) {
        size_t mlen = std::strlen(f.marker);
        size_t pos = 0;
        while ((pos = s.find(f.marker, pos)) != std::string::npos) {
            size_t end = s.find(f.marker, pos + mlen);
            if (end == std::string::npos) break;
            std::string inner = s.substr(pos + mlen, end - pos - mlen);
            s.replace(pos, end - pos + mlen, std::string(f.open) + inner + f.close);
            pos += std::strlen(f.open) + inner.size();
        }
    }
    return s;
}

std::string htmlToText(const std::string &html) {
    std::string out;
    bool tag = false;
    int listKind = 0; // 1 <ul>, 2 <ol>
    int orderCounter = 0;
    for (size_t i = 0; i < html.size(); ++i) {
        // 空段落是"空行"，不是两个换行。
        if (html.compare(i, 13, "<p><br /></p>") == 0) { out += '\n'; i += 12; continue; }
        if (html.compare(i, 12, "<p><br/></p>") == 0) { out += '\n'; i += 11; continue; }
        if (html.compare(i, 11, "<p><br></p>") == 0) { out += '\n'; i += 10; continue; }
        // 把 textToHtml 吃掉的行内标记还原，保证编辑往返一致。
        if (html.compare(i, 8, "<strong>") == 0) { out += "**"; i += 7; continue; }
        if (html.compare(i, 9, "</strong>") == 0) { out += "**"; i += 8; continue; }
        if (html.compare(i, 3, "<u>") == 0) { out += "__"; i += 2; continue; }
        if (html.compare(i, 4, "</u>") == 0) { out += "__"; i += 3; continue; }
        if (html.compare(i, 6, "<mark>") == 0) { out += "=="; i += 5; continue; }
        if (html.compare(i, 7, "</mark>") == 0) { out += "=="; i += 6; continue; }
        if (html.compare(i, 4, "<ul>") == 0) { listKind = 1; i += 3; continue; }
        if (html.compare(i, 5, "</ul>") == 0) { listKind = 0; i += 4; continue; }
        if (html.compare(i, 4, "<ol>") == 0) { listKind = 2; orderCounter = 0; i += 3; continue; }
        if (html.compare(i, 5, "</ol>") == 0) { listKind = 0; i += 4; continue; }
        if (html.compare(i, 4, "<li>") == 0) {
            out += listKind == 2 ? std::to_string(++orderCounter) + ". " : "- ";
            i += 3;
            continue;
        }
        if (html.compare(i, 5, "</li>") == 0) { out += '\n'; i += 4; continue; }
        if (html.compare(i, 4, "<br>") == 0) { out += '\n'; i += 3; continue; }
        if (html.compare(i, 5, "<br/>") == 0) { out += '\n'; i += 4; continue; }
        if (html.compare(i, 6, "<br />") == 0) { out += '\n'; i += 5; continue; }
        if (html.compare(i, 4, "</p>") == 0) { out += '\n'; i += 3; continue; }
        if (html[i] == '<') { tag = true; continue; }
        if (html[i] == '>') { tag = false; continue; }
        if (!tag) out += html[i];
    }
    static const struct { const char *entity; const char *plain; } kEntities[] = {
        {"&nbsp;", " "}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"}, {"&amp;", "&"},
    };
    for (const auto &e : kEntities) {
        size_t pos = 0;
        size_t elen = std::strlen(e.entity);
        while ((pos = out.find(e.entity, pos)) != std::string::npos) {
            out.replace(pos, elen, e.plain);
            pos += std::strlen(e.plain);
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// 有序列表标记："数字 + '. ' 或 '、'"。返回内容起始偏移；不是有序项就返回 false。
static bool orderedMarkerEnd(const std::string &s, size_t &content) {
    size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == 0 || i >= s.size()) return false;
    if (s[i] == '.') {
        if (i + 1 >= s.size() || s[i + 1] != ' ') return false;
        content = i + 2;
        return true;
    }
    // U+3001 '、' (UTF-8 E3 80 81)，后面可跟一个空格。
    if (i + 2 < s.size() &&
        (unsigned char)s[i] == 0xE3 && (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x81) {
        content = i + 3;
        if (content < s.size() && s[content] == ' ') ++content;
        return true;
    }
    return false;
}

std::string textToHtml(const std::string &text) {
    if (text.empty()) return "<p><br></p>";

    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            std::string line = text.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            start = i + 1;
        }
    }

    std::string out;
    int listKind = 0; // 0 无, 1 <ul>, 2 <ol>
    for (const std::string &line : lines) {
        size_t from = line.find_first_not_of(" \t");
        size_t to = line.find_last_not_of(" \t");
        std::string trimmed = from == std::string::npos ? "" : line.substr(from, to - from + 1);
        int kind = 0;
        size_t content = 0;
        if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*') && trimmed[1] == ' ') {
            kind = 1;
            content = 2;
        } else if (orderedMarkerEnd(trimmed, content)) {
            kind = 2;
        }
        if (kind != 0) {
            if (listKind != kind) {
                out += listKind == 1 ? "</ul>" : (listKind == 2 ? "</ol>" : "");
                out += kind == 1 ? "<ul>" : "<ol>";
                listKind = kind;
            }
            out += "<li>" + applyInlineFormats(htmlEscape(trimmed.substr(content))) + "</li>";
        } else {
            out += listKind == 1 ? "</ul>" : (listKind == 2 ? "</ol>" : "");
            listKind = 0;
            if (trimmed.empty()) out += "<p><br></p>";
            else out += "<p>" + applyInlineFormats(htmlEscape(trimmed)) + "</p>";
        }
    }
    out += listKind == 1 ? "</ul>" : (listKind == 2 ? "</ol>" : "");
    return out;
}

std::vector<std::string> extractTags(const std::string &text) {
    std::vector<std::string> tags;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '#') continue;
        size_t j = i + 1;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n' && text[j] != '#') ++j;
        if (j > i + 1) tags.push_back(text.substr(i + 1, j - i - 1));
        i = j;
    }
    return tags;
}

Memo memoFromJson(const JsonValue &v) {
    Memo m;
    m.slug = v["slug"].asString();
    m.contentHtml = v["content"].asString();
    m.contentText = htmlToText(m.contentHtml);
    m.createdAt = v["created_at"].asString();
    m.updatedAt = v["updated_at"].asString();
    m.deleted = !v["deleted_at"].isNull() && !v["deleted_at"].asString().empty();
    const JsonValue &tags = v["tags"];
    if (tags.isArray())
        for (size_t i = 0; i < tags.size(); ++i) m.tags.push_back(tags[i].asString());
    if (m.tags.empty()) m.tags = extractTags(m.contentText);
    return m;
}

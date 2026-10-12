#include "weread_skill_client.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <esp_log.h>

#include "net/http.h"
#include "settings_manager.h"

static const char *TAG = "WereadSkill";

WereadSkillClient g_weread_skill;

static constexpr const char *kGatewayUrl = "https://i.weread.qq.com/api/agent/gateway";
static constexpr const char *kSkillVersion = "1.0.5";
// 官方 Skill 的参考实现（KOReader 的 weread.koplugin，见 lib/client.lua:gateway）必须带一个
// 桌面浏览器 UA —— esp_http_client 默认**一个 UA 都不发**，裸 POST 到 i.weread.qq.com 会被
// 网关/WAF 直接打回 4xx。这里照抄它那串。
static constexpr const char *kUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/135.0.0.0 Safari/537.36 Edg/135.0.0.0";

static std::string trimAscii(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && static_cast<unsigned char>(s[b]) <= ' ') ++b;
    while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ') --e;
    return s.substr(b, e - b);
}

// 网关失败时的回话通常是一小段 JSON：{"errcode":-2010,"errlog":"C7bd8zG","errmsg":"用户不存在"}。
// 捞出 errmsg 上屏（比一句笼统的"无效或已过期"准得多）；捞不到给空串，由调用方兜底。
static std::string gatewayErrMsg(const std::string &body) {
    if (body.empty()) return {};
    const JsonValue root = JsonValue::parse(body);   // const：operator[] 不会插入节点
    if (!root.isObject()) return {};
    const std::string msg = root["errmsg"].asString();
    return msg.empty() ? root["errMsg"].asString() : msg;
}

// 只转义 JSON 字符串里必须转的那几个；控制字符按 \u00XX 兜底。
static std::string jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

std::string WereadSkillClient::buildGatewayBody(const std::string &apiName,
                                                const std::vector<WereadGwParam> &params) {
    std::string body;
    body.reserve(128);
    body += "{\"api_name\":\"";
    body += jsonEscape(apiName);
    body += "\",\"skill_version\":\"";
    body += kSkillVersion;
    body += "\"";
    for (const auto &p : params) {
        body += ",\"";
        body += jsonEscape(p.key);
        body += "\":";
        if (p.numeric && !p.value.empty()) {
            body += p.value;  // 数字不加引号
        } else {
            body += "\"";
            body += jsonEscape(p.value);
            body += "\"";
        }
    }
    body += "}";
    return body;
}

std::vector<WereadSkillClient::SearchItem> WereadSkillClient::parseSearch(const JsonValue &root) {
    std::vector<SearchItem> out;
    const JsonValue &groups = root["results"];
    if (!groups.isArray()) return out;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const JsonValue &group = groups[gi];
        const JsonValue &books = group["books"];
        if (!books.isArray()) continue;
        for (size_t bi = 0; bi < books.size(); ++bi) {
            const JsonValue &entry = books[bi];
            // entry 可能是 {bookInfo:{…}, reason:…}，也可能直接就是 bookInfo。
            const JsonValue &info = entry.has("bookInfo") ? entry["bookInfo"] : entry;
            if (!info.isObject()) continue;
            SearchItem it;
            it.bookId = info["bookId"].asString();
            if (it.bookId.empty()) continue;
            it.title = info["title"].asString();
            it.author = info["author"].asString();
            it.category = info["category"].asString();
            it.intro = info["intro"].asString();
            it.rating = static_cast<float>(info["newRating"].asNumber() / 10.0);
            it.readingCount = info["readingCount"].asInt();
            out.push_back(std::move(it));
        }
    }
    return out;
}

std::vector<WereadSkillClient::RecommendItem> WereadSkillClient::parseRecommend(const JsonValue &root) {
    std::vector<RecommendItem> out;
    const JsonValue &books = root["books"];
    if (!books.isArray()) return out;
    for (size_t bi = 0; bi < books.size(); ++bi) {
        const JsonValue &info = books[bi];
        if (!info.isObject()) continue;
        RecommendItem it;
        it.bookId = info["bookId"].asString();
        if (it.bookId.empty()) continue;
        it.title = info["title"].asString();
        it.author = info["author"].asString();
        it.category = info["category"].asString();
        it.intro = info["intro"].asString();
        it.reason = info["reason"].asString();
        it.rating = static_cast<float>(info["newRating"].asNumber() / 10.0);
        it.readingCount = info["readingCount"].asInt();
        out.push_back(std::move(it));
    }
    return out;
}

bool WereadSkillClient::gateway(const std::string &apiName,
                                const std::vector<WereadGwParam> &params,
                                JsonValue &out, Result &result) {
    // 真值只在设置文件里（`/sdcard/settings/weread_api_key`），端上敲进去的——两头都可能
    // 带上空格/换行，先削掉再发，否则服务端看到的是另一个字符串。
    const std::string key = trimAscii(g_settings.getString("weread_api_key"));
    if (key.empty()) {
        result.no_key = true;
        result.error = "请先在设置里配置微信读书 API Key";
        return false;
    }
    // 形状自查。key 是端上盲敲进去的（编辑屏原本画 '*'，见 settingsEditShowsPlaintext），
    // 敲错一位服务端只会回一句"用户不存在"。先本地拦一道，省一个来回。
    if (key.rfind("wrk-", 0) != 0) {
        result.unauthorized = true;
        result.error = "API Key 格式不对（应以 wrk- 开头；当前 " + std::to_string(key.size()) + " 位）";
        return false;
    }

    net::Request req;
    req.url = kGatewayUrl;
    req.method = net::Method::Post;
    req.headers = {{"Content-Type", "application/json;charset=UTF-8"},
                   {"Authorization", "Bearer " + key},
                   {"User-Agent", kUserAgent},
                   {"Accept", "application/json, text/plain, */*"},
                   {"Origin", "https://weread.qq.com"},
                   {"Referer", "https://weread.qq.com/"}};
    req.body = buildGatewayBody(apiName, params);
    req.timeout_ms = 20000;   // 单次读
    req.deadline_ms = 20000;  // 整请求墙钟
    req.cap = 262144;
    req.cap_hard = true;
    // **必须发 SNI**：i.weread.qq.com 在腾讯的共享入口后面，靠 SNI 选 vhost。默认的
    // skip_cert_check=true 在 IDF 里会连 SNI 一起关掉（日志原话："hostname matching and
    // SNI disabled"），落到默认 vhost 上就是不回话。这里走完整校验：crt_bundle 认根 +
    // 查 CN + 发 SNI。证书是正经公共 CA，本来也不该跳过。
    req.skip_cert_check = false;

    // 只记形状，不记真值：长度 + 头几个字符够判断"端上敲进去时被加了东西"。
    ESP_LOGI(TAG, "gateway %s body=%d key=%zu/%s", apiName.c_str(), (int)req.body.size(), key.size(),
             key.rfind("wrk-", 0) == 0 ? "wrk-前缀" : "非 wrk- 前缀");
    // 传输层失败再试一次：WiFi 刚拿到 IP 那几秒里 DNS 常常还没就绪——实测 getaddrinfo
    // 直接返回错误（整段失败只要 7s，根本不是超时）。用户点了才发，不值得为一次抖动放弃。
    net::Response resp = net::request(req);
    if (!resp.ok && resp.status == 0 && !resp.cancelled && !resp.timed_out) {
        ESP_LOGW(TAG, "gateway %s 传输层失败(%s)，重试一次", apiName.c_str(), esp_err_to_name(resp.err));
        resp = net::request(req);
    }

    if (resp.cancelled) {
        result.error = "已取消";
        return false;
    }
    if (resp.timed_out) {
        result.error = "微信读书响应超时";
        return false;
    }
    // 4xx 把网关自己的回话也捞出来——通常是一段 JSON 错误（errCode/errMsg），能直接点名原因。
    if (!resp.ok) {
        ESP_LOGW(TAG, "gateway %s failed: status=%d err=%s transport=%s body=%s", apiName.c_str(),
                 resp.status, esp_err_to_name(resp.err), resp.error.c_str(),
                 resp.body.substr(0, 300).c_str());
    } else {
        ESP_LOGI(TAG, "gateway %s ok: %d bytes", apiName.c_str(), (int)resp.body.size());
    }
    if (resp.status == 401) {
        result.unauthorized = true;
        // 网关自己的话说得更准：401 的 body 是 {"errcode":-2010,"errmsg":"用户不存在"}，
        // 有 errmsg 就带上——"用户不存在"和"key 过期"的下一步完全不同。
        const std::string why = gatewayErrMsg(resp.body);
        result.error = why.empty() ? std::string("API Key 无效或已过期") : ("API Key 被拒：" + why);
        return false;
    }
    if (resp.status == 403) {
        // 403 不是"key 过期"，是网关把这次请求整个拒了（UA/来源/频控）。
        result.unauthorized = true;
        const std::string why = gatewayErrMsg(resp.body);
        result.error = why.empty() ? std::string("微信读书拒绝了请求（403）") : ("请求被拒：" + why);
        return false;
    }
    if (!resp.ok) {
        if (resp.status != 0) {
            result.error = "微信读书接口返回错误 " + std::to_string(resp.status);
        } else {
            // 传输层没拿到响应。resp.error 是 http.cpp 给的中文（"网络连接失败" 等），
            // 比一句笼统的"网络请求失败"更能定位（WiFi 断了 / TLS 握不上 / DNS 挂）。
            result.error = resp.error.empty() ? std::string("网络请求失败") : resp.error;
        }
        return false;
    }
    ESP_LOGI(TAG, "gateway %s ok: %d bytes", apiName.c_str(), (int)resp.body.size());

    out = JsonValue::parse(resp.body);
    if (out.isNull() && !resp.body.empty()) {
        result.error = "响应解析失败";
        return false;
    }
    return true;
}

WereadSkillClient::Result WereadSkillClient::search(const std::string &keyword, int count) {
    Result r;
    const std::string kw = trimAscii(keyword);
    if (kw.empty()) {
        r.error = "请输入搜索关键字";
        return r;
    }

    std::vector<WereadGwParam> params = {
        {"keyword", kw, false},
        {"scope", "10", true},   // 10 = 只搜书籍
        {"maxIdx", "0", true},
        {"count", std::to_string(count), true},
    };
    JsonValue root;
    if (!gateway("/store/search", params, root, r)) return r;
    r.search = parseSearch(root);
    r.success = true;
    return r;
}

WereadSkillClient::Result WereadSkillClient::recommend(int count) {
    Result r;
    std::vector<WereadGwParam> params = {
        {"count", std::to_string(count), true},
        {"maxIdx", "0", true},
    };
    JsonValue root;
    if (!gateway("/book/recommend", params, root, r)) return r;
    r.recommend = parseRecommend(root);
    r.success = true;
    return r;
}

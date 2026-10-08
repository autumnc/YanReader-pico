#include "http.h"
#include "http_cap.h"

#include <cstdio>
#include <cstdlib>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "HTTP";

namespace net {
namespace {

esp_http_client_method_t toIdf(Method m) {
    switch (m) {
        case Method::Get: return HTTP_METHOD_GET;
        case Method::Post: return HTTP_METHOD_POST;
        case Method::Put: return HTTP_METHOD_PUT;
        case Method::Delete: return HTTP_METHOD_DELETE;
        case Method::Head: return HTTP_METHOD_HEAD;
        case Method::Mkcol: return HTTP_METHOD_MKCOL;
        case Method::Propfind: return HTTP_METHOD_PROPFIND;
        case Method::Patch: return HTTP_METHOD_PATCH;
    }
    return HTTP_METHOD_GET;
}

bool methodHasBody(Method m) { return m != Method::Get && m != Method::Head; }

esp_http_client_config_t makeConfig(const Request &req) {
    esp_http_client_config_t cfg = {};
    cfg.url = req.url.c_str();
    cfg.method = toIdf(req.method);
    cfg.timeout_ms = req.timeout_ms;
    if (req.skip_cert_check) {
        cfg.skip_cert_common_name_check = true;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    if (req.buffer_size > 0) cfg.buffer_size = req.buffer_size;
    cfg.max_redirection_count = req.max_redirects;
    return cfg;
}

void applyHeaders(esp_http_client_handle_t client, const Request &req) {
    for (const Header &h : req.headers) {
        esp_http_client_set_header(client, h.name.c_str(), h.value.c_str());
    }
}

void setHttpError(Response &r) {
    char buf[32];
    snprintf(buf, sizeof(buf), "HTTP %d", r.status);
    r.error = buf;
}

// ── 流式路（perform + ON_DATA）───────────────────────────────────────────
struct StreamCtx {
    const Request *req = nullptr;
    const ChunkFn *sink = nullptr;
    std::string *mem = nullptr;  // sink 为空时收这里
    size_t got = 0;
    bool overflow = false;
    bool sink_failed = false;
    bool truncated = false;
    bool timed_out = false;
    bool cancelled = false;
    int64_t deadline_us = 0;  // 0 = 不限
    int last_status = -1;
};

esp_err_t onData(esp_http_client_event_t *evt) {
    StreamCtx *c = static_cast<StreamCtx *>(evt->user_data);
    if (!c || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) return ESP_OK;

    // 墙钟上限 / 取消判定。到点把传输超时压到 1ms：下一次读立刻回超时，perform
    // 随即退出（见文件头第 2 条）。只在 ON_DATA 上判定就够——真的一个字节都不来，
    // 单次读的 timeout_ms 自然兜底。
    const bool past_deadline = c->deadline_us != 0 && esp_timer_get_time() > c->deadline_us;
    const bool cancelled = c->req->cancel && c->req->cancel->load(std::memory_order_acquire);
    if (past_deadline || cancelled) {
        c->timed_out = past_deadline;
        c->cancelled = cancelled;
        if (evt->client) esp_http_client_set_timeout_ms(evt->client, 1);
        return ESP_OK;
    }

    if (evt->client) {
        // 每个分片都重取状态码，**不能**用锁存：跟随重定向时中间那个 302 响应
        // 也可能带一小段 body（HTML 提示页），锁存会把后面的正式内容一起丢掉。
        // 3xx 的分片整段丢弃；状态码一变就把攒下的清掉（那是上一跳的）。
        const int st = esp_http_client_get_status_code(evt->client);
        if (st >= 300 && st < 400) return ESP_OK;
        if (st != c->last_status) {
            if (c->mem) c->mem->clear();
            c->last_status = st;
        }
    }

    const size_t n = static_cast<size_t>(evt->data_len);
    const CapDecision cap = decideCapKeep(c->got, n, c->req->cap, c->req->cap_hard);
    const size_t keep = cap.keep;
    if (cap.overflow) {
        c->overflow = true;
        return ESP_FAIL;  // 中止请求
    }
    if (cap.truncated) c->truncated = true;
    if (keep == 0 && c->req->cap != 0) return ESP_OK;

    if (c->sink) {
        if (keep > 0 && !(*c->sink)(static_cast<const uint8_t *>(evt->data), keep)) {
            c->sink_failed = true;
            return ESP_FAIL;  // 回调喊停（写盘失败等）
        }
    } else if (c->mem) {
        if (keep > 0) c->mem->append(static_cast<const char *>(evt->data), keep);
    }
    c->got += keep;

    if (c->req->progress) {
        size_t total = 0;
        if (evt->client) {
            const int64_t cl = esp_http_client_get_content_length(evt->client);
            if (cl > 0) total = static_cast<size_t>(cl);
        }
        c->req->progress(c->got, total);
    }
    return ESP_OK;
}

}  // namespace

Response stream(const Request &req, const ChunkFn &sink) {
    Response r;
    if (req.url.empty()) {
        r.err = ESP_ERR_INVALID_ARG;
        r.error = "URL 为空";
        return r;
    }

    std::string mem;
    StreamCtx ctx;
    ctx.req = &req;
    ctx.sink = sink ? &sink : nullptr;
    ctx.mem = ctx.sink ? nullptr : &mem;
    if (req.deadline_ms > 0) ctx.deadline_us = esp_timer_get_time() + req.deadline_ms * 1000;

    esp_http_client_config_t cfg = makeConfig(req);
    cfg.user_data = &ctx;
    cfg.event_handler = onData;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        r.err = ESP_ERR_NO_MEM;
        r.error = "HTTP 初始化失败";
        return r;
    }
    applyHeaders(client, req);

    const esp_err_t e = esp_http_client_perform(client);  // 内部跟随重定向
    r.got = ctx.got;
    r.overflow = ctx.overflow;
    r.sink_failed = ctx.sink_failed;
    r.truncated = ctx.truncated;
    r.timed_out = ctx.timed_out;
    r.cancelled = ctx.cancelled;
    const int errno_ = esp_http_client_get_errno(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    r.status = status;
    if (ctx.sink == nullptr) r.body = std::move(mem);

    if (e != ESP_OK) {
        r.err = e;
        ESP_LOGW(TAG, "stream 失败: %s errno=%d url=%s", esp_err_to_name(e), errno_, req.url.c_str());
        if (ctx.cancelled) r.error = "已取消";
        else if (ctx.timed_out) r.error = "请求超时";
        else if (ctx.overflow) r.error = "响应超过上限";
        else if (ctx.sink_failed) r.error = "响应写入失败";
        else r.error = std::string("请求失败: ") + esp_err_to_name(e);
        return r;
    }

    r.ok = status >= 200 && status < 300;
    if (ctx.cancelled) {
        r.ok = false;
        r.error = "已取消";
    } else if (ctx.timed_out) {
        r.ok = false;
        r.error = "请求超时";
    } else if (ctx.overflow) {
        r.ok = false;
        r.error = "响应超过上限";
    } else if (ctx.sink_failed) {
        r.ok = false;
        r.error = "响应写入失败";
    } else if (!r.ok) {
        setHttpError(r);
    }
    return r;
}

// ── 简单路（open / fetch_headers / read）────────────────────────────────
Response request(const Request &req) {
    Response r;
    if (req.url.empty()) {
        r.err = ESP_ERR_INVALID_ARG;
        r.error = "URL 为空";
        return r;
    }
    if (req.cancel && req.cancel->load(std::memory_order_acquire)) {
        r.cancelled = true;
        r.error = "已取消";
        return r;
    }

    esp_http_client_config_t cfg = makeConfig(req);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        r.err = ESP_ERR_NO_MEM;
        r.error = "HTTP 初始化失败";
        return r;
    }
    applyHeaders(client, req);

    const int bodyLen =
        (methodHasBody(req.method) && !req.body.empty()) ? static_cast<int>(req.body.size()) : 0;
    esp_err_t e = esp_http_client_open(client, bodyLen);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "open 失败: %s url=%s", esp_err_to_name(e), req.url.c_str());
        r.err = e;
        r.error = "网络连接失败";
        esp_http_client_cleanup(client);
        return r;
    }
    if (bodyLen > 0) {
        const int written = esp_http_client_write(client, req.body.data(), bodyLen);
        if (written != bodyLen) ESP_LOGW(TAG, "请求体只写出 %d/%d 字节", written, bodyLen);
    }
    esp_http_client_fetch_headers(client);
    r.status = esp_http_client_get_status_code(client);

    // 需要轮询取消时把单次读超时压到 1s：否则一次读要阻塞整个 timeout_ms，
    // 标志来不及看。放在 fetch_headers 之后，免得拖累 TLS 握手。
    if (req.cancel && req.timeout_ms > 1000) esp_http_client_set_timeout_ms(client, 1000);
    // 兜底墙钟：调用方既没给 deadline_ms 也没给 cancel 时，对端连上后挂住/挤牙膏
    // （每 29 秒挤一个字节）会让下面那个 while **永远出不来** —— EAGAIN 只是"回头再看
    // 一眼标志"，没有标志可看就是死循环，而它常常跑在主任务上（整个 UI 卡死、电源键
    // 待机都排不上队）。给个足够宽松的硬上限（5 分钟：比词典下载的 10 分钟短，比任何
    // 交互式请求长），保证一定能退出。调用方给了 deadline_ms 就听调用方的。
    const int64_t effective_deadline_ms =
        req.deadline_ms > 0 ? req.deadline_ms : (req.cancel ? 0 : 300000);
    const int64_t deadline_us = effective_deadline_ms > 0
                                    ? esp_timer_get_time() + effective_deadline_ms * 1000
                                    : 0;

    // 1KB 读缓冲从**栈上搬到堆上**。GCC 在函数入口就把 `char buf[1024]` 的帧分配好了，
    // 所以 TLS 握手（跑在 esp_http_client_open 里）那一刻它也压在这趟调用的栈上 ——
    // 而这趟调用常跑在只有 4096~7680 栈的 worker 里（Flomo 登录、词典下载、WebDAV 同步），
    // 那 1KB 就是它们栈里最重的一块可搬重量。放 PSRAM：它不是 DMA 缓冲（走
    // esp_http_client_read 的 memcpy 路径），外部内存安全，而且不占内部 RAM ——
    // 内部 RAM 的最大连续块正是这里最紧的东西（WiFi 一开就只剩 8192）。
    // 兜底用内部堆：宁可多吃 1KB 内部内存，也别让请求不干活。
    char *buf = static_cast<char *>(heap_caps_malloc(1024, MALLOC_CAP_SPIRAM));
    if (!buf) buf = static_cast<char *>(malloc(1024));
    if (!buf) {
        esp_http_client_cleanup(client);
        r.err = ESP_ERR_NO_MEM;
        r.error = "内存不足";
        return r;
    }
    while (true) {
        if (req.cancel && req.cancel->load(std::memory_order_acquire)) {
            r.cancelled = true;
            break;
        }
        if (deadline_us != 0 && esp_timer_get_time() > deadline_us) {
            r.timed_out = true;
            break;
        }
        if (req.cap != 0 && r.got >= req.cap) {
            if (req.cap_hard) r.overflow = true;
            else r.truncated = true;
            break;
        }
        const int n = esp_http_client_read(client, buf, 1024);
        if (n > 0) {
            const CapDecision cap = decideCapKeep(r.got, static_cast<size_t>(n), req.cap, req.cap_hard);
            const size_t keep = cap.keep;
            if (cap.overflow) {
                r.overflow = true;
                break;
            }
            if (cap.truncated) r.truncated = true;
            if (keep > 0) {
                r.body.append(buf, keep);
                r.got += keep;
            }
        } else if (n == -ESP_ERR_HTTP_EAGAIN) {
            continue;  // 单次读超时，回头再看标志
        } else {
            break;  // 0 = 读完；<0 = 传输错误
        }
    }
    free(buf);   // heap_caps_malloc 的内存也用 free 释放（同一个分配器）
    esp_http_client_cleanup(client);

    r.err = ESP_OK;
    if (r.cancelled) {
        r.error = "已取消";
        return r;
    }
    if (r.timed_out) {
        r.error = "请求超时";
        return r;
    }
    r.ok = r.status >= 200 && r.status < 300 && !r.overflow;
    if (!r.ok) {
        if (r.overflow) r.error = "响应超过上限";
        else if (r.status == 0) r.error = "网络连接失败";
        else setHttpError(r);
    }
    return r;
}

}  // namespace net

#pragma once
// net/http —— 全仓唯一的 HTTP 客户端（架构评审 P2b）。
//
// 只管一件事：**把一次请求发出去、把响应体收回来**。不管 JSON、不管重试、
// 不管签名——那些留在各自的 client 里（他们更清楚自己的重试窗口和认证约定）。
//
// 两个入口，按"要不要跟随重定向 / 要不要流式"选：
//   request()  —— open/fetch_headers/read 逐次读，**不跟随重定向**，body 收进内存。
//   stream()   —— perform + ON_DATA 事件流，**会跟随重定向**，body 逐块交给回调。
//
// 为什么保留两条路而不是合成一条：跟随重定向只有 perform() 会做（GitHub Release
// 的下载地址必然 302 跳到 objects.githubusercontent.com），而 perform 的取消/进度
// 只能从事件回调里做；反过来，几个老站点（flomo / deepseek / webdav）一直是
// open/read 那套，换成 perform 会连它们"遇到 3xx 会怎样"和错误码语义一起换掉。
// 两条路都收在这里，就是为了让调用方按需要选，而不是各自再抄一份。
//
// 两个已经踩过的坑直接烧进实现，不要再踩：
//   1. `timeout_ms` 是**每次读**的超时，不是整个请求的时长——对端每隔 29 秒挤一个
//      字节，perform() 就永远回不来，而它是在主任务上同步跑的，整个 UI 卡死、
//      按键取消也排不上队。所以请求真需要上限时用 `deadline_ms`（墙钟）。
//   2. 墙钟到点后唯一能中止 perform 的办法是把传输超时压到 1ms
//      （`set_timeout_ms` 是无锁公开 API，可以在事件回调里直接调），下一次读
//      立刻返回超时、perform 随即退出。见 [[http-client-abort-trickling]]。

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <esp_err.h>

namespace net {

enum class Method { Get, Post, Put, Delete, Head, Mkcol, Propfind, Patch };

struct Header {
    std::string name;
    std::string value;
};

// 一次请求。默认值就是全仓最常用的那组（30s 读超时、crt_bundle、不校验 CN）。
struct Request {
    std::string url;
    Method method = Method::Get;
    std::vector<Header> headers;
    std::string body;  // 请求体；Get/Head 忽略

    int timeout_ms = 30000;   // **单次读**超时（不是总时长，见文件头第 1 条）
    int64_t deadline_ms = 0;  // 从发起时刻算起的整请求墙钟上限；0 = 不限
    // stream() 用：最多跟这么多跳重定向。**别设 0**——perform() 遇到 3xx 会直接
    // 报 ESP_ERR_HTTP_MAX_REDIRECT，而不是把 3xx 当最终响应交回来。
    // request() 不看这个字段（它本来就不跟随重定向）。
    int max_redirects = 5;
    int buffer_size = 0;      // esp_http_client 内部缓冲；0 = 库默认
    bool skip_cert_check = true;  // 与全仓现状一致：挂 crt_bundle 且不校验 CN

    size_t cap = 0;        // 响应体上限；0 = 不限
    bool cap_hard = false;  // true = 超限即中止请求；false = 丢弃余下（truncated）

    // 进度回调。total > 0 表示服务器给了 content-length（chunked 时为 0）。
    std::function<void(size_t got, size_t total)> progress;
    // 非空时轮询：读到 true 即中止。代价是单次读超时被压到 1s（只有这个粒度）。
    std::atomic_bool *cancel = nullptr;
};

struct Response {
    esp_err_t err = ESP_OK;  // 传输层结果；ESP_OK 不代表 HTTP 2xx
    int status = 0;          // HTTP 状态码；0 = 没拿到
    bool ok = false;         // 传输完成、状态 2xx，且没有 overflow/取消/超时
    std::string body;        // request() 与 sink 为空的 stream() 填这个
    size_t got = 0;          // 收到的响应体字节数
    bool timed_out = false;  // 撞上 deadline_ms
    bool overflow = false;   // 撞上 cap_hard，请求已中止
    bool sink_failed = false;  // sink 回调返回 false（写盘失败等），请求已中止
    bool truncated = false;  // 软上限：余下被丢弃，got 停在 cap
    bool cancelled = false;  // cancel 置位
    std::string error;       // 可直接上屏的中文；ok 时为空
};

// 响应体分片回调；返回 false = 中止请求（写盘失败等），response.sink_failed 置位。
using ChunkFn = std::function<bool(const uint8_t *data, size_t len)>;

// open/fetch_headers/read：不跟随重定向，body 收进内存。适合小响应（JSON/XML）
// 和本来就是"发完等回话"的老站点。
Response request(const Request &req);

// perform + ON_DATA：跟随重定向，body 逐块交给 sink（为空则收进 body）。
// 适合大文件（词典/电子书）和必须跟重定向的地址（GitHub Release 清单）。
Response stream(const Request &req, const ChunkFn &sink = ChunkFn{});

}  // namespace net

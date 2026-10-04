#pragma once

#include <OpdsParser.h>

#include <functional>
#include <string>
#include <vector>

// OPDS(Atom) 目录拉取与电子书下载。HTTP 走 esp_http_client，解析复用 crossmux 的
// OpdsParser（expat）。仅在已连 WiFi 时调用；两个函数都是阻塞式的（超时 20~30s）。

// 拉取一个 OPDS feed。成功时 entries 为解析出的条目，其中每个 href 都已解析成
// 相对该 feed URL 的绝对地址；nextPageUrl 回填 feed 里的 rel="next"（同样绝对化，
// 无则清空）。失败返回 false 且 err 填入原因。
bool opdsFetchFeed(const std::string &url, std::vector<OpdsEntry> &entries,
                   std::string &nextPageUrl, std::string &err);

// 下载 url 到 destPath（覆盖写）。totalUnknown 为真时 total 参数恒为 0。
// progress 可为空。失败返回 false 且 err 填入原因。
bool opdsDownloadFile(const std::string &url, const std::string &destPath,
                      const std::function<void(size_t, size_t)> &progress,
                      std::string &err);

// 从 URL 取一个安全、带扩展名的文件名（去查询串/目录/非法字符，%XX 解码）。
// 取不出有效名字时返回空串。
std::string opdsFilenameFromUrl(const std::string &url);

// 把 href 相对 base 解析成绝对 URL（处理 ./ ../ 与协议相对 //host）。
std::string opdsResolveUrl(const std::string &base, const std::string &href);

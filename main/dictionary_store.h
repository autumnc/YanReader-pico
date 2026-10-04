#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// StarDict 词典的下载/安装/管理。数据来源是 crossmux 的词典清单（JSON），
// 每个词条由若干文件（<id>.idx / .dict 或 .dict.dz / .ifo）组成，逐个下载到
// /sdcard/dictionaries/<id>/，并写一个版本标记文件 `.crossmux-resource`
// （内容 "1:<revision>\n"）——crossmux 的 DictionaryRegistry 认这个标记，
// 本移植的 screen_reader 直接扫目录，两边都能用。
//
// 曾经这里要求"不得引入 esp_http_client.h"——lwip 的 sockets.h/fcntl.h 会定义
// O_RDONLY 等宏，遮蔽 crossmux FsApiConstants.h 里的同名常量（读写标志被换掉，
// 把文件截成 0）。那些常量已改名 FS_*，宏再也撞不上，这条限制随之解除；
// 保留一行说明，免得以后有人把标志名改回去。

#define DICT_ROOT_DIR "/sdcard/dictionaries"
#define DICT_MARKER_NAME ".crossmux-resource"

// 默认清单地址（设置项 dict_manifest_url 可覆盖）。lang=zh 时是英汉+汉语词典，
// lang=en 是纯英文；两者内容一致，取默认即可。
#define DICT_MANIFEST_DEFAULT \
    "https://crossmux.com/api/assets/dictionaries/manifest?version=1&lang=zh"

// 清单里的一个文件（下载后按 size/crc32 校验）。
struct DictFileRef {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
};

// 清单里的一个词条（含本地安装状态）。
struct DictCatalogItem {
    std::string id;
    std::string name;
    std::string description;
    int revision = 0;
    size_t totalSize = 0;   // 所有文件字节数之和
    std::vector<DictFileRef> files;
    int installedRevision = 0;  // 0 = 未安装
    int fileCount() const { return static_cast<int>(files.size()); }
    bool installed() const { return installedRevision > 0; }
    bool updateAvailable() const { return installedRevision > 0 && installedRevision < revision; }
};

// 一次清单拉取的结果。install 需要 baseUrl，所以和 items 一起带出。
struct DictCatalog {
    std::string baseUrl;
    int revision = 0;
    std::vector<DictCatalogItem> items;
};

// 本地已装词条（扫描 DICT_ROOT_DIR 得到）。
struct DictLocalItem {
    std::string id;
    size_t sizeBytes = 0;
    int revision = 0;
};

// 拉取并解析清单。成功返回 true；失败时 err 填原因（网络/JSON/版本不符）。
bool dictCatalogFetch(const std::string &url, DictCatalog &out, std::string &err);

// 安装第 idx 项到 /sdcard/dictionaries/<id>（先写 .staging 再改名，失败不留半成品）。
// progress(fileIdx, fileCount, done, total, phase) 每个文件内按块回调，可为空。
// phase 为 "下载 <文件名>" / "校验" 等短串。
bool dictInstall(const DictCatalog &cat, int idx,
                 const std::function<void(int, int, size_t, size_t, const char *)> &progress,
                 std::string &err);

// 扫描本地已装词条（跳过 .staging / .bak 半成品）。
void dictListLocal(std::vector<DictLocalItem> &out);

// 删除本地词条目录。未安装也算成功。
bool dictDelete(const std::string &id, std::string &err);

// 人类可读的体积（"27.3 MB" / "742 KB"）。
std::string dictFormatSize(size_t bytes);

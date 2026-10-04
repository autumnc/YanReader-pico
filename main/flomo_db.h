#pragma once

// 本地笔记库。移植自 ../Flomo/src/db.{h,cpp}：整个库就是 <dir>/memos.json 一个文件，
// 原子写（见 flomo_file.h）。目录固定 /sdcard/flomo。

#include <string>
#include <vector>

#include "flomo_api.h"

struct Store {
    std::string token;
    std::vector<Memo> memos;
    std::string lastSync;   // 上次同步时间戳（展示用）
};

class MemoDb {
public:
    explicit MemoDb(std::string dir) : dir_(std::move(dir)) {}
    bool load(Store &store);
    bool save(const Store &store);
    const std::string &dir() const { return dir_; }

private:
    std::string path() const;
    std::string dir_;
};

// 落库去重规则（与 Flomo 一致）：同 slug 已存在时，若本地这条有未推上去的改动
// （dirty 且不是待删除），保留本地版本，否则用远端覆盖。新 slug 直接追加。
void upsertMemo(std::vector<Memo> &memos, const Memo &memo);

// 按 updatedAt 倒序（字符串比较即可，ISO 时间戳）。
void sortMemos(std::vector<Memo> &memos);

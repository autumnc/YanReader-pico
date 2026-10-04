#include "flomo_db.h"

#include "flomo_file.h"
#include "flomo_json.h"

#include <algorithm>

std::string MemoDb::path() const { return dir_ + "/memos.json"; }

bool MemoDb::load(Store &store) {
    flomoEnsureDir(dir_);
    JsonValue root = JsonValue::loadFromFile(path());
    if (!root.isObject()) return true;
    store.token = root["token"].asString();
    store.lastSync = root["last_sync"].asString();
    const JsonValue &arr = root["memos"];
    if (arr.isArray()) {
        for (size_t i = 0; i < arr.size(); ++i) {
            Memo m = memoFromJson(arr[i]);
            m.contentText = arr[i]["content_text"].asString(m.contentText);
            m.dirty = arr[i]["dirty"].asBool(false);
            m.pendingOp = arr[i]["pending_op"].asString();
            store.memos.push_back(m);
        }
    }
    sortMemos(store.memos);
    return true;
}

bool MemoDb::save(const Store &store) {
    flomoEnsureDir(dir_);
    JsonValue root = JsonValue::object();
    root.set("token", JsonValue(store.token));
    root.set("last_sync", JsonValue(store.lastSync));
    JsonValue arr = JsonValue::array();
    for (const auto &m : store.memos) {
        JsonValue v = JsonValue::object();
        v.set("slug", JsonValue(m.slug));
        v.set("content", JsonValue(m.contentHtml));
        v.set("content_text", JsonValue(m.contentText));
        v.set("created_at", JsonValue(m.createdAt));
        v.set("updated_at", JsonValue(m.updatedAt));
        v.set("deleted_at", m.deleted ? JsonValue("1") : JsonValue(nullptr));
        v.set("dirty", JsonValue(m.dirty));
        v.set("pending_op", JsonValue(m.pendingOp));
        JsonValue tags = JsonValue::array();
        for (auto &t : m.tags) tags.pushBack(JsonValue(t));
        v.set("tags", tags);
        arr.pushBack(v);
    }
    root.set("memos", arr);
    return JsonValue::saveToFile(path(), root);
}

void upsertMemo(std::vector<Memo> &memos, const Memo &memo) {
    for (auto &m : memos) {
        if (m.slug == memo.slug) {
            bool keepLocal = m.dirty && m.pendingOp != "delete";
            if (!keepLocal) m = memo;
            return;
        }
    }
    memos.push_back(memo);
}

void sortMemos(std::vector<Memo> &memos) {
    std::sort(memos.begin(), memos.end(), [](const Memo &a, const Memo &b) {
        return a.updatedAt > b.updatedAt;
    });
}

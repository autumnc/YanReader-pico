#include "ui/ime_field.h"

#include "ime/IME.h"

// 所有原语都按同一套约定走 UTF-8：**续字节长这样 10xxxxxx**（0xC0 掩码 == 0x80），
// 所以"往前一个字符"就是从 pos-1 起不断跳过续字节，"往后一个字符"就是从 pos+1 起
// 不断跳过续字节。骨架非法（首字节缺失）时最多退到 0，不会越界 —— 这正是要统一的地方：
// 之前三个宿主各写一份，边界处理各不相同。

bool imeFieldInsert(ImeField f, const std::string &s) {
    if (!f.valid() || s.empty()) return false;
    const int c = f.cur() < 0 ? 0 : (f.cur() > f.length() ? f.length() : f.cur());
    f.text->insert(c, s);
    f.setCur(c + (int)s.size());   // 光标为 nullptr 时这步是空操作（追加型字段）
    return true;
}

bool imeFieldBackspace(ImeField f) {
    if (!f.valid()) return false;
    const int c = f.cur();
    if (c <= 0) return false;
    int prev = c - 1;
    while (prev > 0 && ((unsigned char)(*f.text)[prev] & 0xC0) == 0x80) prev--;
    f.text->erase(prev, c - prev);
    f.setCur(prev);
    return true;
}

bool imeFieldMoveLeft(ImeField f) {
    if (!f.valid()) return false;
    int c = f.cur();
    if (c <= 0) return false;
    c--;
    while (c > 0 && ((unsigned char)(*f.text)[c] & 0xC0) == 0x80) c--;
    f.setCur(c);
    return true;
}

bool imeFieldMoveRight(ImeField f) {
    if (!f.valid()) return false;
    int c = f.cur();
    const int n = f.length();
    if (c >= n) return false;
    c++;
    while (c < n && ((unsigned char)(*f.text)[c] & 0xC0) == 0x80) c++;
    f.setCur(c);
    return true;
}

void imeFieldMoveEnd(ImeField f) {
    if (!f.valid()) return;
    f.setCur(f.length());
}

bool imeFieldKeyText(IME &ime, int key, bool multiline, std::string &out) {
    out.clear();
    // handleKey 只在真的动了输入法状态（编码/候选/上屏）时才返回 true —— 这也是
    // "这个键归输入法管"的判据：未组合时的半角标点、英文态下的字母、数字面板的 1..9/0
    // 它都放行，那些要靠下面这条 ASCII 兜底，否则"数字面板打不出数字"。
    if (ime.handleKey(key, out)) {
        if (!multiline && out == "\n") out.clear();   // 单行字段不吃换行
        return true;
    }
    if (key >= 0x20 && key <= 0x7E) {
        out.assign(1, (char)key);
        return true;
    }
    return false;
}

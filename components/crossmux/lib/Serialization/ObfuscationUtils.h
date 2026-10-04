#pragma once
// 凭据混淆：用芯片 eFuse MAC 当密钥做 XOR，再 base64 编码，避免登录 cookie 以明文躺在 SD 卡上。
// 只防"随手看看"，不是加密；和原版一样与具体芯片绑定。
#include <cstddef>
#include <cstdint>
#include <string>

namespace obfuscation {

// XOR 变换（对称操作），密钥取本机 MAC。
void xorTransform(std::string& data);

// base64 后返回（供 session 文件存储）。
std::string obfuscateToBase64(const std::string& plaintext);

// 解码 + 反混淆；base64 非法时返回空串并置 *ok=false。
std::string deobfuscateFromBase64(const char* encoded, bool* ok = nullptr);

std::string deobfuscateFromBase64(const char* encoded, size_t maxDecodedLength, bool* ok, bool* tooLong);

}  // namespace obfuscation

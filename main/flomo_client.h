#pragma once

#include <string>

struct FlomoResult {
    bool success;
    std::string message;
};

class FlomoClient {
public:
    FlomoClient();

    void configure(const std::string &email, const std::string &password);
    bool isConfigured() const;

    // Login and cache token, returns token or empty
    std::string login();

    // 上一次 login() 失败的原因（成功时清空）。界面直接显示它，别再统一说"登录失败"——
    // 服务端的限流/风控和"密码错了"是完全不同的两回事。
    const std::string &lastError() const { return lastError_; }

    // Create a memo (requires valid token from login())
    bool createMemo(const std::string &token, const std::string &content);

    // Convenience: send text to Flomo (handles login, caching)
    FlomoResult send(const std::string &text);

    // Store/retrieve token from settings
    std::string getCachedToken();
    void setCachedToken(const std::string &token);

private:
    std::string email_;
    std::string password_;
    std::string lastError_;

    std::string generateSign(const std::string &params);
};

extern FlomoClient g_flomo;

#pragma once

#include <atomic>
#include <string>
#include <vector>

struct DeepseekResult {
    bool success;
    std::string content;
};

// 一轮对话。role 用 OpenAI 那两个字面量（"user" / "assistant"）。
struct DeepseekTurn {
    std::string role;
    std::string content;
};

class DeepseekClient {
public:
    // Generate a journal writing prompt via Deepseek chat API
    DeepseekResult generatePrompt(const std::string &userContext);
    // Light polish of editor text (fallback when Xiaozhi fails).
    // customInstr may be empty (use the default polish rules only).
    // cancel (optional) is polled during the request; when set, the call
    // aborts quickly and returns success=false with content "已取消".
    DeepseekResult polishText(const std::string &text, const std::string &customInstr,
                              std::atomic_bool *cancel = nullptr);
    // 中性一层的多轮对话：system 非空就插在最前，turns 按顺序展开成 messages。
    // **阅读器的"选段/上下文"概念不进这里**——那是调用方拼 turn 时的事。
    // 走 deepseek-chat（不是 reasoner，见 .cpp 里的说明）。
    DeepseekResult chat(const std::string &system, const std::vector<DeepseekTurn> &turns,
                        std::atomic_bool *cancel = nullptr, int maxTokens = 800);
};

extern DeepseekClient g_deepseek;

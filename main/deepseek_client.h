#pragma once

#include <string>

struct DeepseekResult {
    bool success;
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
                              volatile bool *cancel = nullptr);
};

extern DeepseekClient g_deepseek;

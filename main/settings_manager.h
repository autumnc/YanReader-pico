#pragma once

#include <string>

class SettingsManager {
public:
    bool begin();
    std::string getString(const std::string &key, const std::string &def = "");
    void setString(const std::string &key, const std::string &val);
    void erase(const std::string &key);

    // Convenience accessors
    std::string flomoEmail();
    std::string flomoPassword();
    std::string flomoToken();
    std::string webdavUrl();
    std::string webdavUsername();
    std::string webdavPassword();
    std::string deepseekKey();
    std::string polishPrompt();
    std::string personalExperience();
    std::string personalHobbies();
    std::string wifiSsid();
    std::string wifiPassword();
    std::string timezone();
    std::string ntpServer();
    bool autoSave();
    // 空闲多久自动待机（分钟）：0 = 关，另有 5/10/15/20。设置入口在**阅读模式的设置**
    // 里（「自动待机」），但计时是全局的——三个模式共用同一个空闲计时（原来写作模式
    // 那个「自动休眠」开关已经删掉，它和待机本来就是同一件事）。
    int autoStandbyMinutes();
    bool sleepScreen();
    bool markdownRender();
    bool firstLineIndent();
    bool versionHistory();
    bool recoveryDraft();
    bool verticalReferenceLine();
    int fontSize();
    std::string appMode();  // "journal"(个人日记) 或 "quick"(快捷编辑)
    std::string homeView();  // "week"(周视图) 或 "month"(月视图)
    std::string inputMode();  // "normal"(正常) 或 "typewriter"(打字机)
    std::string imeFuzzy();  // comma/space separated fuzzy pinyin options
    std::string imePredictMode();  // "always" / "space" / "off"
    bool imeSentence();  // 长全拼串的整句候选
    bool imeDocContext();  // 正文已出现的词在小范围加分
    bool imeCandidateHighlight();  // 候选页内左右键高亮选择
    bool imeDebug();  // hidden: log IME candidate scoring details
    std::string editorOrientation();  // "horizontal"(横排) 或 "vertical"(竖排)
    std::string orientation();  // 屏幕方向 "landscape"(横屏) 或 "portrait"(竖屏), 立即生效
    std::string readerOrientation();  // 阅读器方向, 与全局屏幕方向独立, 仅阅读模式生效
    bool nightMode();  // 全设备夜间反色(在推屏唯一出口处取反整块 fb)
    std::string verticalReferenceLineStyle();  // "solid"(实线) / "dash"(虚线) / "dot"(点状虚线)
    bool typingClickEnabled();  // 是否播放按键反馈音
    // 输入法那两行（编码区+候选区）什么时候清残影："punct"(句读后,默认) / "commit"(上屏后)
    // / "both" / "off"。清一次 = 一次区域 GC16（≈330ms，见 ui_render.cpp），所以默认只挑
    // 句读——那是用户天然停手看结果的时刻，不会同一句话里连清两遍。
    std::string imeCleanMode();
    // 实体键盘打字时，**正文那一拍的推屏方式**："solid"(默认，整屏阈值 DU，墨实但每拍
    // 约 220ms) / "fast"(只推差分矩形的跟随 DU，约 56ms，但跟随表推力只有阈值表的
    // 1/4，刚上屏的字先发灰，等打字停顿由那一次区域 GC16 把它坐实)。见 ui_render.cpp
    // 的 ime_commit_fast_policy 与 render_present 的第 2 条路。
    std::string imeCommitMode();
    std::string clickChineseMode();  // "key"(每键一声) / "count"(上屏按字数) / "single"(上屏单声)
    int typingClickVolume();  // 0..100,按键音效音量
    int dailyGoalMinutes();   // 阅读统计的每日目标(分钟), 默认 30

    void setFlomoEmail(const std::string &v);
    void setFlomoPassword(const std::string &v);
    void setFlomoToken(const std::string &v);
    void setWebdavUrl(const std::string &v);
    void setWebdavUsername(const std::string &v);
    void setWebdavPassword(const std::string &v);
    void setDeepseekKey(const std::string &v);
    void setPolishPrompt(const std::string &v);
    void setPersonalExperience(const std::string &v);
    void setPersonalHobbies(const std::string &v);
    void setWifiSsid(const std::string &v);
    void setWifiPassword(const std::string &v);
    void setTimezone(const std::string &v);
    void setNtpServer(const std::string &v);
    void setOrientation(const std::string &v);
    void setReaderOrientation(const std::string &v);
    void setNightMode(bool on);

private:
    std::string get(const std::string &key);
    void set(const std::string &key, const std::string &val);
};

extern SettingsManager g_settings;

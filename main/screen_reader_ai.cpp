// screen_reader_ai.cpp — 阅读模式的 AI 对话屏。
//
// **一个屏、两个入口**（用户点名的形态）：
//   · 阅读页长按选段 → 选区动作条的「问AI」：st.aiCtx = 选中原文，就着它多轮追问；
//   · 「应用」标签页的「DeepSeek 对话」图标：st.aiCtx 为空，就是一个普通的聊天。
// 两者共用这一份实现，aiCtx 空不空只影响标题 / system prompt / 正文抬头三处。
//
// 交互：带输入框、可追问多轮（不是一问一答就结束）。走设置里已经配好的 DeepSeek Key
// （与写作模式的「AI 润色 / 生成提示」同一份 g_settings.deepseekKey()）。
//
// 请求形态照抄 screen_reader_files.cpp 的 FbScanJob 那一套：**一个堆对象同时当入参和结果**，
// worker 不碰 st、不渲染；完成后 release 存进原子指针，主任务在三个地方取件
// （空闲帧 / 按键帧 / 渲染帧）。阅读器的空闲帧**没有帧末统一推屏**，所以取到件的人必须
// 自己重绘 —— 少一处就是"答案到了、屏上还挂着'正在思考…'"。
//
// 没做的一件事：**没进 rdSelfTickingProgress()**。那个名单会把帧刷法钉成 STATUS_REFRESH，
// 而区域差分只在 `m == HALF_REFRESH && st.mode != RdMode::Reading` 时生效
// （screen_reader.cpp 的 reader_list_present）。本屏在飞期间不做动画（状态行画一次就不动），
// 不需要那条刷法；进去了反而把正常滚动也钉在整屏推上。

#include "screen_reader_internal.h"
#include "screen_reader.h"

#include "deepseek_client.h"
#include "settings_manager.h"
#include "wifi_manager.h"
#include "ime/IME.h"
#include "hw/input.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "RdAiChat";

// ── 体量上限（每条都要有个数：全在 PSRAM / 内部 RAM 里滚，不能靠"书不会那么大"）────
static constexpr size_t kAiMaxCtx = 4000;       // 选段原文：超了按 UTF-8 边界截断
static constexpr size_t kAiMaxQuestion = 1000;  // 单条问题：ime_field 不做 maxBytes，只能在这儿判
static const int kAiApiTurns = 6;               // 发给 API 的轮次上限（历史再长也不整段重发）
static const int kAiMaxTurns = 20;              // 保留的轮次上限（超了从头部 pop）
static constexpr size_t kAiMaxBody = 12 * 1024; // 组装后 body 的软上限（超了从最老的轮次开始丢）
static constexpr int kAiMaxLines = 3000;        // 折行行数上限（只防病态输入把行表撑爆）
static constexpr int kAiMaxTokens = 800;        // 一次回答几百字，配 30s 墙钟
static constexpr int kAiTaskStack = 7680;       // 见 rdAiChatSubmit 里的说明：**不能**用 8192

// ── worker 交接（照 screen_reader_files.cpp 的 FbScanJob）──────────────────────
// ctx/question/history 是入参（主任务填好交出所有权），ok/answer/error 是结果。
// **不许引用 st.aiTurns**：worker 线程读 st 是禁忌，history 必须是一份起飞前的快照。
struct RdAiJob {
  // 这一场对话的代号。rdAiChatBegin 每进一次就 ++，取件时对不上就整份丢掉 ——
  // 否则"上一条被 Esc 取消、用户重进后又落地"的那份回答会被塞进新对话的最后一轮。
  int gen = 0;
  std::string ctx, question;   // in
  std::vector<RdState::RdAiTurn> history;   // in
  bool ok = false;             // out
  std::string answer, error;   // out
};

static std::atomic<bool> s_aiRunning{false};
static std::atomic<RdAiJob *> s_aiReady{nullptr};   // release 存 / exchange(acq_rel) 取
static std::atomic_bool s_aiCancel{false};
static int s_aiGen = 0;      // 只在主任务里动

// ── 文本小工具 ───────────────────────────────────────────────────────────
static void trimAsciiInPlace(std::string &s) {
  size_t b = 0, e = s.size();
  while (b < e && static_cast<unsigned char>(s[b]) <= ' ') ++b;
  while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ') --e;
  s = s.substr(b, e - b);
}

// 截到 maxBytes，但**退到不会把一个多字节序列切两半的位置**（切一半的 UTF-8 送进
// json_escape 会变成乱码字节，服务端那边就是一段看不懂的串）。
static std::string limitUtf8(const std::string &s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t n = maxBytes;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return s.substr(0, n);
}

// system prompt 两版：有选段就把它内联进去（比"用户会再发一遍原文"省一轮，
// 也保证第一问就有上下文），没有就是一个普通的阅读助手。
static std::string aiSystemPrompt(const std::string &ctx) {
  if (ctx.empty()) {
    return "你是阅读助手。回答简洁、口语化，适合在电子墨水屏上阅读。";
  }
  return "你是阅读助手。用户正在读书，下面是他选中的一段原文：\n【选段】\n" + ctx +
         "\n请围绕这段文字回答用户的问题；回答简洁、口语化，不要复述原文。";
}

// 把历史拼成 API 的 messages（system 由调用方给）。两件事在这里做：
//   ① 只发最近 kAiApiTurns 轮；
//   ② 组装后超过 kAiMaxBody 就从最老的轮次开始丢（**至少留最后一轮** ——
//      一个没有 user 消息的请求没有任何意义）。
static std::vector<DeepseekTurn> aiBuildTurns(const std::string &system,
                                              const std::vector<RdState::RdAiTurn> &hist) {
  const int n = static_cast<int>(hist.size());
  int start = std::max(0, n - kAiApiTurns);
  std::vector<DeepseekTurn> turns;
  for (;;) {
    turns.clear();
    for (int i = start; i < n; i++) {
      if (!hist[i].q.empty()) turns.push_back({"user", hist[i].q});
      // 还没落地的回答是空串：跳过（发一条空的 assistant 消息会被服务端拒）。
      if (!hist[i].a.empty()) turns.push_back({"assistant", hist[i].a});
    }
    size_t est = system.size() + 64;
    for (const auto &t : turns) est += t.content.size() + 32;
    if (est <= kAiMaxBody || start >= n - 1) break;
    ++start;
  }
  return turns;
}

// 由 aiTurns **重建**整篇正文（不要 append-only：那样越聊越长，而且"回答落地/轮次被裁掉"
// 都得倒着改前面写下的东西）。重建时作废折行缓存。
static void aiRebuildTranscript() {
  std::string t;
  if (!st.aiCtx.empty()) {
    t += "【选段】\n";
    t += st.aiCtx;
    t += "\n";
  } else if (st.aiTurns.empty()) {
    t += "输入问题开始对话。\n";
  }
  for (size_t i = 0; i < st.aiTurns.size(); i++) {
    if (i > 0 || !st.aiCtx.empty()) t += "\n";
    t += "我：";
    t += st.aiTurns[i].q;
    t += "\nAI：";
    t += st.aiTurns[i].a.empty() ? "（正在思考…）" : st.aiTurns[i].a;
    t += "\n";
  }
  st.aiTranscript = std::move(t);
  st.aiLinesW = -1;
}

// ── 真活（worker 侧）──────────────────────────────────────────────────────
// 和屏幕润色那边一样拆成 body + task 两层：vTaskDelete **不回卷 C++ 栈**，
// `DeepseekResult dr`（含整段回答）要是留在任务函数里就不会析构 —— 每问一次漏一份。
static void rdAiRunJob(RdAiJob &job) {
  const std::string system = aiSystemPrompt(job.ctx);
  std::vector<DeepseekTurn> turns = aiBuildTurns(system, job.history);
  DeepseekResult dr = g_deepseek.chat(system, turns, &s_aiCancel, kAiMaxTokens);
  job.ok = dr.success;
  if (dr.success) job.answer = dr.content;
  else job.error = dr.content;
}

static void rdAiTask(void *arg) {
  std::unique_ptr<RdAiJob> job(static_cast<RdAiJob *>(arg));
  rdAiRunJob(*job);
  s_aiReady.store(job.release(), std::memory_order_release);
  s_aiRunning.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

// ── 取件 ─────────────────────────────────────────────────────────────────
// 返回"这一趟动了界面要用的状态"（调用方据此决定要不要当场重绘）。
// **先无条件 exchange 再判别的**：不管还要不要这份结果，堆对象总得有人放掉；
// 而且 s_aiRunning 必须先清 —— 它卡住的话下一次提交会被一直拒。
static bool aiPollResult() {
  std::unique_ptr<RdAiJob> job(s_aiReady.exchange(nullptr, std::memory_order_acq_rel));
  if (!job) return false;
  s_aiRunning.store(false, std::memory_order_release);
  if (job->gen != s_aiGen) return false;         // 上一场对话的残留：放掉，不碰 st
  // 本场在等的那一份落地了：先把在飞标记放下来，**再**看还在不在这一屏 —— 反了的话
  // "在飞时离开这一屏"会留下 aiBusy 永远为真（虽然重进屏会复位，但那是个破了的契约）。
  // 顺序不能更早：gen 不符的那一支下面还可能有新一场对话正等着，清它会误伤。
  st.aiBusy = false;
  // 这一拍可能有异步内容落地（回答/错误）—— 置位挡住"键盘增量帧"那条捷径：它的判据
  // 只看键盘目标串变没变，看不见上半屏的正文/状态行变化（见 rdRequestWholeRepaint）。
  rdRequestWholeRepaint();
  if (st.mode != RdMode::AiChat) return false;   // 已经离开这一屏：同上
  if (job->ok) {
    if (!st.aiTurns.empty()) st.aiTurns.back().a = job->answer;
    st.aiStatus.clear();
  } else {
    // 失败就把这一轮**撤掉**、问题放回输入框：用户按回车就能重试，不用重打一遍。
    if (!st.aiTurns.empty()) {
      st.aiInput = st.aiTurns.back().q;
      st.aiTurns.pop_back();
    }
    st.aiStatus = job->error.empty() ? std::string("请求失败") : job->error;
  }
  s_aiCancel.store(false, std::memory_order_release);
  st.aiScrollToEnd = true;
  aiRebuildTranscript();
  st.dirty = 1;
  return true;
}

// 输入框矩形与命中**必须同源**（同 dictQueryTop 那段注释）：渲染用 drawTitle 的返回值
// 当 qy，命中用这里；两者相等是约定，所以这里直接取 coverTop()。
static int aiInputTop() { return coverTop(); }

// ── 渲染 ─────────────────────────────────────────────────────────────────
void renderAiChat() {
  aiPollResult();   // 渲染时也取件（三处缺一，就是"答案到了屏上还在转圈"）
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  // 无 ctx（应用页入口）的标题跟着入口名走：入口页那格叫「DeepSeek 对话」，
  // 进来看到别的名字会以为是两个东西。有 ctx 那版仍是「问 AI」（从选区动作条来的）。
  const int top = drawTitle(st.aiCtx.empty() ? "DeepSeek 对话" : "问 AI");
  const int qy = top;
  const int qh = dictQueryH();
  g_rd.drawRect(MARGIN, qy, w - 2 * MARGIN, qh, true);
  {
    const std::string raw = st.aiInput.empty() ? std::string("输入问题后回车发送") : st.aiInput;
    const std::string disp = g_rd.truncatedText(uiFontId(), raw.c_str(), w - 2 * MARGIN - 16);
    drawLineText(MARGIN + 8, qy + 6, disp.c_str(), true);
  }
  int ry = qy + qh + 12;
  const int bottom = rdBodyBottom();
  if (!st.aiStatus.empty()) {
    drawLineText(MARGIN, ry, st.aiStatus.c_str(), true);
    ry += uiLineHeight() + 6;
  }
  if (!st.aiTranscript.empty()) {
    const int maxW = w - 2 * MARGIN;
    // 折行只算一次：宽度没变就复用上一帧的行表（多轮正文逐帧重新量宽度会拖慢滚动/打字）。
    if (st.aiLinesW != maxW) {
      st.aiLinesW = maxW;
      st.aiLines = g_rd.wrappedText(uiFontId(), st.aiTranscript.c_str(), maxW, kAiMaxLines);
    }
    const int lineH = uiLineHeight() + 4;
    const int maxLines = std::max(1, (bottom - ry) / lineH);
    const int total = static_cast<int>(st.aiLines.size());
    if (st.aiScrollToEnd) {
      st.aiScroll = std::max(0, total - maxLines);
      st.aiScrollToEnd = false;
    }
    // 夹住首行偏移：翻页键只管加，上限在这里兜底（与词典/脚注同一套做法）。
    const int first = std::max(0, std::min(st.aiScroll, std::max(0, total - maxLines)));
    st.aiScroll = first;
    st.aiScrollMax = std::max(0, total - maxLines);
    st.aiPageLines = maxLines;
    const int last = std::min(total, first + maxLines);
    int y = ry;
    for (int i = first; i < last; i++) {
      drawLineText(MARGIN, y, st.aiLines[i].c_str(), true);
      y += lineH;
    }
    // 翻页指示：只有真看不全时才画，且只在状态行占着那一行时画（同 renderDict 的做法；
    // 状态行空着时那一行是输入框底边，画上去就是压框）。
    if (total > maxLines && !st.aiStatus.empty()) {
      char ind[24];
      const int pageCount = (total + maxLines - 1) / maxLines;
      snprintf(ind, sizeof(ind), "%d/%d", first / maxLines + 1, pageCount);
      const int tw = g_rd.getTextWidth(uiFontId(), ind);
      drawLineText(w - MARGIN - tw, ry - uiLineHeight() - 6, ind, true);
    }
  }
  if (st.vkVisible) {
    drawVk();
  } else {
    drawRdImeBar();   // 蓝牙键盘打字：编码行 + 候选行（见 drawRdImeBar）
    drawFooter(st.aiScrollMax > 0 ? "上下划翻页  点输入框输入  Enter 发送  Esc 返回"
                                  : "点输入框输入  Enter 发送  Esc 返回");
  }
  rdDrawVkIcon();   // **必须最后画**（面板收起时它才露出来，画在前面会被面板盖掉）
}

// ── 提交 ─────────────────────────────────────────────────────────────────
void rdAiChatSubmit() {
  if (st.aiBusy) return;
  std::string q = st.aiInput;
  trimAsciiInPlace(q);
  if (q.empty()) return;
  if (q.size() > kAiMaxQuestion) {
    st.aiStatus = "问题过长（最多 1000 字节）";
    st.dirty = 1;
    return;
  }
  // 上一条还在飞：**不能**再起一个（两个 worker 同时取消/落地会互相踩）。
  // 正常路径走不到这儿 —— 在飞时按键已经被 handleAiChat 吞了；这一句是给"刚被 Esc
  // 取消、worker 还没退干净"那不到一秒的窗口兜底。
  if (s_aiRunning.load(std::memory_order_acquire)) {
    st.aiStatus = "上一条还没结束，请稍候";
    st.dirty = 1;
    return;
  }

  // WiFi 由**主任务**负责连：readerEnsureWifi 会写 st.busyMsg 并自己 renderCurrent()，
  // 而 worker 不许碰 st、不许渲染（全仓铁律）。
  if (!g_wifi.isConnected()) {
    rdShowBusy("正在连接 WiFi…", "");
    std::string werr;
    if (!readerEnsureWifi(werr)) {
      st.aiStatus = werr.empty() ? std::string("WiFi 连接失败") : werr;
      st.dirty = 1;
      return;
    }
  }

  // 先把这一轮放进历史：用户立刻看见自己的问题 + 一句"正在思考…"（体感上是"发出去了"，
  // 而不是"按了回车屏幕没变"）。
  st.aiTurns.push_back({q, std::string()});
  if (static_cast<int>(st.aiTurns.size()) > kAiMaxTurns) st.aiTurns.erase(st.aiTurns.begin());
  st.aiInput.clear();
  st.aiScrollToEnd = true;
  aiRebuildTranscript();
  st.dirty = 1;

  std::unique_ptr<RdAiJob> job(new (std::nothrow) RdAiJob());
  if (!job) {
    st.aiTurns.pop_back();
    st.aiInput = q;
    st.aiStatus = "内存不足，请重试";
    aiRebuildTranscript();
    return;
  }
  job->gen = s_aiGen;
  job->ctx = st.aiCtx;
  job->question = q;
  job->history = st.aiTurns;   // 快照：worker 不许引用 st

  s_aiCancel.store(false, std::memory_order_release);
  s_aiRunning.store(true, std::memory_order_release);
  TaskHandle_t h = nullptr;
  // 栈用 **7680 不是 8192**：xTaskCreate 的栈只能从内部 RAM 出，而我们是**先连上 WiFi
  // 再起任务**的，那时最大连续块正好 8192 —— 申请 8192 会因为块头差一点而每次都失败
  // （screen_settings.cpp 记着实测 free=18415 largest=8192；Flomo 的 TLS 登录也是从 6144
  // 崩过之后定到 7680）。polish/prompt_gen 用 8192 没事，是因为它们的任务自己才去连 WiFi。
  if (xTaskCreate(rdAiTask, "ai_chat", kAiTaskStack, job.get(), 1, &h) != pdPASS) {
    s_aiRunning.store(false, std::memory_order_release);
    st.aiTurns.pop_back();
    st.aiInput = q;
    st.aiStatus = "系统繁忙，请重试";
    aiRebuildTranscript();
    return;
  }
  (void)job.release();
  st.aiBusy = true;
  st.aiStatus = "正在思考…（Esc 取消）";
}

// ── 按键 ─────────────────────────────────────────────────────────────────
void handleAiChat(int key) {
  if (aiPollResult()) st.dirty = 1;   // 按键帧也取件（同 handleFileBrowser / handleDict）
  if (key == 0x1B) {
    if (st.aiBusy) {
      // 取消是**请求**不是立即生效：net::Request 以 1s 粒度轮询取消旗（见 net/http.h），
      // 所以先报"正在取消…"，真正落地由 aiPollResult 写回（那时问题会回到输入框）。
      s_aiCancel.store(true, std::memory_order_release);
      st.aiStatus = "正在取消…";
      st.dirty = 1;
      return;
    }
    IME::getInstance().cancelComposition();
    st.vkVisible = false;
    // 两个入口各自的归处：应用页进来的回应用页，阅读页进来的回阅读页（同 handleDict 的三元）。
    st.mode = (st.retMode == RdMode::Apps) ? RdMode::Apps : RdMode::Reading;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  // 在飞时除 Esc 外一律吞掉：不吞的话上下/翻页会改滚动位置、回车会再起一个请求。
  if (st.aiBusy) return;
  // 滚动：单行用 KEY_UP/DOWN（BLE/方向键），整屏用 KEY_PAGE_UP/DOWN（触摸竖滑在这里
  // 不会被展平成单步 —— "回退成单步"的名单里没有 AiChat，见 screen_reader_handle）。
  // 上限/步长都用渲染时算出的值，键盘没画过的那一帧也有兜底 1。
  if (key == KEY_UP) {
    st.aiScroll = std::max(0, st.aiScroll - 1);
    st.dirty = 1;
    return;
  }
  if (key == KEY_DOWN) {
    st.aiScroll = std::min(st.aiScrollMax, st.aiScroll + 1);
    st.dirty = 1;
    return;
  }
  if (key == KEY_PAGE_UP) {
    st.aiScroll = std::max(0, st.aiScroll - std::max(1, st.aiPageLines));
    st.dirty = 1;
    return;
  }
  if (key == KEY_PAGE_DOWN) {
    st.aiScroll = std::min(st.aiScrollMax, st.aiScroll + std::max(1, st.aiPageLines));
    st.dirty = 1;
    return;
  }
  if (key == '\n') {
    int x = 0, y = 0;
    if (input_tap_xy(&x, &y)) {
      if (rdVkIconHit(x, y)) {
        rdToggleVk();
      } else if (y >= aiInputTop() && y < aiInputTop() + dictQueryH()) {
        rdVkWantShow();   // 点到输入框：展开键盘（蓝牙键盘连着时给一次浮动提示）
      } else if (st.vkVisible) {
        vkTap(x, y);
      }
      st.dirty = 1;
      return;
    }
    vkEnter();   // 非点按回车（BLE/KEY3）→ vkEnter 里分派到 rdAiChatSubmit
    st.dirty = 1;
    return;
  }
  if (key == 0x08) {
    feedVkBackspace();
    st.dirty = 1;
    return;
  }
  if (key >= 0x20 && key <= 0x7E) {
    feedVkKey(key);
    st.dirty = 1;
    return;
  }
}

// ── 空闲帧 ───────────────────────────────────────────────────────────────
// 空闲帧是唯一"没人按键"的节拍源，所以答案落地主要靠它。取到件时必须当场重绘：
// 空闲帧没有帧末统一推屏，只标脏等于没画（同 rdFileScanIdleTick 那段注释）。
bool rdAiChatIdleTick() { return aiPollResult(); }

// ── 开场（两个入口共用）──────────────────────────────────────────────────
bool rdAiChatBegin(const std::string &ctx, RdMode from) {
  if (g_settings.deepseekKey().empty()) {
    // 不进屏、也不清选区：阅读页那边浮层还在，用户还能改点别的动作。
    rdShowFloat("未配置 DeepSeek Key", "先到 设置 → AI 里填写", 4000);
    return false;
  }
  aiPollResult();   // 清一次场（上一场可能刚落地或还没人取）
  ++s_aiGen;        // 新一场对话：旧一代的结果就算落地也丢弃（见 RdAiJob::gen）
  // 只有确定没有 worker 在跑时才清取消旗 —— 否则会把上一条正被取消的请求"复活"。
  if (!s_aiRunning.load(std::memory_order_acquire)) s_aiCancel.store(false, std::memory_order_release);

  st.aiCtx = limitUtf8(ctx, kAiMaxCtx);
  st.aiInput.clear();
  st.aiStatus.clear();
  st.aiTurns.clear();
  st.aiBusy = false;
  // 一进来贴着顶：有选段时最该看见的就是那段原文（它是这一屏的由头）。
  st.aiScroll = 0;
  st.aiScrollMax = 0;
  st.aiPageLines = 1;
  st.aiScrollToEnd = false;
  aiRebuildTranscript();

  st.retMode = from;   // 每次开场都覆盖，所以残留的旧值无害
  st.mode = RdMode::AiChat;
  st.fullRefresh = true;
  rdVkWantShow();      // 进来就是要打字的：直接弹键盘
  st.dirty = 1;
  ESP_LOGI(TAG, "begin from=%d ctx=%d bytes", static_cast<int>(from), static_cast<int>(st.aiCtx.size()));
  return true;
}

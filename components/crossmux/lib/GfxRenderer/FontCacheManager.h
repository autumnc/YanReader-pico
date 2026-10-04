#pragma once

// 阅读模式使用 ttf_font 单例，无 SD 卡字体缓存扫描；保留最小接口以兼容 kept lib。
class FontCacheManager {
 public:
  FontCacheManager() = default;
  ~FontCacheManager() = default;

  bool isScanning() const { return false; }
  void releaseSdFontCaches() {}
};

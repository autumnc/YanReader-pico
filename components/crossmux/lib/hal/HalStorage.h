#pragma once

#include <Print.h>
#include <common/FsApiConstants.h>  // for oflag_t

#include <cstdint>
#include <memory>
#include <string>

class HalFile;

enum class UsbDriveState : uint8_t {
  Unsupported,
  WaitingForHost,
  Connected,
  Ejected,
  Disconnected,
  IoError,
};

// SD 卡存储 HAL：基于 stdio（newlib VFS），挂载点 /sdcard 由 main 负责。
class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  bool getSpace(uint64_t& totalBytes, uint64_t& freeBytes);
  bool cardDetectAsserted() const;
  void prepareForDeepSleep();
  // USB Drive 独占 SD 卡，阅读模式首版不支持，一律返回 Unsupported/false。
  bool beginUsbDrive();
  bool disconnectUsbDriveHost();
  void endUsbDrive();
  UsbDriveState usbDriveState() const;

  HalFile open(const char* path, const oflag_t oflag = FS_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);
  bool removeDir(const char* path);

  // 递归建目录（微信读书的缓存层级较深）。已存在视为成功。
  bool ensureDirectoryExists(const char* path) { return mkdir(path, true); }
  // 读整个小文件（返回内容；失败返回空串，*ok 区分"空文件"和"读失败"）。
  std::string readFile(const char* path, bool* ok = nullptr);
  // 整体覆写一个文件。
  bool writeFile(const char* path, const std::string& content);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);

  static HalStorage& getInstance() { return instance; }

  // 当前仍打开的 SD 文件数 = FATFS VFS 占用的槽位数。挂载时 max_files 用满后
  // f_open 会返回 ENFILE（"SD 卡读写失败"），这个计数让日志能直接看出是否逼近上限。
  static int liveFileCount();

  // 阅读模式为单任务访问 SD，newlib 的 stdio 自带每文件锁；保留 RAII 接口占位。
  class StorageLock {
   public:
    StorageLock();
    ~StorageLock();
    StorageLock(const StorageLock&) = delete;
    StorageLock& operator=(const StorageLock&) = delete;
  };

 private:
  static HalStorage instance;
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  explicit HalFile(std::unique_ptr<Impl> impl);

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&) noexcept;
  HalFile& operator=(HalFile&&) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const uint8_t* buf, size_t count) override;
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  bool rename(const char* newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const;
  operator bool() const;
};

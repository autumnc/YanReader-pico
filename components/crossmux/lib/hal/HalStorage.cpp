#include "HalStorage.h"

#include <Logging.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "esp_vfs_fat.h"
#else
#include <sys/statvfs.h>
#endif

// SD 挂载点（与 pjournal 主组件一致）。
static constexpr const char* SD_ROOT = "/sdcard";

// crossmux 路径 → VFS 原生态路径。
static std::string toNativePath(const char* path) {
  if (!path || !*path) return SD_ROOT;
  std::string p(path);
  if (p.rfind(SD_ROOT, 0) == 0) return p;            // 已是 /sdcard/...
  if (p[0] == '/') return std::string(SD_ROOT) + p;  // 绝对路径 → SD 根下
  return std::string(SD_ROOT) + "/" + p;             // 相对路径 → SD 根下
}

// oflag → fopen 模式。
static const char* toFopenMode(oflag_t oflag) {
  const bool rd = (oflag & FS_RDONLY) || (oflag & FS_RDWR);
  const bool wr = (oflag & FS_WRONLY) || (oflag & FS_RDWR);
  if (oflag & FS_APPEND) return wr ? "a+b" : "ab";
  if (rd && wr) return (oflag & (FS_CREAT | FS_TRUNC)) ? "w+b" : "r+b";
  if (wr) return "wb";
  return "rb";
}

// 仍处于打开状态的 SD 文件数（FATFS VFS 槽位占用）。见 HalStorage::liveFileCount()。
static int s_liveFiles = 0;

int HalStorage::liveFileCount() { return s_liveFiles; }

// HalFile 的 pimpl：必须在 HalStorage::open 之前定义，保证 new HalFile::Impl(...) 时类型完整。
// 同一个 Impl 既可能是个文件（fp），也可能是个目录（dp）——crossmux 里
// DictionaryRegistry / WeReadStore 都会 `Storage.open(某个目录)` 再 openNextFile() 枚举，
// 而上游是 SdFat 的 FsFile，目录和文件是同一个类型。移植时只实现了文件那一半，
// isDirectory() 直接 return false、openNextFile() 直接 return {}，
// **结果就是字典目录永远枚举不出 .idx → 发现不了任何词典 → 词典功能整体不可用**
// （日志表现：STORAGE: open failed: /sdcard/dictionaries/<名字> (errno=2)，其实那是
// 想用 fopen 打开目录被 FatFS 拒了）。这里把目录那一半补齐。
class HalFile::Impl {
 public:
  Impl(FILE* fp, std::string path) : fp(fp), path(std::move(path)) {}
  Impl(DIR* dp, std::string path) : dp(dp), path(std::move(path)), isDir(true) {}
  ~Impl() {
    if (fp) {
      fclose(fp);
      --s_liveFiles;   // 与 open() 里的 ++ 配对（close() 已置空 fp 时不会重复减）
    }
    if (dp) closedir(dp);
  }
  FILE* fp = nullptr;
  DIR* dp = nullptr;   // 非空 = 这是个可枚举的目录（此时 fp 恒为空，不占 VFS 槽位）
  std::string path;
  bool isDir = false;
  // openNextFile() 造出来的"目录项"（只有路径，没有打开句柄）。isOpen() 认它，
  // 否则枚举循环会在第一个普通文件处收摊（见 isOpen 的注释）。
  bool isEntry = false;
};

HalStorage HalStorage::instance;

HalStorage::HalStorage() {}
bool HalStorage::begin() { return true; }  // SD 由 main 挂载
bool HalStorage::ready() const { return true; }
bool HalStorage::cardDetectAsserted() const { return true; }
void HalStorage::prepareForDeepSleep() {}
bool HalStorage::beginUsbDrive() { return false; }
bool HalStorage::disconnectUsbDriveHost() { return false; }
void HalStorage::endUsbDrive() {}
UsbDriveState HalStorage::usbDriveState() const { return UsbDriveState::Unsupported; }

bool HalStorage::getSpace(uint64_t& totalBytes, uint64_t& freeBytes) {
#ifdef ESP_PLATFORM
  return esp_vfs_fat_info(SD_ROOT, &totalBytes, &freeBytes) == ESP_OK;
#else
  struct statvfs st;
  if (statvfs(SD_ROOT, &st) != 0) return false;
  totalBytes = static_cast<uint64_t>(st.f_bsize) * st.f_blocks;
  freeBytes = static_cast<uint64_t>(st.f_bsize) * st.f_bavail;
  return true;
#endif
}

HalStorage::StorageLock::StorageLock() {}
HalStorage::StorageLock::~StorageLock() {}

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  std::string native = toNativePath(path);
  // 目录要单独认：f_open 对目录的成败在各版 FatFS 上不一致（有的返回 FR_NO_FILE=ENOENT，
  // 有的干脆当空文件打开），猜不得。**只在没有写标志的调用上**先 stat 一次——目录只可能
  // 被只读打开（crossmux 拿它来枚举），写路径保持原来的一次 fopen，不额外付这次 stat。
  const bool readOnly = !(oflag & (FS_WRONLY | FS_RDWR | FS_CREAT | FS_TRUNC | FS_APPEND));
  if (readOnly) {
    struct stat st;
    if (::stat(native.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      DIR* dp = opendir(native.c_str());
      if (!dp) {
        LOG_ERR("STORAGE", "opendir failed: %s (errno=%d %s)", native.c_str(), errno,
                strerror(errno));
        return {};
      }
      std::unique_ptr<HalFile::Impl> dirImpl(new (std::nothrow) HalFile::Impl(dp, native));
      if (!dirImpl) {
        closedir(dp);
        return {};
      }
      return HalFile(std::move(dirImpl));   // 目录不占 VFS 文件槽位，不动 s_liveFiles
    }
  }
  FILE* fp = fopen(native.c_str(), toFopenMode(oflag));
  if (!fp) {
    // live=xx/24：挂载时的 max_files。ENFILE(23) 表示槽位全满（"SD 卡读写失败"的
    // 真因是句柄没关，而不是卡坏了），live 会直接指到还剩几个槽位。
    LOG_ERR("STORAGE", "open failed: %s (errno=%d %s) live=%d", native.c_str(), errno, strerror(errno), s_liveFiles);
    return {};
  }
  std::unique_ptr<HalFile::Impl> impl(new (std::nothrow) HalFile::Impl(fp, native));
  if (!impl) {
    fclose(fp);
    return {};
  }
  ++s_liveFiles;
  return HalFile(std::move(impl));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
  std::string native = toNativePath(path);
  if (!pFlag) return ::mkdir(native.c_str(), 0777) == 0;
  std::string cur;
  size_t pos = 0;
  while (pos < native.size()) {
    size_t slash = native.find('/', pos);
    std::string seg = native.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
    if (!seg.empty()) {
      cur += "/" + seg;
      struct stat st;
      if (stat(cur.c_str(), &st) == 0) {
        // 已存在的路径段（含挂载点 /sdcard）跳过：对挂载点 mkdir 可能返回
        // 非 EEXIST 的错误（如 ENOENT/EINVAL），直接据此判失败会中断递归。
      } else if (::mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) {
        return false;
      }
    }
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }
  return true;
}

bool HalStorage::exists(const char* path) {
  struct stat st;
  return stat(toNativePath(path).c_str(), &st) == 0;
}
bool HalStorage::remove(const char* path) { return ::remove(toNativePath(path).c_str()) == 0; }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  return ::rename(toNativePath(oldPath).c_str(), toNativePath(newPath).c_str()) == 0;
}
bool HalStorage::rmdir(const char* path) { return ::rmdir(toNativePath(path).c_str()) == 0; }

static bool removeDirRecursive(const std::string& path) {
  DIR* d = opendir(path.c_str());
  if (!d) return false;
  struct dirent* e;
  bool ok = true;
  while ((e = readdir(d)) != nullptr) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    std::string child = path + "/" + e->d_name;
    struct stat st;
    if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      if (!removeDirRecursive(child)) ok = false;
    } else {
      if (::remove(child.c_str()) != 0) ok = false;
    }
  }
  closedir(d);
  if (::rmdir(path.c_str()) != 0) ok = false;
  return ok;
}
bool HalStorage::removeDir(const char* path) { return removeDirRecursive(toNativePath(path)); }

std::string HalStorage::readFile(const char* path, bool* ok, size_t maxBytes) {
  if (ok) *ok = false;
  HalFile f = open(path, FS_RDONLY);
  if (!f.isOpen()) return std::string();
  const size_t size = f.fileSize();
  if (maxBytes > 0 && size > maxBytes) {
    LOG_ERR("STORAGE", "readFile refused oversized file: %s (%u > %u)", path,
            static_cast<unsigned>(size), static_cast<unsigned>(maxBytes));
    return std::string();
  }
  std::string out(size, '\0');
  if (size > 0 && f.read(&out[0], size) != static_cast<int>(size)) return std::string();
  f.close();
  if (ok) *ok = true;
  return out;
}

bool HalStorage::writeFile(const char* path, const std::string& content) {
  HalFile f = open(path, FS_RDWR | FS_CREAT | FS_TRUNC);
  if (!f.isOpen()) return false;
  const bool ok = content.empty() || f.write(content.data(), content.size()) == content.size();
  f.close();
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  file = open(path, FS_RDONLY);
  if (!file.isOpen()) {
    LOG_ERR(moduleName, "openFileForRead failed: %s", path);
    return false;
  }
  return true;
}
bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  // 必须是 O_RDWR 而不是 O_WRONLY：crossmux 大量依赖「写一半再读回来」。
  // Section 增量排版就是典型——边把页写进 <spine>.bin.part，边用同一个句柄回读
  // 已写出的页给用户看（Section::loadPageDuringBuild）。上游用的是 SdFat 的
  // FILE_WRITE，它本就是 O_RDWR|O_CREAT|O_TRUNC；这里若用 O_WRONLY，fopen 得到
  // "wb"，fread 在其上直接失败 → 回读永远 nullptr → **排版期间整页空白**
  // （读完整章后才从文件走 loadPageAt 正常）。truncate 语义不变。
  // / Must be O_RDWR, not O_WRONLY: crossmux reads back what it just wrote
  // (incremental pagination). SdFat's FILE_WRITE was O_RDWR|O_CREAT|O_TRUNC.
  file = open(path, FS_RDWR | FS_CREAT | FS_TRUNC);
  if (!file.isOpen()) {
    LOG_ERR(moduleName, "openFileForWrite failed: %s", path);
    return false;
  }
  return true;
}
bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

// ---- HalFile ----

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) noexcept = default;
HalFile& HalFile::operator=(HalFile&&) noexcept = default;

void HalFile::flush() {
  if (impl && impl->fp) fflush(impl->fp);
}

size_t HalFile::getName(char* name, size_t len) {
  if (!impl || len == 0) return 0;
  const std::string& p = impl->path;
  size_t slash = p.find_last_of('/');
  std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
  size_t n = base.size() < len - 1 ? base.size() : len - 1;
  memcpy(name, base.c_str(), n);
  name[n] = 0;
  return n;
}

size_t HalFile::size() {
  if (!impl || !impl->fp) return 0;
  long cur = ftell(impl->fp);
  fseek(impl->fp, 0, SEEK_END);
  long sz = ftell(impl->fp);
  fseek(impl->fp, cur, SEEK_SET);
  return sz > 0 ? static_cast<size_t>(sz) : 0;
}
size_t HalFile::fileSize() { return size(); }
uint64_t HalFile::fileSize64() { return static_cast<uint64_t>(size()); }

bool HalFile::seek(size_t pos) {
  return impl && impl->fp && fseek(impl->fp, static_cast<long>(pos), SEEK_SET) == 0;
}
bool HalFile::seek64(uint64_t pos) {
  return impl && impl->fp && fseek(impl->fp, static_cast<long>(pos), SEEK_SET) == 0;
}
bool HalFile::seekCur(int64_t offset) {
  return impl && impl->fp && fseek(impl->fp, static_cast<long>(offset), SEEK_CUR) == 0;
}
bool HalFile::seekSet(size_t offset) { return seek(offset); }

int HalFile::available() const {
  if (!impl || !impl->fp) return 0;
  long cur = ftell(impl->fp);
  if (cur < 0) return 0;
  fseek(impl->fp, 0, SEEK_END);
  long end = ftell(impl->fp);
  fseek(impl->fp, cur, SEEK_SET);
  return static_cast<int>(end - cur);
}
size_t HalFile::position() const {
  if (!impl || !impl->fp) return 0;
  long p = ftell(impl->fp);
  return p > 0 ? static_cast<size_t>(p) : 0;
}

int HalFile::read(void* buf, size_t count) {
  if (!impl || !impl->fp) return 0;
  return static_cast<int>(fread(buf, 1, count, impl->fp));
}
int HalFile::read() {
  if (!impl || !impl->fp) return -1;
  return fgetc(impl->fp);
}
size_t HalFile::write(const uint8_t* buf, size_t count) {
  if (!impl || !impl->fp) return 0;
  return fwrite(buf, 1, count, impl->fp);
}
size_t HalFile::write(const void* buf, size_t count) {
  return write(static_cast<const uint8_t*>(buf), count);
}
size_t HalFile::write(uint8_t b) {
  if (!impl || !impl->fp) return 0;
  // 必须是 fwrite，不能是 fputc —— picolibc 的 stdio 两条写路径对"缓冲区正好写满"处理不一致，
  // 混用会把 stdio 缓冲区写越界一个字节：
  //   fwrite：只在循环还要继续时才 flush，所以"这一笔正好填满 128 字节"会带着 len == size
  //           返回（留到下一次 fwrite 入口才 flush，那时是安全的）；
  //   __bufio_put（fputc/putc/fputs/fprintf 走的路径）：先写后判 ——
  //           `buf[len++] = c; if (len >= size) flush;`
  // 于是"fwrite 刚好填满 + 紧接着 fputc"会写 buf[128]，越过 128 字节的堆块一个字节，踩坏
  // 相邻堆块头。整本重新缓存到"生成图书"那步随机崩溃重启，就是这个。
  // 本机只有微信读书的 nav.part 会混用两条路径（writeLiteral 走 fwrite、writeXmlText 逐字节
  // 走 fputc，两者交替），也只有"本机已有这本书"时字节数才对得上（新书对不齐就不触发）。
  // 1 字节写并入 fwrite 后，一个 HalFile 只剩一条写路径。
  return fwrite(&b, 1, 1, impl->fp);
}

bool HalFile::rename(const char* newPath) {
  if (!impl) return false;
  std::string np = toNativePath(newPath);
  if (::rename(impl->path.c_str(), np.c_str()) != 0) return false;
  impl->path = np;
  return true;
}
bool HalFile::isDirectory() const { return impl && impl->isDir; }
void HalFile::rewindDirectory() {
  if (impl && impl->dp) rewinddir(impl->dp);
}
bool HalFile::close() {
  if (!impl) return false;
  bool did = false;
  if (impl->fp) {
    fclose(impl->fp);
    impl->fp = nullptr;
    --s_liveFiles;   // 显式关闭也要减：析构时 fp 已空，不会重复减
    did = true;
  }
  if (impl->dp) {
    closedir(impl->dp);
    impl->dp = nullptr;
    did = true;
  }
  return did;
}

// 枚举目录里的下一项（跳过 . 和 ..）。返回的是"这一项自己"的 HalFile：文件项只带路径
// （没有打开句柄，所以不占 VFS 槽位，调用方要读内容得自己再 open 一次——crossmux
// 的调用点都只看名字和 isDirectory()）；子目录项带目录标志。
// 是否目录用 stat 现场判定，不靠 dirent.d_type（FatFS 不保证填它）。
HalFile HalFile::openNextFile() {
  if (!impl || !impl->dp) return {};
  for (;;) {
    struct dirent* e = readdir(impl->dp);
    if (!e) return {};
    const char* nm = e->d_name;
    if (!nm || !nm[0]) continue;
    if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
    std::string child = impl->path + "/" + nm;
    std::unique_ptr<Impl> ci(new (std::nothrow) Impl(static_cast<FILE*>(nullptr), child));
    if (!ci) return {};
    ci->isEntry = true;   // 目录项本身是"有效"的，见 isOpen()
    struct stat st;
    if (::stat(child.c_str(), &st) == 0) ci->isDir = S_ISDIR(st.st_mode) != 0;
    return HalFile(std::move(ci));
  }
}

// "这个 HalFile 指向的东西有效吗"。目录项也算有效——否则
// `for (auto e = dir.openNextFile(); e; ...)` 遇到它就会提前收摊。
//
// **文件项也必须算有效**：openNextFile() 返回的文件项只有路径、没有打开句柄（fp 恒空），
// 早先的判据是 `fp || isDir`，于是"枚举到第一个普通文件就收摊"——调用方的循环体一次都
// 不执行。症状不是报错而是一片空白：DictionaryRegistry::findStem 永远看不到 *.idx
// （entries=[]），resolveBasePath 永远失败，词典功能整体不可用（"未找到词典"），
// 而用户把三件套摆得再对也没用。见 [[crossmux-dict-assets]]。
bool HalFile::isOpen() const { return impl && (impl->fp != nullptr || impl->dp != nullptr || impl->isEntry); }
HalFile::operator bool() const { return isOpen(); }

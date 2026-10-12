// 主机测试的替身。被测的是 weread_skill_client.cpp 里的**纯函数**
// （buildGatewayBody / parseSearch / parseRecommend），但静态链接把整个 .o 拉进来，
// 同一个 .o 里的 gateway()/search()/recommend() 会引用 g_settings.getString 和
// net::request —— 这里给空实现，测试从不调用它们。flomo_json.cpp 的
// saveToFile 引用 flomoSafeWriteFile，也一并补上。
#include "net/http.h"
#include "settings_manager.h"
#include "flomo_file.h"

#include <string>

SettingsManager g_settings;

// 测例不经过网络路径，getString 给个空串即可。
std::string SettingsManager::getString(const std::string &, const std::string &def) { return def; }

// 网络：永远返回"传输失败"，纯函数测例不会走到这里。
namespace net {
Response request(const Request &) {
    Response r;
    r.err = -1;  // 非 ESP_OK
    return r;
}
}  // namespace net

// flomo_json.cpp 的 loadFromFile/saveToFile 走这几条；测例从不读写文件。
std::string flomoReadWholeFile(const std::string &) { return {}; }
bool flomoFileExists(const std::string &) { return false; }
bool flomoEnsureDir(const std::string &) { return true; }
bool flomoSafeWriteFile(const std::string &, const std::string &) { return true; }
void flomoRepairFile(const std::string &) {}

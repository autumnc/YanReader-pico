#include "webdav_client.h"
#include "journal_storage.h"
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <sys/stat.h>
#include <utime.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>

static const char *TAG = "WebDAV";
WebDavClient g_webdav;

// Simple XML parser for PROPFIND multi-status responses
// Finds all <d:response> elements and extracts href, getlastmodified
struct PropfindEntry {
    std::string href;
    std::string lastModified;
    bool isCollection = false;
};

static std::vector<PropfindEntry> parsePropfindResponse(const std::string &xml) {
    std::vector<PropfindEntry> entries;
    // Find each <d:response> block
    const char *p = xml.c_str();
    while (true) {
        const char *respStart = strstr(p, "<d:response>");
        if (!respStart) {
            respStart = strstr(p, "<response>");
            if (!respStart) break;
        }
        const char *respEnd = strstr(respStart, "</d:response>");
        if (!respEnd) {
            respEnd = strstr(respStart, "</response>");
            if (!respEnd) break;
        }
        std::string block(respStart, respEnd - respStart + 14);

        PropfindEntry entry;

        // href
        auto hrefTag = block.find("<d:href>");
        if (hrefTag == std::string::npos) hrefTag = block.find("<href>");
        if (hrefTag != std::string::npos) {
            auto hrefStart = block.find('>', hrefTag) + 1;
            auto hrefEnd = block.find("</", hrefStart);
            if (hrefEnd != std::string::npos) {
                entry.href = block.substr(hrefStart, hrefEnd - hrefStart);
            }
        }

        // collection check
        if (block.find("<d:collection") != std::string::npos ||
            block.find("<collection") != std::string::npos) {
            entry.isCollection = true;
        }

        // getlastmodified
        auto lmTag = block.find("<d:getlastmodified>");
        if (lmTag == std::string::npos) lmTag = block.find("<getlastmodified>");
        if (lmTag != std::string::npos) {
            auto lmStart = block.find('>', lmTag) + 1;
            auto lmEnd = block.find("</", lmStart);
            if (lmEnd != std::string::npos) {
                entry.lastModified = block.substr(lmStart, lmEnd - lmStart);
            }
        } else {
            // Try <d:getlastmodified xmlns="DAV:">
            lmTag = block.find("getlastmodified");
            if (lmTag != std::string::npos) {
                auto lmStart = block.find('>', lmTag) + 1;
                auto lmEnd = block.find("</", lmStart);
                if (lmEnd != std::string::npos) {
                    entry.lastModified = block.substr(lmStart, lmEnd - lmStart);
                }
            }
        }

        entries.push_back(entry);
        p = respEnd + 1;
    }
    return entries;
}

WebDavClient::WebDavClient() {}
WebDavClient::~WebDavClient() {}

void WebDavClient::configure(const std::string &url, const std::string &username, const std::string &password) {
    baseUrl_ = url;
    username_ = username;
    password_ = password;
    if (!baseUrl_.empty() && baseUrl_.back() != '/') baseUrl_ += '/';
}

bool WebDavClient::isConfigured() const {
    return !baseUrl_.empty() && !username_.empty() && !password_.empty();
}

std::string WebDavClient::buildUrl(const std::string &path) const {
    if (path.empty() || path == "/") return baseUrl_;
    std::string url = baseUrl_;
    // Remove leading slash from path if base already ends with one
    if (!path.empty() && path[0] == '/') url += path.substr(1);
    else url += path;
    return url;
}

std::string WebDavClient::authHeader() const {
    // Basic auth: base64(username:password)
    std::string raw = username_ + ":" + password_;
    size_t outLen = 0;
    mbedtls_base64_encode(nullptr, 0, &outLen, (const uint8_t *)raw.data(), raw.size());
    std::string b64(outLen, '\0');
    mbedtls_base64_encode((uint8_t *)&b64[0], outLen, &outLen,
                          (const uint8_t *)raw.data(), raw.size());
    // Remove null terminator
    if (!b64.empty() && b64.back() == '\0') b64.pop_back();
    return "Basic " + b64;
}

// HTTP request helper: returns response body
static std::string httpRequest(const std::string &url, const std::string &method,
                                const std::string &auth, const std::string &body = "",
                                const std::string &contentType = "",
                                const std::string &extraHeader = "",
                                int *outStatusCode = nullptr) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = method == "GET" ? HTTP_METHOD_GET :
                 method == "PUT" ? HTTP_METHOD_PUT :
                 method == "DELETE" ? HTTP_METHOD_DELETE :
                 method == "MKCOL" ? HTTP_METHOD_MKCOL :
                 method == "HEAD" ? HTTP_METHOD_HEAD :
                 HTTP_METHOD_PROPFIND;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return "";

    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");
    if (!auth.empty()) esp_http_client_set_header(client, "Authorization", auth.c_str());
    if (!contentType.empty()) esp_http_client_set_header(client, "Content-Type", contentType.c_str());
    if (!extraHeader.empty()) esp_http_client_set_header(client, "Depth", extraHeader.c_str());

    std::string response;
    int status = 0;
    int bodyLen = (!body.empty() && method != "GET" && method != "HEAD") ? (int)body.size() : 0;
    esp_err_t err = esp_http_client_open(client, bodyLen);
    if (err == ESP_OK) {
        // Write request body for PUT/POST/etc
        if (bodyLen > 0) {
            int written = esp_http_client_write(client, body.c_str(), bodyLen);
            ESP_LOGI(TAG, "Written %d/%d bytes to request body", written, bodyLen);
        }

        int content_length = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %s %s status=%d content_length=%d",
                 method.c_str(), url.c_str(), status, content_length);

        // 读取响应体 (上限 2MB 防止内存耗尽)
        const size_t MAX_RESPONSE_SIZE = 2 * 1024 * 1024;
        if (status == 200 || status == 207 || content_length > 0) {  // PROPFIND 返回 207
            char buf[512];
            int len;
            int total_read = 0;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                if (response.size() + len > MAX_RESPONSE_SIZE) {
                    ESP_LOGW(TAG, "Response body exceeds 2MB limit, truncating");
                    break;
                }
                buf[len] = 0;
                response += buf;
                total_read += len;
                // 如果 content_length 为 -1（chunked），继续读取
                // 如果有明确长度，检查是否读完
                if (content_length > 0 && total_read >= content_length) break;
            }
            ESP_LOGI(TAG, "Total bytes read: %d", total_read);
        }
    } else {
        ESP_LOGW(TAG, "HTTP %s %s open failed: %d", method.c_str(), url.c_str(), err);
        status = -err;  // 使用负的错误码表示网络错误
    }

    esp_http_client_cleanup(client);
    if (outStatusCode) *outStatusCode = status;
    return response;
}

bool WebDavClient::ensureDirectory(const std::string &path) {
    if (!isConfigured()) return false;
    std::string url = buildUrl(path);
    std::string auth = authHeader();
    ESP_LOGI(TAG, "MKCOL request for: %s", url.c_str());
    int status = 0;
    httpRequest(url, "MKCOL", auth, "", "", "", &status);
    ESP_LOGI(TAG, "MKCOL status: %d", status);
    // 201=Created, 405=Already exists, 200/301/302=OK
    return (status == 201 || status == 200 || status == 405 || status == 301 || status == 302);
}

bool WebDavClient::upload(const std::string &remotePath, const std::string &content) {
    if (!isConfigured()) return false;
    std::string url = buildUrl(remotePath);
    std::string auth = authHeader();
    int status = 0;
    httpRequest(url, "PUT", auth, content, "text/plain; charset=utf-8", "", &status);
    return (status == 200 || status == 201 || status == 204);
}

std::string WebDavClient::download(const std::string &remotePath, int *outStatus) {
    if (outStatus) *outStatus = 0;
    if (!isConfigured()) return "";
    std::string url = buildUrl(remotePath);
    std::string auth = authHeader();
    int status = 0;
    auto body = httpRequest(url, "GET", auth, "", "", "", &status);
    if (outStatus) *outStatus = status;
    if (status == 200 || status == 203) return body;
    return "";
}

// A journal file name must be a plain basename: no path separators, no "..",
// and not a dotfile. Otherwise a malicious remote href (e.g. "%2E%2E%2F...")
// could escape the journal directory after URL decoding.
static bool isSafeJournalName(const std::string &name) {
    if (name.empty()) return false;
    if (name[0] == '.') return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    return true;
}

std::vector<std::pair<std::string, std::string>> WebDavClient::listFiles(const std::string &dirPath) {
    std::vector<std::pair<std::string, std::string>> result;
    if (!isConfigured()) return result;

    std::string url = buildUrl(dirPath);
    std::string auth = authHeader();

    const char *propfindBody =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\">"
        "<d:prop><d:getlastmodified/><d:resourcetype/></d:prop>"
        "</d:propfind>";

    int status = 0;
    auto body = httpRequest(url, "PROPFIND", auth, propfindBody,
                            "application/xml; charset=utf-8", "1", &status);

    ESP_LOGI(TAG, "PROPFIND status: %d, response length: %d", status, (int)body.length());
    if (status != 207 && status != 200) {
        ESP_LOGW(TAG, "PROPFIND failed with status %d", status);
        return result;
    }

    // Debug: log first 500 chars of response
    if (!body.empty()) {
        std::string preview = body.substr(0, std::min((size_t)500, body.length()));
        ESP_LOGI(TAG, "PROPFIND response preview:\n%s", preview.c_str());
    }

    auto entries = parsePropfindResponse(body);
    ESP_LOGI(TAG, "Parsed %d entries from PROPFIND", (int)entries.size());
    for (auto &e : entries) {
        if (e.isCollection) continue;
        // Extract filename from href
        auto slash = e.href.rfind('/');
        std::string fname;
        if (slash != std::string::npos) fname = e.href.substr(slash + 1);
        else fname = e.href;

        // URL decode the filename
        std::string decoded;
        for (size_t i = 0; i < fname.size(); i++) {
            if (fname[i] == '%' && i + 2 < fname.size()) {
                char hex[3] = {fname[i+1], fname[i+2], 0};
                decoded += (char)strtol(hex, nullptr, 16);
                i += 2;
            } else {
                decoded += fname[i];
            }
        }

        if (!isSafeJournalName(decoded)) continue;
        result.push_back({decoded, e.lastModified});
    }
    return result;
}

std::string WebDavClient::headFile(const std::string &remotePath) {
    if (!isConfigured()) return "";
    std::string url = buildUrl(remotePath);
    std::string auth = authHeader();
    int status = 0;
    httpRequest(url, "HEAD", auth, "", "", "", &status);
    if (status == 200 || status == 203) {
        return "exists";
    }
    return "";
}

bool WebDavClient::remove(const std::string &remotePath) {
    if (!isConfigured()) return false;
    std::string url = buildUrl(remotePath);
    std::string auth = authHeader();
    int status = 0;
    httpRequest(url, "DELETE", auth, "", "", "", &status);
    return (status == 200 || status == 204 || status == 404);
}

// ── Sync state and helpers ──────────────────────────────────────────

static bool isJournalFile(const std::string &name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    return ext == ".txt" || ext == ".md";
}

// Parse HTTP-date (e.g. "Thu, 01 Jan 2024 12:00:00 GMT") to time_t
static time_t parseWebdavDate(const std::string &s) {
    if (s.empty()) return 0;
    struct tm tm = {};
    const char *ret = strptime(s.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (!ret || *ret != '\0') return 0;
    // Temporarily switch to UTC so mktime treats tm as UTC
    char *oldTz = getenv("TZ");
    std::string savedTz = oldTz ? oldTz : "";
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(&tm);
    // Restore original timezone
    if (!savedTz.empty()) setenv("TZ", savedTz.c_str(), 1);
    else unsetenv("TZ");
    tzset();
    return t;
}

// Load sync state from /sdcard/settings/sync_state.txt
// Format: each line is "filename=unixtimestamp"
static std::map<std::string, time_t> loadSyncState() {
    std::map<std::string, time_t> state;
    FILE *f = fopen("/sdcard/settings/sync_state.txt", "r");
    if (!f) return state;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *endp;
        time_t ts = (time_t)strtoll(eq + 1, &endp, 10);
        state[line] = ts;
    }
    fclose(f);
    return state;
}

static void saveSyncState(const std::map<std::string, time_t> &state) {
    mkdir("/sdcard/settings", 0777);
    FILE *f = fopen("/sdcard/settings/sync_state.txt", "w");
    if (!f) return;
    for (auto &kv : state) {
        fprintf(f, "%s=%lld\n", kv.first.c_str(), (long long)kv.second);
    }
    fclose(f);
}

// ── Bidirectional sync ──────────────────────────────────────────────

SyncResult WebDavClient::sync(const std::string &localDir) {
    (void)localDir;
    if (!isConfigured()) {
        return {false, "请先在设置中配置 WebDAV"};
    }

    // Try to ensure remote journal directory exists, but don't fail if it doesn't work
    // (some WebDAV servers like Jianguoyun don't support MKCOL)
    std::string remoteDir = "journal/";
    if (!ensureDirectory(remoteDir)) {
        ESP_LOGW(TAG, "Failed to create remote journal/ directory, will try to list files anyway");
        // Don't return error, continue with sync
    }

    // Collect local files with mtimes
    auto localPairs = g_journal.listFileMtimes();
    ESP_LOGI(TAG, "Local files found: %d", (int)localPairs.size());
    std::map<std::string, time_t> localFiles;
    for (auto &p : localPairs) {
        localFiles[p.first] = p.second;
        ESP_LOGI(TAG, "  Local: %s (mtime=%lld)", p.first.c_str(), (long long)p.second);
    }

    // Collect remote files with mtimes
    ESP_LOGI(TAG, "Fetching remote file list from: %s", remoteDir.c_str());
    auto remoteList = listFiles(remoteDir);
    ESP_LOGI(TAG, "Remote files found: %d", (int)remoteList.size());
    std::map<std::string, time_t> remoteFiles;
    for (auto &rf : remoteList) {
        if (!isJournalFile(rf.first)) continue;
        remoteFiles[rf.first] = parseWebdavDate(rf.second);
        ESP_LOGI(TAG, "  Remote: %s (mtime=%lld)", rf.first.c_str(), (long long)remoteFiles[rf.first]);
    }

    // Load previous sync state
    auto prevState = loadSyncState();
    ESP_LOGI(TAG, "Previous sync state entries: %d", (int)prevState.size());

    // If no remote files found, try listing root directory
    if (remoteFiles.empty() && remoteDir != "") {
        ESP_LOGI(TAG, "No files in journal/, trying root directory...");
        auto rootList = listFiles("");
        ESP_LOGI(TAG, "Root directory files: %d", (int)rootList.size());
        for (auto &rf : rootList) {
            if (!isJournalFile(rf.first)) continue;
            remoteFiles[rf.first] = parseWebdavDate(rf.second);
            ESP_LOGI(TAG, "  Root: %s (mtime=%lld)", rf.first.c_str(), (long long)remoteFiles[rf.first]);
        }
    }

    // Safety check: if remote list is empty but local has files and previous sync state exists,
    // the network is likely down — abort sync to avoid deleting all local files
    if (remoteFiles.empty() && !localFiles.empty() && !prevState.empty()) {
        ESP_LOGE(TAG, "Remote file list is empty but local has %d files and sync state has %d entries — network likely down, aborting sync",
                 (int)localFiles.size(), (int)prevState.size());
        return {false, "网络异常，无法获取远程文件列表"};
    }

    int uploaded = 0, downloaded = 0, skipped = 0;
    int deletedLocal = 0, deletedRemote = 0, failed = 0;

    std::map<std::string, time_t> newState;
    auto auth = authHeader();

    // Union of all filenames
    std::set<std::string> allFiles;
    for (auto &lf : localFiles) allFiles.insert(lf.first);
    for (auto &rf : remoteFiles) allFiles.insert(rf.first);

    for (auto &fname : allFiles) {
        bool localExists = localFiles.find(fname) != localFiles.end();
        bool remoteExists = remoteFiles.find(fname) != remoteFiles.end();
        bool inPrev = prevState.find(fname) != prevState.end();

        ESP_LOGI(TAG, "Processing %s: local=%d remote=%d inPrev=%d",
                 fname.c_str(), localExists, remoteExists, inPrev);

        time_t localMtime = localExists ? localFiles[fname] : 0;
        time_t remoteMtime = remoteExists ? remoteFiles[fname] : 0;

        if (!localExists && !remoteExists) {
            continue;
        } else if (!localExists && remoteExists) {
            if (inPrev) {
                // Local was deleted → delete remote
                if (remove(remoteDir + fname)) deletedRemote++;
                else failed++;
            } else {
                // Remote new → download
                ESP_LOGI(TAG, "Downloading remote file: %s", fname.c_str());
                int dlStatus = 0;
                std::string content = download(remoteDir + fname, &dlStatus);
                if ((dlStatus == 200 || dlStatus == 203) &&
                    g_journal.saveEntryRaw(fname, content)) {
                    downloaded++;
                    newState[fname] = remoteMtime;
                    std::string localPath = "/sdcard/pjournal/" + fname;
                    struct utimbuf ut = {remoteMtime, remoteMtime};
                    utime(localPath.c_str(), &ut);
                } else {
                    failed++;
                }
            }
        } else if (localExists && !remoteExists) {
            if (inPrev) {
                // Remote was deleted → delete local
                if (g_journal.deleteEntry(fname)) deletedLocal++;
                else failed++;
            } else {
                // Local new → upload
                std::string content = g_journal.readEntry(fname);
                if (upload(remoteDir + fname, content)) {
                    uploaded++;
                    newState[fname] = localMtime;
                } else {
                    failed++;
                }
            }
        } else {
            // Both exist → compare mtimes
            time_t lm = localMtime ? localMtime : 0;
            time_t rm = remoteMtime ? remoteMtime : 0;
            time_t pm = inPrev ? prevState[fname] : 0;

            // Fast skip: if both local and remote mtimes match previous sync state,
            // the file hasn't changed on either side — skip without any network I/O
            if (pm > 0 && lm == pm && rm == pm) {
                skipped++;
                newState[fname] = pm;
                continue;
            }

            long diff = (long)(lm - rm);
            if (diff < 0) diff = -diff;
            if (diff <= 60) {
                skipped++;
                newState[fname] = lm ? lm : rm;
            } else if (lm > rm) {
                std::string content = g_journal.readEntry(fname);
                if (upload(remoteDir + fname, content)) {
                    uploaded++;
                    newState[fname] = lm;
                } else {
                    failed++;
                    newState[fname] = prevState.count(fname) ? prevState[fname] : lm;
                }
            } else {
                int dlStatus = 0;
                std::string content = download(remoteDir + fname, &dlStatus);
                if ((dlStatus == 200 || dlStatus == 203) &&
                    g_journal.saveEntryRaw(fname, content)) {
                    downloaded++;
                    newState[fname] = rm;
                    std::string localPath = "/sdcard/pjournal/" + fname;
                    struct utimbuf ut = {rm, rm};
                    utime(localPath.c_str(), &ut);
                } else {
                    failed++;
                }
            }
        }
    }

    saveSyncState(newState);

    char msg[192];
    std::string parts;
    if (uploaded > 0)          parts += "上传" + std::to_string(uploaded) + " ";
    if (downloaded > 0)        parts += "下载" + std::to_string(downloaded) + " ";
    if (deletedLocal > 0)      parts += "本地删除" + std::to_string(deletedLocal) + " ";
    if (deletedRemote > 0)     parts += "远程删除" + std::to_string(deletedRemote) + " ";
    if (skipped > 0)           parts += "跳过" + std::to_string(skipped) + " ";
    if (failed > 0)            parts += "失败" + std::to_string(failed) + " ";

    if (parts.empty()) {
        snprintf(msg, sizeof(msg), "无需同步");
    } else {
        parts.pop_back(); // trailing space
        if (failed == 0 && uploaded + downloaded + deletedLocal + deletedRemote > 0)
            snprintf(msg, sizeof(msg), "同步成功: %s", parts.c_str());
        else if (failed == 0)
            snprintf(msg, sizeof(msg), "%s", parts.c_str());
        else
            snprintf(msg, sizeof(msg), "部分失败: %s", parts.c_str());
    }

    if (failed > 0 && uploaded + downloaded + deletedLocal + deletedRemote == 0)
        return {false, msg};
    return {true, msg};
}

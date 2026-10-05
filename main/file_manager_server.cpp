#include "file_manager_server.h"
#include "journal_storage.h"
#include "settings_manager.h"
#include "screen_reader.h"  // readerRequestStandbyImage（/api/set_standby 投递给主任务解图）
#include <esp_log.h>
#include <esp_http_server.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <cstdint>

static const char *TAG = "FileMgr";
static httpd_handle_t s_server = nullptr;
static uint16_t s_port = 80;

// 停机旗子。httpd_stop() 是个**调用方忙等**（httpd_main.c: `while (status !=
// THREAD_STOPPED) sleep(100)`），而 httpd 线程只在**当前请求处理完之后**才看停机
// 控制消息。所以只要有一个大文件在传（下载目录 ZIP 上限 16MB、上传上限 8MB，都是
// 客户端配速），在主循环里直接 httpd_stop 就会冻结整个 UI 几十秒到几分钟——触摸、
// 按键、PMU、待机检查全停。立起这面旗子，让流式循环在下一个分片边界主动退出，
// httpd_stop 就只等毫秒级。见 main/file_manager_server.cpp 各 handler。
static std::atomic<bool> s_shutdown{false};

// ── 传输进度 ─────────────────────────────────────────────────────────────
// 单份全局快照，httpd 任务写、主任务（界面）读。见头文件里的并发约定。
static FmXfer s_xfer = {};

const FmXfer *file_manager_get_xfer() { return &s_xfer; }

// 开始一次传输。**先把名字写好，最后才置 active** —— 读方看到 active=1 时名字必定
// 已经落定，不会读到半截字符串。
static void xferBegin(int kind, const char *name, uint32_t total) {
    s_xfer.active = 0;
    s_xfer.kind = kind;
    s_xfer.done = 0;
    s_xfer.total = total;
    strncpy(s_xfer.name, name ? name : "", sizeof(s_xfer.name) - 1);
    s_xfer.name[sizeof(s_xfer.name) - 1] = '\0';
    s_xfer.active = 1;
}

static void xferAdd(uint32_t n) { s_xfer.done += n; }

// 结束传输。先清 active：读方这一拍最多看到"active=0 但 done 是旧值"，然后自己
// 决定要不要显示完成提示。
static void xferEnd() { s_xfer.active = 0; }

// 传输结束时无条件清 active（handler 里有好几条提前 return 的失败路径，靠析构兜住，
// 免得界面上留一条永远"传输中"的进度）。
struct XferGuard {
    ~XferGuard() { xferEnd(); }
};

// ── Helpers ──────────────────────────────────────────────────────────────

static std::string urlDecode(const char *src) {
    std::string out;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            out += (char)strtol(hex, nullptr, 16);
            src += 3;
        } else if (*src == '+') {
            out += ' ';
            src++;
        } else {
            out += *src++;
        }
    }
    return out;
}

static bool isSafePath(const std::string &path) {
    if (path.find("..") != std::string::npos) return false;
    // /sdcard must be the mount root itself or followed by '/', otherwise
    // "/sdcard2/..." would bypass the check.
    if (path.compare(0, 7, "/sdcard") != 0) return false;
    if (path.size() > 7 && path[7] != '/') return false;
    return true;
}

// 图片后缀 → MIME。返回 nullptr = 不是我们认的图片（照旧按 attachment 下载）。
// 只认这几种：浏览器能直接显示的常见格式；BMP 也认（有些导出工具就存它）。
// 与设备端"设为待机画面"认的后缀**故意不一致**：那边解码器只吃 JPG/PNG，
// 但网页端能否预览是浏览器的事，多认一个 BMP 不吃亏。
static const char *imageMimeFor(const std::string &path) {
    struct { const char *ext; const char *mime; } kMap[] = {
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".png", "image/png"},
        {".bmp", "image/bmp"},  {".gif", "image/gif"},   {".webp", "image/webp"},
    };
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return nullptr;
    std::string ext = path.substr(dot);
    for (char &c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (auto &m : kMap) if (ext == m.ext) return m.mime;
    return nullptr;
}

// 设备端「设为待机画面」认的后缀（只有 JPG/PNG 有解码器）。网页上给按钮的条件用它，
// 免得点了才报"只支持 JPG/PNG"。
static bool isStandbyImagePath(const std::string &path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char &c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

static std::string formatSize(off_t size) {
    char buf[32];
    if (size < 1024) snprintf(buf, sizeof(buf), "%lld B", (long long)size);
    else if (size < 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f KB", size / 1024.0);
    else snprintf(buf, sizeof(buf), "%.1f MB", size / (1024.0 * 1024.0));
    return buf;
}

static void sendJsonOK(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void sendJsonError(httpd_req_t *req, const char *msg) {
    httpd_resp_set_type(req, "application/json");
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(req, buf);
}

static std::string getQueryParam(httpd_req_t *req, const char *key) {
    size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) return "";
    std::vector<char> query(len + 1);
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) return "";
    char val[512] = {};
    if (httpd_query_key_value(query.data(), key, val, sizeof(val)) != ESP_OK) return "";
    return urlDecode(val);
}

// Optional access token. When a "文件管理密码" is set in settings, every
// request must carry it via the X-Auth-Token header or the ?token= query
// param. Empty setting keeps the old open access.
static bool authOk(httpd_req_t *req) {
    std::string expected = g_settings.getString("file_mgr_token");
    if (expected.empty()) return true;
    char buf[128] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", buf, sizeof(buf)) == ESP_OK &&
        expected == buf) return true;
    std::string q = getQueryParam(req, "token");
    return !q.empty() && q == expected;
}

static esp_err_t sendAuthError(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    sendJsonError(req, "unauthorized");
    return ESP_OK;
}

// ── ZIP helpers (store mode, no compression) ────────────────────────────

struct ZipEntry {
    std::string relPath;   // relative path inside zip
    uint32_t crc32;
    uint32_t size;
    uint32_t offset;       // offset of local file header in zip stream
};

static uint32_t crc32Table[256];
static bool crc32TableInit = false;
static const size_t MAX_ZIP_FILES = 512;
static const uint64_t MAX_ZIP_TOTAL_SIZE = 16ULL * 1024 * 1024;

static void initCRC32Table() {
    if (crc32TableInit) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32Table[i] = c;
    }
    crc32TableInit = true;
}

static uint32_t computeCRC32(const uint8_t *data, size_t len, uint32_t crc = 0xFFFFFFFF) {
    initCRC32Table();
    for (size_t i = 0; i < len; i++)
        crc = crc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static uint32_t crc32Finish(uint32_t crc) { return crc ^ 0xFFFFFFFF; }

// Write little-endian uint16/uint32 into buffer
static void putU16(uint8_t *buf, uint16_t v) { buf[0]=v; buf[1]=v>>8; }
static void putU32(uint8_t *buf, uint32_t v) { buf[0]=v; buf[1]=v>>8; buf[2]=v>>16; buf[3]=v>>24; }

// Recursively collect files under dirPath, storing relative paths from basePath
static void collectFiles(const std::string &dirPath, const std::string &basePath,
                         std::vector<ZipEntry> &entries, uint64_t &totalSize,
                         int depth = 0) {
    // 递归深度上限：这个 handler 跑在 httpd 的 8KB 栈上（见 file_manager_server_start
    // 的 config.stack_size），目录树离谱地深时递归会直接爆栈 panic（不是卡死，是崩溃）。
    if (depth > 12) return;
    DIR *dir = opendir(dirPath.c_str());
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (entries.size() >= MAX_ZIP_FILES || totalSize >= MAX_ZIP_TOTAL_SIZE) break;
        if (ent->d_name[0] == '.') continue;
        std::string full = dirPath + "/" + ent->d_name;
        std::string rel = basePath.empty() ? ent->d_name : basePath + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collectFiles(full, rel, entries, totalSize, depth + 1);
        } else {
            if (rel.length() > 0xFFFF) {
                ESP_LOGW(TAG, "Skipping path too long for ZIP (%d bytes): %s", (int)rel.length(), rel.c_str());
                continue;
            }
            if (totalSize + (uint64_t)st.st_size > MAX_ZIP_TOTAL_SIZE) {
                ESP_LOGW(TAG, "Skipping ZIP file beyond size cap: %s", rel.c_str());
                continue;
            }
            ZipEntry e;
            e.relPath = rel;
            e.crc32 = 0;
            e.size = (uint32_t)st.st_size;
            e.offset = 0;
            entries.push_back(e);
            totalSize += (uint64_t)st.st_size;
        }
    }
    closedir(dir);
}

// ── Embedded HTML ────────────────────────────────────────────────────────

static const char *HTML_PAGE = R"raw(<!DOCTYPE html>
<html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>文件管理</title>
<style>
body{font-family:sans-serif;max-width:800px;margin:0 auto;padding:12px;font-size:14px}
h1{font-size:18px;margin:0 0 12px}
#breadcrumb{margin-bottom:8px;color:#666}
table{width:100%;border-collapse:collapse}
th,td{padding:6px 8px;text-align:left;border-bottom:1px solid #eee;font-size:13px}
th{background:#f5f5f5;font-weight:600}
.dir{color:#2563eb;cursor:pointer}
.dir:hover{text-decoration:underline}
.act{white-space:nowrap}
.act button{margin:0 2px;padding:2px 8px;font-size:12px;cursor:pointer}
#toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:12px 0}
#toolbar input[type=text]{width:120px;padding:3px 6px}
#toolbar input[type=file]{font-size:12px}
#msg{padding:6px;margin:8px 0;border-radius:4px;display:none}
.ok{background:#d4edda;color:#155724}
.err{background:#f8d7da;color:#721c24}
#up{display:none;margin:8px 0}
#up .bar{height:8px;background:#e5e7eb;border-radius:4px;overflow:hidden}
#up .bar i{display:block;height:100%;width:0;background:#2563eb;transition:width .15s linear}
#up .txt{font-size:12px;color:#555;margin-top:4px}
button[disabled]{opacity:.5;cursor:default}
</style></head><body>
<h1>pjournal - 文件管理</h1>
<div id="breadcrumb"></div>
<div id="toolbar">
<input type="file" id="fileInput">
<button onclick="upload()">上传</button>
<input type="text" id="dirName" placeholder="文件夹名">
<button onclick="mkdir()">新建</button>
<button onclick="setToken()">密码</button>
</div>
<div id="up"><div class="bar"><i id="upBar"></i></div><div class="txt" id="upTxt"></div></div>
<div id="msg"></div>
<table><thead><tr><th>名称</th><th>大小</th><th>操作</th></tr></thead>
<tbody id="list"></tbody></table>
<script>
var curPath='/sdcard';
var upBusy=false;
var token='';try{token=localStorage.getItem('pjournal_token')||''}catch(e){}
function hd(){return token?{'X-Auth-Token':token}:{}}
function setToken(){var t=prompt('文件管理密码(留空则不设)',token);if(t!==null){token=t.trim();try{localStorage.setItem('pjournal_token',token)}catch(e){}}}
function showMsg(t,ok){var e=document.getElementById('msg');e.textContent=t;e.className=ok?'ok':'err';e.style.display='block';setTimeout(function(){e.style.display='none'},3000)}
function loadDir(p){
  curPath=p;
  fetch('/api/list?path='+encodeURIComponent(p),{headers:hd()}).then(r=>r.json()).then(d=>{
    document.getElementById('breadcrumb').textContent=d.path;
    var h='';
    if(d.path!=='/sdcard') h+='<tr><td class="dir" onclick="loadDir(\''+esc(p.replace(/\/[^/]+$/,''))+'\')">..</td><td></td><td></td></tr>';
    d.entries.forEach(e=>{
      var fp=esc((d.path==='/'?'':d.path)+'/'+e.name);
      if(e.type==='dir') h+='<tr><td class="dir" onclick="loadDir(\''+fp+'\')">'+esc(e.name)+'/</td><td></td><td class="act"><button onclick="dlDir(\''+fp+'\')">下载</button><button onclick="del(\''+fp+'\',true)">删除</button></td></tr>';
      else{
        // 图片多两个动作：「查看」新开一页内联显示（以前一律按 attachment 下载，
        // 网页上根本看不了图）；「设为待机」只在解码器认的后缀（JPG/PNG）上给。
        var a='<button onclick="dl(\''+fp+'\')">下载</button>';
        if(e.image) a+='<button onclick="view(\''+fp+'\')">查看</button>';
        if(e.standby) a+='<button onclick="setSb(\''+fp+'\')">设为待机</button>';
        a+='<button onclick="del(\''+fp+'\',false)">删除</button>';
        h+='<tr><td>'+esc(e.name)+'</td><td>'+e.size+'</td><td class="act">'+a+'</td></tr>';
      }
    });
    document.getElementById('list').innerHTML=h;
  }).catch(e=>showMsg('加载失败',false));
}
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/'/g,'&#39;')}
function upload(){
  var f=document.getElementById('fileInput').files[0];
  if(!f){showMsg('先选择文件',false);return}
  if(upBusy)return;
  // fetch() 不给上传进度（浏览器只对下载暴露 body 流），所以这里必须用 XHR：
  // xhr.upload.onprogress 是网页端唯一能拿到"已发出多少字节"的接口。服务端本来就是
  // 边收边写盘，事件会一路正常触发。
  var fd=new FormData();fd.append('file',f);
  var x=new XMLHttpRequest();
  var btns=document.querySelectorAll('#toolbar button');
  upBusy=true;
  for(var i=0;i<btns.length;i++)btns[i].disabled=true;   // 传完之前别让人重复点
  document.getElementById('up').style.display='block';
  upPct(0);upShow('上传中  '+fmtB(f.size));
  x.open('POST','/api/upload?path='+encodeURIComponent(curPath)+'&name='+encodeURIComponent(f.name),true);
  if(token)x.setRequestHeader('X-Auth-Token',token);
  x.upload.onprogress=function(e){
    if(!e.lengthComputable)return;
    var p=Math.floor(e.loaded*100/e.total);
    upPct(p);upShow('上传中 '+p+'%   '+fmtB(e.loaded)+' / '+fmtB(e.total));
  };
  x.onload=function(){
    upEnd(btns);
    var d={};try{d=JSON.parse(x.responseText)}catch(e){}
    if(d.ok){upPct(100);upShow('上传完成   '+fmtB(f.size));showMsg('上传成功',true);loadDir(curPath)}
    else{upShow('上传失败');showMsg('上传失败: '+(d.error||('HTTP '+x.status)),false)}
    upReset();
  };
  x.onerror=function(){upEnd(btns);upShow('上传失败');showMsg('上传失败（连接中断）',false);upReset()};
  x.send(fd);
}
function upEnd(btns){upBusy=false;for(var i=0;i<btns.length;i++)btns[i].disabled=false}
function upPct(p){document.getElementById('upBar').style.width=p+'%'}
function upShow(t){document.getElementById('upTxt').textContent=t}
function upReset(){setTimeout(function(){document.getElementById('up').style.display='none';upPct(0)},2500)}
function fmtB(n){
  if(n<1024)return n+' B';
  if(n<1048576)return (n/1024).toFixed(1)+' KB';
  return (n/1048576).toFixed(1)+' MB';
}
function dl(p){window.open('/api/download?path='+encodeURIComponent(p)+'&token='+encodeURIComponent(token))}
function dlDir(p){window.open('/api/download_dir?path='+encodeURIComponent(p)+'&token='+encodeURIComponent(token))}
function view(p){window.open('/api/download?view=1&path='+encodeURIComponent(p)+'&token='+encodeURIComponent(token))}
function setSb(p){
  if(!confirm('把这台设备的待机画面设成这张图？\n'+p))return;
  fetch('/api/set_standby?path='+encodeURIComponent(p),{method:'POST',headers:hd()})
  .then(r=>r.json()).then(d=>showMsg(d.ok?'已设为待机画面（设备空闲几秒后生效）':'设置失败: '+d.error,d.ok))
  .catch(e=>showMsg('设置失败',false));
}
function del(p,isDir){
  if(!confirm('确认删除?'))return;
  fetch('/api/delete?path='+encodeURIComponent(p)+'&dir='+isDir,{method:'POST',headers:hd()})
  .then(r=>r.json()).then(d=>{showMsg(d.ok?'删除成功':'删除失败: '+d.error,d.ok);loadDir(curPath)})
  .catch(()=>showMsg('删除失败',false));
}
function mkdir(){
  var n=document.getElementById('dirName').value.trim();if(!n)return;
  fetch('/api/mkdir?path='+encodeURIComponent(curPath+'/'+n),{method:'POST',headers:hd()})
  .then(r=>r.json()).then(d=>{showMsg(d.ok?'创建成功':'创建失败: '+d.error,d.ok);if(d.ok){document.getElementById('dirName').value='';loadDir(curPath)}})
  .catch(()=>showMsg('创建失败',false));
}
loadDir('/sdcard');
</script></body></html>)raw";

// ── URI Handlers ─────────────────────────────────────────────────────────

static esp_err_t __attribute__((unused)) handler_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, HTML_PAGE);
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_list(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    if (path.empty()) path = "/sdcard";
    if (!isSafePath(path)) {
        sendJsonError(req, "invalid path");
        return ESP_OK;
    }

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);

    DIR *dir = opendir(path.c_str());
    if (!dir) {
        if (mtx) xSemaphoreGiveRecursive(mtx);
        sendJsonError(req, "cannot open directory");
        return ESP_OK;
    }

    std::string json = "{\"path\":\"";
    json += path;
    json += "\",\"entries\":[";

    struct dirent *ent;
    bool first = true;
    // 条目数上限：这个 JSON 在 PSRAM 里按目录大小线性长。正常目录几十上百项无感，
    // 但 SD 卡上堆了几千个文件时，一次列目录就是几百 KB 的字符串，且是在处理 HTTP
    // 请求的过程中分配。截断比把内存吃光好——反正这么长的列表在界面上也没法用。
    static constexpr int kMaxEntries = 2000;
    int count = 0;
    bool truncated = false;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        if (count >= kMaxEntries) { truncated = true; break; }
        count++;
        std::string full = path + "/" + ent->d_name;
        struct stat st;
        bool isDir = false;
        off_t fsize = 0;
        if (stat(full.c_str(), &st) == 0) {
            isDir = S_ISDIR(st.st_mode);
            fsize = st.st_size;
        }
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"";
        // escape JSON string
        for (const char *p = ent->d_name; *p; p++) {
            if (*p == '"' || *p == '\\') json += '\\';
            json += *p;
        }
        json += "\",\"type\":\"";
        json += isDir ? "dir" : "file";
        json += "\",\"size\":\"";
        json += formatSize(fsize);
        json += "\"";
        // 图片条目：前端据此多给「查看」按钮。standby 再细分一层 —— 只有解码器认的
        // JPG/PNG 才给「设为待机」，别的（bmp/gif/webp）能看不能设。
        if (!isDir && imageMimeFor(full)) {
            json += ",\"image\":true";
            if (isStandbyImagePath(full)) json += ",\"standby\":true";
        }
        json += "}";
    }
    closedir(dir);
    if (mtx) xSemaphoreGiveRecursive(mtx);

    json += "],\"truncated\":";
    json += truncated ? "true" : "false";
    json += "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_download(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    if (!isSafePath(path)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        if (mtx) xSemaphoreGiveRecursive(mtx);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    // extract filename for Content-Disposition
    std::string filename = path;
    auto slash = filename.rfind('/');
    if (slash != std::string::npos) filename = filename.substr(slash + 1);

    // 图片预览：网页上的「查看」走 &view=1。此前无论什么文件都按 attachment 下载，
    // 浏览器只会弹存盘、不显示 —— 图片在网页端就等于"看不了"。这里按要求放行 inline：
    // 认得出的图片给真 MIME，浏览器自己渲染（大图会自适应窗口）；不是图片或没带
    // view=1 就照旧下载。下载按钮不受影响。
    const char *mime = (getQueryParam(req, "view") == "1") ? imageMimeFor(path) : nullptr;
    httpd_resp_set_type(req, mime ? mime : "application/octet-stream");
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "%s; filename=\"%s\"", mime ? "inline" : "attachment", filename.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);

    XferGuard xferGuard;
    xferBegin(1, filename.c_str(), (uint32_t)st.st_size);

    char buf[4096];
    size_t n;
    bool sendOk = true;
    while (!s_shutdown.load(std::memory_order_relaxed)) {
        n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        if (mtx) xSemaphoreGiveRecursive(mtx);
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            sendOk = false;
            if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
            break;
        }
        xferAdd((uint32_t)n);
        if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
    }
    fclose(f);
    if (mtx) xSemaphoreGiveRecursive(mtx);
    if (sendOk) httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_download_dir(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    if (!isSafePath(path)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    // Collect all files
    std::vector<ZipEntry> entries;
    std::string dirName = path;
    auto slash = dirName.rfind('/');
    if (slash != std::string::npos) dirName = dirName.substr(slash + 1);
    uint64_t totalSize = 0;
    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
    collectFiles(path, dirName, entries, totalSize);
    if (mtx) xSemaphoreGiveRecursive(mtx);

    if (entries.empty()) {
        sendJsonError(req, "empty directory");
        return ESP_OK;
    }

    // 打包下载的进度：总量就用 collectFiles 累出来的文件字节和（ZIP 头尾的几百字节
    // 不算，误差可以忽略）。
    XferGuard xferGuard;
    xferBegin(2, (dirName + ".zip").c_str(), (uint32_t)totalSize);

    // Set response headers
    httpd_resp_set_type(req, "application/zip");
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s.zip\"", dirName.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);

    // Write zip: local file headers + file data, then central directory, then end record
    uint8_t buf[4096];      // file read buffer
    uint32_t offset = 0;

    // Pass 1: write local file headers + data
    for (auto &e : entries) {
        e.offset = offset;
        size_t nameLen = e.relPath.length();
        std::vector<uint8_t> lfh(30 + nameLen);

        // Local file header (30 + nameLen bytes)
        putU32(lfh.data() + 0, 0x04034b50);   // signature
        putU16(lfh.data() + 4, 20);           // version needed
        putU16(lfh.data() + 6, 0);            // flags
        putU16(lfh.data() + 8, 0);            // compression: store
        putU16(lfh.data() + 10, 0);           // mod time
        putU16(lfh.data() + 12, 0);           // mod date
        putU32(lfh.data() + 14, 0);           // crc32 (placeholder, fill after reading)
        putU32(lfh.data() + 18, 0);           // compressed size (placeholder)
        putU32(lfh.data() + 22, 0);           // uncompressed size (placeholder)
        putU16(lfh.data() + 26, (uint16_t)nameLen);  // filename length
        putU16(lfh.data() + 28, 0);           // extra field length
        memcpy(lfh.data() + 30, e.relPath.c_str(), nameLen);

        // Read file, compute CRC32 incrementally
        // dirName 是 path 最后一个 '/' 之后的部分。请求路径带尾斜杠时（如 /sdcard/）
        // 它是空串，而 collectFiles 此刻的 relPath 本就不含 "dirName/" 前缀 —— 这时再
        // substr(1) 会把文件名首字符也砍掉，fopen 失败、CRC/size 全 0：ZIP 里每个条目
        // 都是 0 字节，静默数据损坏。空 dirName 直接用 relPath，并压掉重复的斜杠。
        std::string fullPath = path;
        if (!fullPath.empty() && fullPath.back() == '/') fullPath.pop_back();
        fullPath += "/";
        fullPath += dirName.empty() ? e.relPath : e.relPath.substr(dirName.length() + 1);
        if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
        FILE *f = fopen(fullPath.c_str(), "rb");
        uint32_t crc = 0xFFFFFFFF;
        uint32_t fsize = 0;
        if (f) {
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                crc = computeCRC32(buf, n, crc);
                fsize += n;
            }
            rewind(f);
        }
        if (mtx) xSemaphoreGiveRecursive(mtx);
        crc = crc32Finish(crc);

        // Fill in CRC and sizes
        putU32(lfh.data() + 14, crc);
        putU32(lfh.data() + 18, fsize);
        putU32(lfh.data() + 22, fsize);
        e.crc32 = crc;
        e.size = fsize;

        // Send local file header
        if (httpd_resp_send_chunk(req, (const char*)lfh.data(), 30 + nameLen) != ESP_OK) {
            if (f) fclose(f);
            return ESP_OK;
        }
        offset += 30 + nameLen;

        // Send file data
        if (f) {
            size_t n;
            while (!s_shutdown.load(std::memory_order_relaxed)) {
                if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
                n = fread(buf, 1, sizeof(buf), f);
                if (mtx) xSemaphoreGiveRecursive(mtx);
                if (n == 0) break;
                if (httpd_resp_send_chunk(req, (const char*)buf, n) != ESP_OK) {
                    fclose(f);
                    return ESP_OK;
                }
                xferAdd((uint32_t)n);
            }
            fclose(f);
        }
        offset += fsize;
    }

    // Pass 2: central directory
    std::vector<uint8_t> cdh;  // central directory header buffer
    uint32_t cdOffset = offset;

    for (auto &e : entries) {
        if (s_shutdown.load(std::memory_order_relaxed)) return ESP_OK;
        size_t nameLen = e.relPath.length();
        cdh.resize(46 + nameLen);

        putU32(cdh.data() + 0, 0x02014b50);   // signature
        putU16(cdh.data() + 4, 20);           // version made by
        putU16(cdh.data() + 6, 20);           // version needed
        putU16(cdh.data() + 8, 0);            // flags
        putU16(cdh.data() + 10, 0);           // compression: store
        putU16(cdh.data() + 12, 0);           // mod time
        putU16(cdh.data() + 14, 0);           // mod date
        putU32(cdh.data() + 16, e.crc32);     // crc32
        putU32(cdh.data() + 20, e.size);      // compressed size
        putU32(cdh.data() + 24, e.size);      // uncompressed size
        putU16(cdh.data() + 28, (uint16_t)nameLen);  // filename length
        putU16(cdh.data() + 30, 0);           // extra field length
        putU16(cdh.data() + 32, 0);           // file comment length
        putU16(cdh.data() + 34, 0);           // disk number start
        putU16(cdh.data() + 36, 0);           // internal file attributes
        putU32(cdh.data() + 38, 0);           // external file attributes
        putU32(cdh.data() + 42, e.offset);    // relative offset of local header
        memcpy(cdh.data() + 46, e.relPath.c_str(), nameLen);

        if (httpd_resp_send_chunk(req, (const char*)cdh.data(), 46 + nameLen) != ESP_OK) {
            return ESP_OK;
        }
        offset += 46 + nameLen;
    }

    uint32_t cdSize = offset - cdOffset;

    // End of central directory record
    uint8_t eocd[22];
    putU32(eocd + 0, 0x06054b50);          // signature
    putU16(eocd + 4, 0);                   // disk number
    putU16(eocd + 6, 0);                   // disk with central dir
    putU16(eocd + 8, (uint16_t)entries.size());  // entries on this disk
    putU16(eocd + 10, (uint16_t)entries.size()); // total entries
    putU32(eocd + 12, cdSize);             // central dir size
    putU32(eocd + 16, cdOffset);           // central dir offset
    putU16(eocd + 20, 0);                  // comment length

    httpd_resp_send_chunk(req, (const char*)eocd, 22);
    httpd_resp_send_chunk(req, nullptr, 0);

    ESP_LOGI(TAG, "ZIP download: %s (%d files)", path.c_str(), (int)entries.size());
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_upload(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string dir = getQueryParam(req, "path");
    std::string name = getQueryParam(req, "name");
    if (!isSafePath(dir) || name.empty()) {
        sendJsonError(req, "invalid path or name");
        return ESP_OK;
    }

    // reject names with path separators
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        sendJsonError(req, "invalid filename");
        return ESP_OK;
    }

    std::string fullpath = dir + "/" + name;
    std::string tmpPath = dir + "/." + name + ".upload";

    // Cap uploads so an oversized body can't exhaust heap or fill the card.
    const size_t MAX_UPLOAD = 8 * 1024 * 1024;
    size_t content_len = req->content_len;
    if (content_len == 0) {
        sendJsonError(req, "empty body");
        return ESP_OK;
    }
    if (content_len > MAX_UPLOAD) {
        sendJsonError(req, "file too large");
        return ESP_OK;
    }

    // get content-type to find boundary
    char ct[128] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        sendJsonError(req, "no content-type");
        return ESP_OK;
    }

    // find boundary
    std::string ctStr(ct);
    auto bpos = ctStr.find("boundary=");
    if (bpos == std::string::npos) {
        sendJsonError(req, "no boundary");
        return ESP_OK;
    }
    std::string boundary = "--" + ctStr.substr(bpos + 9);

    // 进度上报。总长用 content_len 估（含 multipart 头尾那几百字节，忽略不计）。
    XferGuard xferGuard;
    xferBegin(0, name.c_str(), (uint32_t)content_len);

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);

    FILE *f = fopen(tmpPath.c_str(), "wb");
    if (!f) {
        if (mtx) xSemaphoreGiveRecursive(mtx);
        sendJsonError(req, "cannot create file");
        return ESP_OK;
    }
    if (mtx) xSemaphoreGiveRecursive(mtx);

    // Stream the multipart body to disk with a sliding window so only a few KB
    // are buffered instead of the whole upload. The file part's data ends at
    // "\r\n--boundary", which also consumes the trailing CRLF.
    const size_t CHUNK = 2048;
    const std::string hdrSep = "\r\n\r\n";
    // boundary already carries the leading "--", so the part separator in the
    // body is "\r\n--<value>", i.e. "\r\n" + boundary.
    const std::string marker = "\r\n" + boundary;
    const size_t keep = marker.size() - 1;   // tail kept as a possible partial marker
    std::string window;
    size_t remaining = content_len;
    bool inHeader = true;
    bool done = false;
    bool ok = true;
    size_t written = 0;

    while (remaining > 0 && !done && ok && !s_shutdown.load(std::memory_order_relaxed)) {
        char raw[CHUNK];
        size_t want = remaining < CHUNK ? remaining : CHUNK;
        int r = httpd_req_recv(req, raw, want);
        if (r <= 0) { ok = false; break; }
        remaining -= (size_t)r;
        window.append(raw, (size_t)r);

        if (inHeader) {
            auto it = std::search(window.begin(), window.end(), hdrSep.begin(), hdrSep.end());
            if (it == window.end()) {
                if (window.size() > 8192) ok = false;   // malformed: no header end
                continue;
            }
            window.erase(window.begin(), it + (int)hdrSep.size());
            inHeader = false;
        }

        while (!done && !window.empty()) {
            auto it = std::search(window.begin(), window.end(), marker.begin(), marker.end());
            if (it != window.end()) {
                size_t n = (size_t)(it - window.begin());
                if (n > 0) {
                    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
                    if (fwrite(window.data(), 1, n, f) != n) ok = false;
                    if (mtx) xSemaphoreGiveRecursive(mtx);
                    written += n;
                    xferAdd((uint32_t)n);
                }
                window.clear();
                done = true;
                break;
            }
            if (window.size() <= keep) break;   // need more data before deciding
            size_t writeNow = window.size() - keep;
            if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
            if (fwrite(window.data(), 1, writeNow, f) != writeNow) ok = false;
            if (mtx) xSemaphoreGiveRecursive(mtx);
            written += writeNow;
            xferAdd((uint32_t)writeNow);
            window.erase(0, writeNow);
        }
    }
    // 被停机旗子打断：宁可丢弃临时文件，也**绝不能**把半截内容 rename 成正式文件。
    if (s_shutdown.load(std::memory_order_relaxed)) ok = false;
    // Body ended without the closing boundary — flush whatever is buffered.
    if (ok && !inHeader && !done && !window.empty()) {
        if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
        if (fwrite(window.data(), 1, window.size(), f) != window.size()) ok = false;
        if (mtx) xSemaphoreGiveRecursive(mtx);
        written += window.size();
        xferAdd((uint32_t)window.size());
        window.clear();
    }
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (mtx) xSemaphoreGiveRecursive(mtx);

    if (!ok) {
        remove(tmpPath.c_str());   // discard the partial file
        sendJsonError(req, "upload failed");
        return ESP_OK;
    }
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
    remove(fullpath.c_str());
    bool renamed = rename(tmpPath.c_str(), fullpath.c_str()) == 0;
    if (mtx) xSemaphoreGiveRecursive(mtx);
    if (!renamed) {
        remove(tmpPath.c_str());
        sendJsonError(req, "upload failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Uploaded: %s (%d bytes)", fullpath.c_str(), (int)written);
    sendJsonOK(req);
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_delete(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    std::string dirFlag = getQueryParam(req, "dir");
    if (!isSafePath(path)) {
        sendJsonError(req, "invalid path");
        return ESP_OK;
    }

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);

    int ret;
    if (dirFlag == "1" || dirFlag == "true") {
        ret = rmdir(path.c_str());
    } else {
        ret = remove(path.c_str());
    }
    if (mtx) xSemaphoreGiveRecursive(mtx);

    if (ret == 0) {
        ESP_LOGI(TAG, "Deleted: %s", path.c_str());
        sendJsonOK(req);
    } else {
        sendJsonError(req, "delete failed");
    }
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_mkdir(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    if (!isSafePath(path)) {
        sendJsonError(req, "invalid path");
        return ESP_OK;
    }

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);

    int ret = mkdir(path.c_str(), 0777);
    if (mtx) xSemaphoreGiveRecursive(mtx);

    if (ret == 0) {
        ESP_LOGI(TAG, "Created dir: %s", path.c_str());
        sendJsonOK(req);
    } else {
        sendJsonError(req, "mkdir failed");
    }
    return ESP_OK;
}

// 「设为待机画面」（网页端）。这里**只投递不落地**：把设置键改掉、把"解这张图"记成
// 一张待办就返回。真正解图在主任务上做（readerStandbyPump）——httpd 任务只有 8KB 栈，
// 解几百万像素的 JPEG 会直接爆栈，见 screen_reader.h 的注释。
// 返回后设备那边还没生成缓存，待机表盘会先画一帧"正在生成"的占位，几秒后就好了。
static esp_err_t __attribute__((unused)) handler_set_standby(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    if (!isSafePath(path)) {
        sendJsonError(req, "invalid path");
        return ESP_OK;
    }
    // 只有 JPG/PNG 有解码器。前端按 standby 标记给不给按钮，这里再挡一道。
    if (!isStandbyImagePath(path)) {
        sendJsonError(req, "只支持 JPG / PNG");
        return ESP_OK;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        sendJsonError(req, "不是文件");
        return ESP_OK;
    }

    readerRequestStandbyImage(path);
    ESP_LOGI(TAG, "Web set standby image: %s", path.c_str());
    sendJsonOK(req);
    return ESP_OK;
}

// ── Public API ───────────────────────────────────────────────────────────

bool file_manager_server_start(uint16_t port) {
    if (s_server) return true;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 10;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return false;
    }
    s_port = port;

    httpd_uri_t uris[] = {
        {"/",                HTTP_GET,  handler_index,        nullptr},
        {"/api/list",        HTTP_GET,  handler_list,         nullptr},
        {"/api/download",    HTTP_GET,  handler_download,     nullptr},
        {"/api/download_dir",HTTP_GET,  handler_download_dir, nullptr},
        {"/api/upload",      HTTP_POST, handler_upload,       nullptr},
        {"/api/delete",      HTTP_POST, handler_delete,       nullptr},
        {"/api/mkdir",       HTTP_POST, handler_mkdir,        nullptr},
        {"/api/set_standby", HTTP_POST, handler_set_standby,  nullptr},
    };
    for (auto &u : uris) {
        httpd_register_uri_handler(s_server, &u);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", port);
    return true;
}

void file_manager_server_stop() {
    if (s_server) {
        s_shutdown.store(true, std::memory_order_relaxed);
        httpd_stop(s_server);
        s_server = nullptr;
        s_shutdown.store(false, std::memory_order_relaxed);
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

uint16_t file_manager_server_get_port() {
    return s_port;
}

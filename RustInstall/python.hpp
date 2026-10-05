// python.hpp : CPython 官方发行文件的枚举、下载、校验与管理
// 数据源策略（FTP 绝对优先）：
//   主源  https://www.python.org/ftp/python/                目录列表（多年稳定）
//         /ftp/python/                                      → 版本枚举（1 个请求，含索引日期）
//         /ftp/python/{version}/                            → 该版本文件（1 个请求/版本）
//         直链 = https://www.python.org/ftp/python/{version}/{filename}
//   可选  API v2 仅在提供 --py-token 时作为元数据增强（SHA-256/MD5/精确日期），
//         匿名调用受严格限流，失败自动回退 FTP 数据，不影响功能。
//   镜像  华为云 / npmmirror 与官方 FTP 目录结构一致，用于加速下载。
#pragma once

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "http.hpp"
#include "json.hpp"
#include "logger.hpp"
#include "md5.hpp"
#include "providers/archive.hpp"
#include "rust_dist.hpp" // dist::run_hidden
#include "sha256.hpp"
#include "strutil.hpp"

namespace py {

namespace fs = std::filesystem;

struct PyVersion {
    std::string version, date; // date 来自 FTP 索引的目录修改日期（近似发布日期）
};

struct PyFile {
    std::string version, release_date, os_name, arch, filename, url, rel_path, name;
    std::string sha256, md5; // 仅 API 增强时可用
    uint64_t filesize = 0;   // 仅 API 增强时可用；FTP 模式由下载时 Content-Length 保证
    bool from_api = false;
};

// --------------------------------------------------------------- 镜像

struct PyMirror {
    const char* name;
    const wchar_t* base; // 以 / 结尾，后接 ftp/python/...
};

// 优先级：华为云 → 官方 → npmmirror（国内镜像通常更快；失败自动切换）
inline const PyMirror kMirrors[] = {
    {"官方 www.python.org", L"https://www.python.org/"},
    {"华为云 mirrors.huaweicloud.com", L"https://mirrors.huaweicloud.com/"},
    {"npmmirror registry.npmmirror.com", L"https://registry.npmmirror.com/-/binary/"},
};
inline constexpr int kMirrorCount = 3;

inline int parse_mirror_name(const std::string& s) {
    std::string m = su::lower(s);
    if (m == "auto") return -1;
    if (m == "official") return 0;
    if (m == "huawei") return 1;
    if (m == "npmmirror" || m == "npm") return 2;
    return -2;
}
inline void build_order(int hint, int (&order)[kMirrorCount]) {
    static const int kAuto[kMirrorCount] = {1, 0, 2};
    if (hint >= 0) {
        order[0] = hint;
        order[1] = (hint + 1) % kMirrorCount;
        order[2] = (hint + 2) % kMirrorCount;
    } else {
        for (int i = 0; i < kMirrorCount; ++i) order[i] = kAuto[i];
    }
}
inline std::wstring url_for(int mirror, const std::string& rel_path) {
    std::string rel = rel_path;
    // 华为云/npmmirror 的 python 镜像挂载点没有 ftp/ 前缀：.../python/<版本>/<文件>
    if (mirror != 0 && su::starts_with(rel, "ftp/python/")) rel = "python/" + rel.substr(11);
    return std::wstring(kMirrors[mirror].base) + su::utf8_to_wide(rel);
}
inline std::string mirror_desc(int m) { return kMirrors[m].name; }

// --------------------------------------------------------------- 工具

// FNV-1a：缓存文件名
inline std::string fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[20];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

inline std::string basename_of(const std::string& url) {
    size_t p = url.find_last_of('/');
    return p == std::string::npos ? url : url.substr(p + 1);
}

inline std::string arch_from_filename(const std::string& fn) {
    std::string l = su::lower(fn);
    if (l.find("arm64") != std::string::npos) return "arm64";
    if (l.find("amd64") != std::string::npos || l.find("x64") != std::string::npos)
        return "amd64";
    return "x86";
}

// 本机架构（amd64/arm64/x86）
inline std::string detect_host_arch() {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    if (HMODULE k = GetModuleHandleW(L"kernel32.dll")) {
        auto fn = (IsWow64Process2Fn)(void*)GetProcAddress(k, "IsWow64Process2");
        USHORT proc = 0, machine = 0;
        if (fn && fn(GetCurrentProcess(), &proc, &machine)) {
            if (machine == IMAGE_FILE_MACHINE_ARM64 || proc == IMAGE_FILE_MACHINE_ARM64)
                return "arm64";
        }
    }
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) return "arm64";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) return "x86";
    return "amd64";
}

// 仅正式版目录：X.Y / X.Y.Z / X.Y.Z.W（排除 rc/beta/a 等预发布与杂物）
// 版本号数值比较：按数字段逐段比较（3.10 > 3.9），返回 <0/0/>0
inline std::vector<long> version_parts(const std::string& v) {
    std::vector<long> out;
    long cur = 0;
    bool any = false;
    for (char c : v) {
        if (isdigit((unsigned char)c)) {
            cur = cur * 10 + (c - '0');
            any = true;
        } else if (c == '.') {
            out.push_back(cur);
            cur = 0;
            any = false;
        }
    }
    if (any || out.empty()) out.push_back(cur);
    return out;
}

inline int version_cmp(const std::string& a, const std::string& b) {
    auto pa = version_parts(a);
    auto pb = version_parts(b);
    size_t n = std::max(pa.size(), pb.size());
    pa.resize(n, 0);
    pb.resize(n, 0);
    for (size_t i = 0; i < n; ++i)
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    return 0;
}

inline bool is_stable_version(const std::string& v) {
    if (v.empty()) return false;
    int dots = 0;
    for (char c : v) {
        if (c == '.') {
            if (++dots > 3) return false;
            continue;
        }
        if (!isdigit((unsigned char)c)) return false;
    }
    return true;
}

// --------------------------------------------------------------- HTTP + 缓存

// GET 并缓存到 <exe目录>\cache\python\<hash>.cache（TTL 6h；失败时回退过期缓存）
inline std::string http_get_cached(const std::string& url, std::string& err,
                                   const std::wstring& extra_header = L"") {
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    fs::path cache_dir = fs::path(exe).parent_path() / L"cache" / L"python";
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    fs::path cache_file = cache_dir / su::utf8_to_wide("py_" + fnv1a(url) + ".cache");

    auto read_cache = [&](uint64_t& ts, std::string& body) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, cache_file.c_str(), L"rb") != 0 || !f) return false;
        char header[64] = {};
        bool ok = fgets(header, sizeof(header), f) != nullptr;
        if (ok) ts = (uint64_t)_strtoui64(header, nullptr, 10);
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
        bool bad = ferror(f) != 0;
        fclose(f);
        return !bad && ts > 0;
    };
    auto write_cache = [&](const std::string& body) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, cache_file.c_str(), L"wb") != 0 || !f) return;
        fprintf(f, "%llu\n", (unsigned long long)GetTickCount64());
        fwrite(body.data(), 1, body.size(), f);
        fclose(f);
    };

    uint64_t ts = 0;
    std::string cached;
    bool has_cache = read_cache(ts, cached);
    const uint64_t kTtlMs = 6ull * 3600 * 1000;
    if (has_cache && GetTickCount64() - ts < kTtlMs) {
        logx::linef("缓存命中（%s）", su::wide_to_utf8(cache_file.wstring()).c_str());
        return cached;
    }

    std::vector<std::wstring> headers = {http::kUserAgent};
    if (!extra_header.empty()) headers.push_back(extra_header);
    http::Result r = http::get_text(su::utf8_to_wide(url), headers);
    if (r.ok) {
        write_cache(r.body);
        return r.body;
    }
    err = "HTTP " + std::to_string(r.status) + " " + r.error;
    logx::linef("请求失败 %s → %s", url.c_str(), err.c_str());
    if (has_cache) { // 过期缓存兜底
        logx::line("使用过期缓存兜底");
        return cached;
    }
    return {};
}

// --------------------------------------------------------------- FTP 主源

// 解析目录索引页：href + 邻近的修改日期（Apache 风格索引）
inline bool fetch_ftp_entries(const std::string& url_path,
                              std::vector<std::pair<std::string, std::string>>& out,
                              std::string& err) {
    std::string body = http_get_cached("https://www.python.org/" + url_path, err);
    if (body.empty()) return false;
    size_t pos = 0;
    while ((pos = body.find("href=\"", pos)) != std::string::npos) {
        size_t b = pos + 6;
        size_t e = body.find('"', b);
        if (e == std::string::npos) break;
        std::string href = body.substr(b, e - b);
        pos = e + 1;
        while (!href.empty() && (href.back() == '/' || href.back() == '\\r' ||
                                 href.back() == '\\n'))
            href.pop_back();
        if (href.empty() || href == "." || href == ".." || href[0] == '/' ||
            href.find(':') != std::string::npos || href.find('?') != std::string::npos ||
            href.find('#') != std::string::npos)
            continue;
        // href 之后的片段中找索引日期（Apache 格式：DD-MMM-YYYY，如 02-May-2025）
        std::string date;
        size_t limit = std::min(body.size(), pos + 300);
        static const char* months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        for (size_t p2 = pos; p2 + 11 <= limit; ++p2) {
            if (!isdigit((unsigned char)body[p2]) || !isdigit((unsigned char)body[p2 + 1]) ||
                body[p2 + 2] != '-')
                continue;
            const char* m = body.c_str() + p2 + 3;
            int mi = -1;
            for (int k = 0; k < 12; ++k)
                if (strncmp(m, months[k], 3) == 0) {
                    mi = k;
                    break;
                }
            if (mi >= 0 && body[p2 + 6] == '-' &&
                isdigit((unsigned char)body[p2 + 7]) &&
                isdigit((unsigned char)body[p2 + 8]) &&
                isdigit((unsigned char)body[p2 + 9]) &&
                isdigit((unsigned char)body[p2 + 10])) {
                // DD-MMM-YYYY → YYYY-MM-DD
                static const char* mm[12] = {"01", "02", "03", "04", "05", "06",
                                             "07", "08", "09", "10", "11", "12"};
                date = body.substr(p2 + 7, 4) + "-" + mm[mi] + "-" + body.substr(p2, 2);
                break;
            }
        }
        out.push_back({href, date});
    }
    return !out.empty();
}

// 版本枚举（FTP 主源，1 个请求）
inline bool fetch_versions_ftp(std::vector<PyVersion>& out, std::string& err) {
    std::vector<std::pair<std::string, std::string>> entries;
    if (!fetch_ftp_entries("ftp/python/", entries, err)) {
        err = "无法解析 Python FTP 目录：" + err;
        logx::line(err);
        return false;
    }
    for (auto& [name, date] : entries)
        if (is_stable_version(name)) out.push_back({name, date});
    // 按版本号从高到低（目录修改日期仅作并列时的参考）
    std::stable_sort(out.begin(), out.end(), [](const PyVersion& a, const PyVersion& b) {
        int c = version_cmp(a.version, b.version);
        if (c != 0) return c > 0;
        return a.date > b.date;
    });
    logx::linef("FTP 版本枚举完成：%d 个正式版", (int)out.size());
    return !out.empty();
}

// 某版本的 Windows 文件（FTP 主源，1 个请求/版本）
inline bool fetch_files_ftp(const std::string& version, const std::string& date,
                            std::vector<PyFile>& out, std::string& err) {
    std::vector<std::pair<std::string, std::string>> entries;
    if (!fetch_ftp_entries("ftp/python/" + version + "/", entries, err)) {
        err = "无法解析版本目录 ftp/python/" + version + "/：" + err;
        logx::line(err);
        return false;
    }
    for (auto& [name, fdate] : entries) {
        std::string l = su::lower(name);
        bool win = su::ends_with(l, ".exe") || su::ends_with(l, ".msi") ||
                   su::ends_with(l, ".zip");
        if (!win) continue;
        PyFile pf;
        pf.version = version;
        pf.release_date = fdate.empty() ? date : fdate;
        pf.filename = name;
        pf.url = "https://www.python.org/ftp/python/" + version + "/" + name;
        pf.rel_path = "ftp/python/" + version + "/" + name;
        pf.os_name = "Windows";
        pf.arch = arch_from_filename(name);
        pf.name = "（官方 FTP 目录）";
        out.push_back(std::move(pf));
    }
    logx::linef("FTP 文件枚举: 版本 %s → %d 个 Windows 文件", version.c_str(), (int)out.size());
    return true;
}

// --------------------------------------------------------------- API 可选增强

struct PyApiMeta {
    std::string sha256, md5, date;
    uint64_t filesize = 0;
};

// 拉取 JSON 数组；兼容“整包数组”与“{results, meta.next} 分页”两种形态
inline bool fetch_json_list(const std::string& url, const std::wstring& token_header,
                            std::vector<json::Val>& out, std::string& err) {
    std::string next = url;
    int guard = 0;
    while (!next.empty() && guard++ < 64) {
        std::string body = http_get_cached(next, err, token_header);
        if (body.empty()) return false;
        json::Val root;
        std::string perr;
        if (!json::Parser(body).parse(root, perr)) {
            err = "JSON 解析失败：" + perr;
            return false;
        }
        if (root.t == json::Val::Arr) {
            for (const json::Val& v : root.arr) out.push_back(v);
            return true;
        }
        if (root.t != json::Val::Obj) {
            err = "响应格式异常";
            return false;
        }
        if (const json::Val* e = root.get("error")) {
            err = "API 错误：" + e->str_or();
            return false;
        }
        if (const json::Val* d = root.get("detail")) { // 限流等
            err = "API 提示：" + d->str_or();
            return false;
        }
        const json::Val* results = root.get("results");
        if (results && results->t == json::Val::Arr)
            for (const json::Val& v : results->arr) out.push_back(v);
        const json::Val* meta = root.get("meta");
        next.clear();
        if (meta) {
            if (const json::Val* nx = meta->get("next")) {
                std::string n = nx->str_or();
                if (su::starts_with(n, "http")) next = n;
            }
        }
    }
    return true;
}

// 可选：用 API（需 --py-token）加载元数据表，键 = "<版本>/<文件名>"。
// 匿名调用受严格限流（可能 429），失败不致命 —— FTP 数据照常工作。
inline bool load_api_metadata(const std::string& token,
                              std::map<std::string, PyApiMeta>& out, std::string& err) {
    std::wstring auth = L"Authorization: Token " + su::utf8_to_wide(token);
    std::vector<json::Val> rel_items;
    if (!fetch_json_list("https://www.python.org/api/v2/downloads/release/", auth, rel_items,
                         err))
        return false;
    std::map<std::string, std::string> rel_date; // release uri → 日期
    int stable = 0;
    for (const json::Val& r : rel_items) {
        bool published = r.get("is_published") && r.get("is_published")->boolean_or(false);
        bool pre = r.get("pre_release") && r.get("pre_release")->boolean_or(false);
        if (!published || pre) continue;
        std::string uri = r.get("resource_uri") ? r.get("resource_uri")->str_or() : "";
        std::string ver = r.get("version") ? r.get("version")->str_or() : "";
        std::string date =
            r.get("release_date") ? r.get("release_date")->str_or().substr(0, 10) : "";
        if (!uri.empty() && !ver.empty()) {
            rel_date[uri] = date;
            ++stable;
        }
    }
    std::vector<json::Val> file_items;
    if (!fetch_json_list("https://www.python.org/api/v2/downloads/release_file/", auth,
                         file_items, err))
        return false;
    logx::linef("API 元数据：正式版 %d 个，文件 %d 条", stable, (int)file_items.size());
    int merged = 0;
    for (const json::Val& f : file_items) {
        std::string url = f.get("url") ? f.get("url")->str_or() : "";
        size_t p = url.find("/ftp/python/");
        if (p == std::string::npos) continue;
        std::string rest = url.substr(p + 12); // <版本>/<文件名>
        size_t slash = rest.find('/');
        if (slash == std::string::npos) continue;
        std::string ver = rest.substr(0, slash);
        std::string fn = rest.substr(slash + 1);
        if (ver.empty() || fn.empty()) continue;
        PyApiMeta m;
        m.sha256 = f.get("sha256_sum") ? f.get("sha256_sum")->str_or() : "";
        m.md5 = f.get("md5_sum") ? f.get("md5_sum")->str_or() : "";
        m.filesize = (uint64_t)(f.get("filesize") ? f.get("filesize")->num : 0);
        std::string rel_uri = f.get("release") ? f.get("release")->str_or() : "";
        auto dit = rel_date.find(rel_uri);
        if (dit != rel_date.end()) m.date = dit->second;
        out[ver + "/" + fn] = std::move(m);
        ++merged;
    }
    logx::linef("API 元数据：合并 %d 条", merged);
    return true;
}

// --------------------------------------------------------------- 过滤

enum class PyKind { Installer, Embed, All };

inline bool matches_kind(const PyFile& f, PyKind kind) {
    std::string l = su::lower(f.filename);
    bool is_inst = su::ends_with(l, ".exe") || su::ends_with(l, ".msi");
    bool is_embed = l.find("embed") != std::string::npos && su::ends_with(l, ".zip");
    if (kind == PyKind::Installer) return is_inst;
    if (kind == PyKind::Embed) return is_embed;
    return is_inst || is_embed;
}

// 版本下可用文件（Windows + 架构 + 类型），安装器优先
inline std::vector<const PyFile*> filter_files(const std::vector<PyFile>& files,
                                               const std::string& version,
                                               const std::string& arch, PyKind kind) {
    std::vector<const PyFile*> out;
    for (const PyFile& f : files) {
        if (f.version != version) continue;
        if (f.os_name.find("Windows") == std::string::npos) continue;
        if (!matches_kind(f, kind)) continue;
        if (f.arch != arch) continue;
        out.push_back(&f);
    }
    std::stable_sort(out.begin(), out.end(), [](const PyFile* a, const PyFile* b) {
        auto rank = [](const PyFile* f) {
            std::string l = su::lower(f->filename);
            if (su::ends_with(l, ".exe")) return 0;
            if (su::ends_with(l, ".msi")) return 1;
            return 2;
        };
        return rank(a) < rank(b);
    });
    return out;
}

// --------------------------------------------------------------- 下载/校验/安装

inline bool download(const PyFile& f, int mirror_hint, const fs::path& dest,
                     const std::function<void(uint64_t, uint64_t)>& progress, int& used,
                     std::string& err, const std::function<bool()>& cancelled = nullptr) {
    int order[kMirrorCount];
    build_order(mirror_hint, order);
    for (int i = 0; i < kMirrorCount; ++i) {
        if (cancelled && cancelled()) {
            err = "已取消";
            logx::line("下载已取消（用户）");
            return false;
        }
        int idx = order[i];
        uint64_t resume_at = 0;
        std::error_code ec;
        if (fs::exists(dest, ec)) resume_at = (uint64_t)fs::file_size(dest, ec);
        logx::linef("下载尝试 %d/%d [%s] %s（断点 %s）", i + 1, kMirrorCount,
                    kMirrors[idx].name, su::wide_to_utf8(dest.wstring()).c_str(),
                    su::human_size(resume_at).c_str());
        auto t0 = GetTickCount64();
        http::DownloadResult r = http::download(url_for(idx, f.rel_path), dest.wstring(), true,
                                                {http::kUserAgent}, progress, cancelled);
        uint64_t ms = GetTickCount64() - t0;
        if (r.ok) {
            used = idx;
            logx::linef("下载完成 [%s] %s 用时 %.1fs%s", kMirrors[idx].name,
                        su::human_size(r.written).c_str(), ms / 1000.0,
                        r.resumed ? "（断点续传）" : "");
            return true;
        }
        if (r.cancelled) {
            err = "已取消";
            logx::linef("下载取消 [%s] 已下载 %s", kMirrors[idx].name,
                        su::human_size(r.written).c_str());
            return false;
        }
        logx::linef("下载失败 [%s] HTTP %lu: %s", kMirrors[idx].name, r.status, r.error.c_str());
        err = r.error;
    }
    return false;
}

inline bool verify(const PyFile& f, const fs::path& dest, std::string& err) {
    if (!f.sha256.empty()) {
        std::string got = sha256::file_hex(dest.wstring());
        if (got.empty() || su::lower(got) != su::lower(f.sha256)) {
            err = "SHA-256 校验失败: 期望 " + f.sha256 + " 实际 " +
                  (got.empty() ? "(读取失败)" : got);
            logx::line(err);
            return false;
        }
        logx::linef("SHA-256 校验通过: %s", got.c_str());
        return true;
    }
    if (!f.md5.empty()) {
        std::string got = md5::file_hex(dest.wstring());
        if (got.empty() || su::lower(got) != su::lower(f.md5)) {
            err = "MD5 校验失败: 期望 " + f.md5 + " 实际 " + (got.empty() ? "(读取失败)" : got);
            logx::line(err);
            return false;
        }
        logx::linef("MD5 校验通过: %s", got.c_str());
        return true;
    }
    if (f.filesize > 0) {
        std::error_code ec;
        uint64_t sz = fs::file_size(dest, ec);
        if (sz != f.filesize) {
            err = "文件大小不符: 期望 " + std::to_string(f.filesize) + " 实际 " +
                  std::to_string(sz);
            logx::line(err);
            return false;
        }
        logx::linef("大小比对通过（%s）", su::human_size(sz).c_str());
        return true;
    }
    // FTP 主源：无预知校验和 —— 下载器已保证写入字节数 == Content-Length
    logx::line("提示：无预知校验和，完整性由下载字节数与 Content-Length 比对保证");
    return true;
}

// exe 静默安装（per-user）：TargetDir 指定目录，PrependPath 可选
inline bool install_exe(const fs::path& installer, const fs::path& target_dir, bool prepend_path,
                        std::string& err) {
    std::wstring args = L"/quiet InstallAllUsers=0 TargetDir=\"" + target_dir.wstring() +
                        L"\" Include_test=0 " +
                        (prepend_path ? L"PrependPath=1" : L"PrependPath=0");
    logx::linef("静默安装: %s %s", su::wide_to_utf8(installer.wstring()).c_str(),
                su::wide_to_utf8(args).c_str());
    DWORD code = 0;
    if (!dist::run_hidden(installer, args, code, err)) {
        logx::line("静默安装启动失败: " + err);
        return false;
    }
    if (code != 0 && code != 3010) {
        err = "安装程序退出码 " + std::to_string(code);
        logx::line("静默安装失败: " + err);
        return false;
    }
    logx::linef("静默安装完成（退出码 %lu）", code);
    return true;
}

// zip 解压（经公共解压层：tar.exe 异步 + 轮询进度回调）
inline bool extract_zip(const fs::path& zip, const fs::path& target_dir, std::string& err) {
    if (!archive::extract_to_dir_with_progress(zip, target_dir, L"-xf", err)) {
        logx::line("zip 解压失败: " + err);
        return false;
    }
    logx::linef("zip 解压完成 → %s", su::wide_to_utf8(target_dir.wstring()).c_str());
    return true;
}

// 安装结果验证：目标目录下 python.exe --version
inline bool verify_install(const fs::path& target_dir, std::string& version_out,
                           std::string& err) {
    fs::path exe = target_dir / L"python.exe";
    if (!fs::exists(exe)) {
        err = "目标目录下未找到 python.exe";
        logx::line(err);
        return false;
    }
    version_out = dist::run_capture_first_line(exe, L"--version");
    if (version_out.empty()) {
        err = "python.exe --version 无输出";
        logx::line(err);
        return false;
    }
    logx::line("验证 python: " + version_out);
    return true;
}

// --------------------------------------------------------------- 已装检测

struct InstalledPy {
    std::string version;
    std::wstring path;
};

// 注册表 PythonCore（HKLM + HKCU）：SOFTWARE\Python\PythonCore\<版本>\InstallPath
inline std::vector<InstalledPy> detect_installed() {
    std::vector<InstalledPy> out;
    auto scan = [&](HKEY root) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, L"SOFTWARE\\Python\\PythonCore", 0, KEY_READ, &key) !=
            ERROR_SUCCESS)
            return;
        for (int i = 0;; ++i) {
            wchar_t name[64] = {};
            DWORD len = 64;
            if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS)
                break;
            HKEY ip = nullptr;
            if (RegOpenKeyExW(key, (std::wstring(name) + L"\\InstallPath").c_str(), 0,
                              KEY_QUERY_VALUE, &ip) == ERROR_SUCCESS) {
                wchar_t val[MAX_PATH * 2] = {};
                DWORD size = sizeof(val) - sizeof(wchar_t);
                DWORD type = 0;
                if (RegQueryValueExW(ip, nullptr, nullptr, &type, (BYTE*)val, &size) ==
                        ERROR_SUCCESS &&
                    size > 0) {
                    val[size / 2] = L'\0';
                    std::wstring dir = val;
                    while (!dir.empty() && (dir.back() == L'\0' || dir.back() == L'\\'))
                        dir.pop_back();
                    if (!dir.empty()) out.push_back({su::wide_to_utf8(name), dir});
                }
                RegCloseKey(ip);
            }
        }
        RegCloseKey(key);
    };
    scan(HKEY_LOCAL_MACHINE);
    scan(HKEY_CURRENT_USER);
    for (size_t i = 0; i < out.size(); ++i)
        for (size_t j = out.size(); j-- > i + 1;)
            if (su::lower(out[j].path) == su::lower(out[i].path)) out.erase(out.begin() + j);
    if (out.empty()) logx::line("未在注册表中检测到已安装的 Python");
    else logx::linef("检测到已安装的 Python %d 个", (int)out.size());
    return out;
}

} // namespace py

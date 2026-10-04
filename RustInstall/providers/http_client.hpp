// providers/http_client.hpp : 公共 HTTP 层（WinHTTP 封装 + 磁盘缓存 + 镜像回退下载）
#pragma once

#include <windows.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "http.hpp"
#include "logger.hpp"
#include "strutil.hpp"

namespace httpc {

namespace fs = std::filesystem;

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

inline fs::path cache_dir() {
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    return fs::path(exe).parent_path() / L"cache" / L"sdk";
}

// GET 并缓存到 <exe目录>\cache\sdk\<hash>.cache（TTL 6h；失败时回退过期缓存）
inline std::string get_cached(const std::string& url, std::string& err,
                              const std::vector<std::wstring>& extra_headers = {}) {
    fs::path cache_file = cache_dir() / su::utf8_to_wide("sdk_" + fnv1a(url) + ".cache");
    std::error_code ec;
    fs::create_directories(cache_file.parent_path(), ec);

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
    headers.insert(headers.end(), extra_headers.begin(), extra_headers.end());
    http::Result r = http::get_text(su::utf8_to_wide(url), headers);
    if (r.ok) {
        write_cache(r.body);
        return r.body;
    }
    err = "HTTP " + std::to_string(r.status) + " " + r.error;
    logx::linef("请求失败 %s → %s", url.c_str(), err.c_str());
    if (has_cache) {
        logx::line("使用过期缓存兜底");
        return cached;
    }
    return {};
}

// 文本 GET（带自定义头，用于 Token 认证等）
inline std::string get_text(const std::string& url,
                            const std::vector<std::wstring>& headers, std::string& err) {
    http::Result r = http::get_text(su::utf8_to_wide(url), headers);
    if (!r.ok) err = "HTTP " + std::to_string(r.status) + " " + r.error;
    return r.body;
}

inline std::string strip_bom(const std::string& s) {
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        return s.substr(3);
    return s;
}

// 镜像回退下载：候选依次尝试，断点续传
inline bool download_mirrors(
    const std::vector<std::pair<std::string, std::wstring>>& mirrors, const fs::path& dest,
    const std::function<void(uint64_t, uint64_t)>& progress, int& used, std::string& err,
    const std::function<bool()>& cancelled = nullptr) {
    for (size_t i = 0; i < mirrors.size(); ++i) {
        if (cancelled && cancelled()) {
            err = "已取消";
            logx::line("下载已取消（用户）");
            return false;
        }
        uint64_t resume_at = 0;
        std::error_code ec;
        if (fs::exists(dest, ec)) resume_at = (uint64_t)fs::file_size(dest, ec);
        logx::linef("下载尝试 %d/%d [%s] %s（断点 %s）", (int)(i + 1), (int)mirrors.size(),
                    mirrors[i].first.c_str(), su::wide_to_utf8(dest.wstring()).c_str(),
                    su::human_size(resume_at).c_str());
        auto t0 = GetTickCount64();
        http::DownloadResult r = http::download(mirrors[i].second, dest.wstring(), true,
                                                {http::kUserAgent}, progress, cancelled);
        uint64_t ms = GetTickCount64() - t0;
        if (r.ok) {
            used = (int)i;
            logx::linef("下载完成 [%s] %s 用时 %.1fs%s", mirrors[i].first.c_str(),
                        su::human_size(r.written).c_str(), ms / 1000.0,
                        r.resumed ? "（断点续传）" : "");
            return true;
        }
        if (r.cancelled) {
            err = "已取消";
            logx::linef("下载取消 [%s] 已下载 %s", mirrors[i].first.c_str(),
                        su::human_size(r.written).c_str());
            return false;
        }
        logx::linef("下载失败 [%s] HTTP %lu: %s", mirrors[i].first.c_str(), r.status,
                    r.error.c_str());
        err = r.error;
    }
    return false;
}

} // namespace httpc

// http.hpp : 基于 WinHTTP 的最小 HTTP 客户端
// 提供：GET 文本、HEAD 探测、支持断点续传(Range)与进度回调的文件下载。
#pragma once

#include <winhttp.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "strutil.hpp"

#pragma comment(lib, "winhttp.lib")

namespace http {

namespace detail {

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

inline std::string last_error(const char* what) {
    DWORD e = GetLastError();
    char buf[96];
    snprintf(buf, sizeof(buf), "%s 失败（WinHTTP 错误码 %lu）", what, (unsigned long)e);
    return buf;
}

inline bool query_u64(HINTERNET req, DWORD level, uint64_t& out) {
    DWORD v = 0, size = sizeof(v);
    if (!WinHttpQueryHeaders(req, level | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &v, &size, WINHTTP_NO_HEADER_INDEX))
        return false;
    out = v;
    return true;
}

} // namespace detail

inline const wchar_t* kUserAgent = L"User-Agent: RustInstall/1.0";

// 打开会话/连接/请求并接收响应头
inline bool open_response(const std::wstring& url, const wchar_t* verb,
                          const std::vector<std::wstring>& headers, detail::Handle& ses,
                          detail::Handle& con, detail::Handle& req, unsigned long& status,
                          std::string& err) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512];
    wchar_t path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 511;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4095;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        err = detail::last_error("URL 解析");
        return false;
    }

    ses.h = WinHttpOpen(L"RustInstall/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses.h) {
        err = detail::last_error("初始化网络");
        return false;
    }
    WinHttpSetTimeouts(ses.h, 10000, 20000, 30000, 60000);
    con.h = WinHttpConnect(ses.h, uc.lpszHostName, uc.nPort, 0);
    if (!con.h) {
        err = detail::last_error("连接服务器");
        return false;
    }
    req.h = WinHttpOpenRequest(con.h, verb, uc.lpszUrlPath, nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req.h) {
        err = detail::last_error("创建请求");
        return false;
    }

    std::wstring hdrs;
    for (auto& h : headers) hdrs += h + L"\r\n";
    if (!WinHttpSendRequest(req.h, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
                            (DWORD)hdrs.size(), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        err = detail::last_error("发送请求");
        return false;
    }
    DWORD s = 0, size = sizeof(s);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &s, &size, WINHTTP_NO_HEADER_INDEX))
        status = s;
    return true;
}

// 读取整个响应体（上限 512 MB，防失控）
inline bool read_all(detail::Handle& req, std::string& body, std::string& err) {
    body.clear();
    char buf[65536];
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            err = detail::last_error("查询数据量");
            return false;
        }
        if (avail == 0) return true;
        DWORD take = avail < sizeof(buf) ? avail : sizeof(buf);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf, take, &got) || got == 0) {
            err = detail::last_error("读取数据");
            return false;
        }
        body.append(buf, got);
        if (body.size() > 512ull * 1024 * 1024) {
            err = "响应体过大";
            return false;
        }
    }
}

struct Result {
    bool ok = false;
    unsigned long status = 0;
    std::string body;
    std::string error;
};

inline Result get_text(const std::wstring& url, const std::vector<std::wstring>& headers) {
    Result r;
    detail::Handle ses, con, req;
    if (!open_response(url, L"GET", headers, ses, con, req, r.status, r.error)) return r;
    if (r.status != 200) {
        std::string body;
        std::string e;
        if (read_all(req, body, e)) r.body = std::move(body);
        r.error = "HTTP " + std::to_string(r.status);
        return r;
    }
    if (!read_all(req, r.body, r.error)) return r;
    r.ok = true;
    return r;
}

inline bool head(const std::wstring& url, uint64_t& size, unsigned long& status, std::string& err) {
    detail::Handle ses, con, req;
    size = 0;
    if (!open_response(url, L"HEAD", {kUserAgent}, ses, con, req, status, err)) return false;
    if (status != 200) {
        err = "HTTP " + std::to_string(status);
        return false;
    }
    detail::query_u64(req.h, WINHTTP_QUERY_CONTENT_LENGTH, size);
    return true;
}

struct DownloadResult {
    bool ok = false;
    bool cancelled = false; // 用户取消（已下载部分保留，可用于续传）
    unsigned long status = 0;
    uint64_t total = 0;     // 文件总字节数（服务器未提供则为 0）
    uint64_t written = 0;   // 本次累计写入的字节数（含续传起点）
    bool resumed = false;   // 是否发生了断点续传
    std::string error;
};

// 下载到文件。dest 已存在且 resume=true 时尝试从断点续传。
// progress(done, total) 在每个数据块后回调；total 可能为 0（长度未知）。
// cancelled() 返回 true 时中止下载（保留已下载部分）。
inline DownloadResult download(const std::wstring& url, const std::wstring& dest, bool resume,
                               const std::vector<std::wstring>& headers,
                               const std::function<void(uint64_t, uint64_t)>& progress,
                               const std::function<bool()>& cancelled = nullptr) {
    DownloadResult r;
    uint64_t existing = 0;
    if (resume) {
        HANDLE f = CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0) existing = (uint64_t)sz.QuadPart;
            CloseHandle(f);
        }
    }

    std::vector<std::wstring> hdrs = headers;
    if (existing > 0)
        hdrs.push_back(L"Range: bytes=" + std::to_wstring(existing) + L"-");

    detail::Handle ses, con, req;
    if (!open_response(url, L"GET", hdrs, ses, con, req, r.status, r.error)) return r;

    bool append = false;
    if (r.status == 206) {
        append = true;
        r.resumed = true;
        // Content-Range: bytes N-M/T → 总长 T
        DWORD sz = 0;
        wchar_t cr[128] = {};
        sz = sizeof(cr) - sizeof(wchar_t);
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX,
                                cr, &sz, WINHTTP_NO_HEADER_INDEX)) {
            std::wstring s(cr);
            size_t slash = s.rfind(L'/');
            if (slash != std::wstring::npos) r.total = (uint64_t)_wtoi64(s.c_str() + slash + 1);
        }
    } else if (r.status == 200) {
        existing = 0; // 服务器不支持续传，从头下载
    } else if (r.status == 416 && existing > 0) {
        // 本地文件可能已经完整：与服务器长度比对，一致即视为完成
        uint64_t hsize = 0;
        unsigned long hstatus = 0;
        std::string herr;
        if (head(url, hsize, hstatus, herr) && hsize == existing) {
            r.ok = true;
            r.total = hsize;
            r.written = existing;
            return r;
        }
        return download(url, dest, false, headers, progress, cancelled); // 否则从头重下
    } else {
        r.error = "HTTP " + std::to_string(r.status);
        return r;
    }
    if (r.total == 0) detail::query_u64(req.h, WINHTTP_QUERY_CONTENT_LENGTH, r.total);

    HANDLE f = CreateFileW(dest.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        r.error = "无法创建文件 " + su::wide_to_utf8(dest);
        return r;
    }
    if (append) {
        LARGE_INTEGER li{};
        SetFilePointerEx(f, li, nullptr, FILE_END);
    }

    std::string buf(256 * 1024, '\0');
    uint64_t done = existing;
    for (;;) {
        if (cancelled && cancelled()) {
            r.cancelled = true;
            r.error = "已取消";
            CloseHandle(f);
            return r;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            r.error = detail::last_error("查询数据量");
            CloseHandle(f);
            return r;
        }
        if (avail == 0) break;
        DWORD take = avail < (DWORD)buf.size() ? avail : (DWORD)buf.size();
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), take, &got) || got == 0) {
            r.error = detail::last_error("读取数据");
            CloseHandle(f);
            return r;
        }
        DWORD put = 0;
        if (!WriteFile(f, buf.data(), got, &put, nullptr) || put != got) {
            r.error = "写入文件失败（磁盘空间不足？）";
            CloseHandle(f);
            return r;
        }
        done += put;
        if (progress) progress(done, r.total);
    }
    CloseHandle(f);
    r.written = done;
    if (r.total > 0 && done != r.total) {
        r.error = "下载不完整（" + std::to_string(done) + "/" + std::to_string(r.total) + " 字节）";
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace http

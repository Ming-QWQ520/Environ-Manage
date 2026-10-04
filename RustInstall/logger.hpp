// logger.hpp : 轻量日志（带时间戳，线程安全）
// 用法：logx::linef("...%s...", x) 写一行；目录确定后调用 logx::set_file(path)
// 打开日志文件（追加），此前缓冲的行会一并落盘。
#pragma once

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace logx {

inline std::mutex g_mu;
inline FILE* g_file = nullptr;
inline std::vector<std::string> g_buffer; // set_file 之前的缓冲

inline void write_line(const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[40];
    snprintf(ts, sizeof(ts), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::string line = std::string(ts) + msg + "\n";
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) {
        fputs(line.c_str(), g_file);
        fflush(g_file);
    } else {
        g_buffer.push_back(line);
    }
}

inline void line(const std::string& msg) { write_line(msg); }

inline void linef(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write_line(buf);
}

// 打开日志文件（追加模式）；调用前的日志从缓冲落盘。重复调用无效。
inline void set_file(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    g_file = f;
    fputs("============================================================\n", g_file);
    if (!g_buffer.empty()) { // 缓冲中的开场日志夹在分隔线之间
        for (const std::string& l : g_buffer) fputs(l.c_str(), g_file);
        g_buffer.clear();
        fputs("============================================================\n", g_file);
    }
    fflush(g_file);
}

inline void close() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

} // namespace logx

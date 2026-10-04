// console_ui.hpp : 批处理（脚本）模式专用的控制台输出助手
// 交互模式（TUI）由 FTXUI 库实现，见 RustInstall.cpp 中的 tui 命名空间。
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "strutil.hpp"

namespace ui {

// 控制台切换到 UTF-8 并启用 ANSI 转义（批处理模式的彩色输出需要）
inline void init() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    if (HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE)) {
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleTitleW(L"Environ Manage（环境管理器）");
}

constexpr const char* kReset = "\x1b[0m";
constexpr const char* kDim = "\x1b[2m";
constexpr const char* kCyan = "\x1b[1;36m";
constexpr const char* kGreen = "\x1b[1;32m";
constexpr const char* kRed = "\x1b[1;31m";
constexpr const char* kYellow = "\x1b[1;33m";

// 进度条文本行
inline std::string progress_line(uint64_t done, uint64_t total, double bps) {
    const int W = 24;
    char bar[W + 1];
    int fill = total > 0 ? (int)(W * done / total) : 0;
    if (fill > W) fill = W;
    for (int i = 0; i < W; ++i) bar[i] = i < fill ? '#' : '-';
    bar[W] = '\0';
    char line[192];
    if (total > 0)
        snprintf(line, sizeof(line), "[%s] %5.1f%%  %s / %s  %s/s", bar,
                 100.0 * (double)done / (double)total, su::human_size(done).c_str(),
                 su::human_size(total).c_str(), su::human_size((uint64_t)bps).c_str());
    else
        snprintf(line, sizeof(line), "%s  %s/s", su::human_size(done).c_str(),
                 su::human_size((uint64_t)bps).c_str());
    return line;
}

// 批处理模式进度：\r 单行刷新
inline void progress(uint64_t done, uint64_t total, double bps) {
    char line[256];
    snprintf(line, sizeof(line), "\r  %s", progress_line(done, total, bps).c_str());
    fputs(line, stdout);
    fflush(stdout);
}

inline void progress_done(uint64_t total, bool ok) {
    printf("\r  %s%s%s  %s\n\n", ok ? kGreen : kRed, ok ? "[下载完成]" : "[下载失败]", kReset,
           su::human_size(total).c_str());
}

} // namespace ui

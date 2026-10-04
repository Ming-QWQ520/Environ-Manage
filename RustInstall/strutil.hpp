// strutil.hpp : 字符串编码转换与基础工具（全程序统一使用 UTF-8 窄字符串）
#pragma once

#include <windows.h>

#include <algorithm>
#include <string>

namespace su {

inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

inline bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

inline std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

inline bool icontains(const std::wstring& hay, const std::wstring& needle) {
    return lower(hay).find(lower(needle)) != std::wstring::npos;
}

// 人类可读的文件大小
inline std::string human_size(uint64_t b) {
    char buf[64];
    double v = (double)b;
    if (v >= 1073741824.0)      snprintf(buf, sizeof(buf), "%.2f GB", v / 1073741824.0);
    else if (v >= 1048576.0)    snprintf(buf, sizeof(buf), "%.1f MB", v / 1048576.0);
    else if (v >= 1024.0)       snprintf(buf, sizeof(buf), "%.1f KB", v / 1024.0);
    else                        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
    return buf;
}

// 分号/分号分隔的 PATH 里是否已包含某个目录（大小写不敏感，忽略尾部斜杠差异）
inline bool path_list_contains(const std::wstring& list, const std::wstring& dir) {
    std::wstring want = lower(dir);
    while (!want.empty() && want.back() == L'\\') want.pop_back();
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t next = list.find(L';', pos);
        std::wstring item = list.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
        item = lower(trim(item));
        while (!item.empty() && item.back() == L'\\') item.pop_back();
        if (item == want) return true;
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    return false;
}

} // namespace su

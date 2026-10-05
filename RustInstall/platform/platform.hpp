// platform/platform.hpp : 平台操作层 —— PATH、环境变量、目录 junction、进程执行
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../json.hpp"
#include "logger.hpp"
#include "strutil.hpp"

namespace platform {

namespace fs = std::filesystem;

inline bool ensure_dir(const fs::path& dir, std::string& err) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        err = "无法创建目录 " + su::wide_to_utf8(dir.wstring()) + "：" + ec.message();
        return false;
    }
    return true;
}

// 目录 junction（无需管理员权限）；已存在则先移除链接本身
inline bool make_junction(const fs::path& link, const fs::path& target, std::string& err) {
    std::error_code ec;
    if (fs::exists(link, ec) || fs::is_symlink(link, ec)) fs::remove(link, ec);
    fs::create_directories(link.parent_path(), ec);
    std::wstring cmd = L"/c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = "无法启动 cmd.exe 创建 junction";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0 || !fs::exists(link, ec)) {
        err = "创建 junction 失败（退出码 " + std::to_string(code) + "）";
        return false;
    }
    logx::linef("junction: %s → %s", su::wide_to_utf8(link.wstring()).c_str(),
                su::wide_to_utf8(target.wstring()).c_str());
    return true;
}

// 设置用户环境变量（HKCU\Environment，覆盖语义），并广播变更
inline bool set_user_env(const std::string& name, const std::wstring& value, std::string& err) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        err = "打开注册表 HKCU\\Environment 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    std::wstring wname = su::utf8_to_wide(name);
    rc = RegSetValueExW(key, wname.c_str(), 0, REG_SZ, (const BYTE*)value.c_str(),
                        (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        err = "设置环境变量 " + name + " 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    DWORD_PTR resp = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 3000, &resp);
    logx::linef("已设置环境变量 %s=%s", name.c_str(), su::wide_to_utf8(value).c_str());
    return true;
}

// 把目录加入用户 PATH（HKCU\Environment，存在则跳过）
inline bool add_path_dir(const fs::path& dir, std::string& err) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        err = "打开注册表 HKCU\\Environment 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    std::wstring cur;
    DWORD type = 0, size = 0;
    rc = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size);
    if (rc == ERROR_SUCCESS && size > 0) {
        cur.resize(size / 2);
        RegQueryValueExW(key, L"Path", nullptr, &type, (BYTE*)cur.data(), &size);
        while (!cur.empty() && cur.back() == L'\0') cur.pop_back();
    }
    std::wstring wdir = dir.wstring();
    bool exists = false;
    {
        std::wstring low_cur = su::lower(cur), low_dir = su::lower(wdir);
        if (low_cur.find(low_dir) != std::wstring::npos) exists = true;
    }
    if (exists) {
        RegCloseKey(key);
        logx::line("PATH 已包含该目录，无需修改");
        return true;
    }
    if (!cur.empty() && cur.back() != L';') cur += L";";
    cur += wdir;
    rc = RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ, (const BYTE*)cur.c_str(),
                        (DWORD)((cur.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        err = "写入用户 PATH 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    DWORD_PTR resp = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 3000, &resp);
    logx::linef("已写入用户 PATH: %s", su::wide_to_utf8(wdir).c_str());
    return true;
}

// 隐藏窗口运行进程并等待
// 启动隐藏进程（异步）：成功返回进程句柄供 WaitForSingleObject 轮询等待，
// 结束后需 CloseHandle(pi.hProcess) 与 CloseHandle(pi.hThread)；失败返回 nullptr
inline HANDLE run_hidden_async(const fs::path& exe, const std::wstring& args,
                               PROCESS_INFORMATION& pi, std::string& err) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        err = "无法启动 " + su::wide_to_utf8(exe.wstring());
        return nullptr;
    }
    return pi.hProcess;
}

inline bool run_hidden(const fs::path& exe, const std::wstring& args, DWORD& exit_code,
                       std::string& err) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        err = "无法启动 " + su::wide_to_utf8(exe.wstring());
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// 运行命令并捕获首行输出
inline std::string capture_first_line(const fs::path& exe, const std::wstring& args,
                                      unsigned timeout_ms = 15000) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return {};
    }
    std::string out;
    DWORD waited = 0;
    bool exited = false;
    while (waited < timeout_ms) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD got = 0;
            if (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
            else break;
            if (out.size() > 64 * 1024) break;
        }
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            exited = true;
            for (;;) {
                DWORD avail = 0;
                if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
                char buf[4096];
                DWORD got = 0;
                if (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
                else break;
            }
            break;
        }
        waited += 100;
    }
    if (!exited) TerminateProcess(pi.hProcess, 1);
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    size_t end = out.find('\n');
    return su::trim(out.substr(0, end == std::string::npos ? out.size() : end));
}

// 从用户 PATH 移除目录（大小写不敏感、忽略末尾反斜杠）；整值为空时删除 Path 值
inline bool remove_path_dir(const fs::path& dir, std::string& err) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        err = "打开注册表 HKCU\\Environment 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    std::wstring cur;
    DWORD type = 0, size = 0;
    rc = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size);
    if (rc == ERROR_SUCCESS && size > 0) {
        cur.resize(size / 2);
        RegQueryValueExW(key, L"Path", nullptr, &type, (BYTE*)cur.data(), &size);
        while (!cur.empty() && cur.back() == L'\0') cur.pop_back();
    }
    RegCloseKey(key);

    // 规范比较：小写 + 去末尾反斜杠
    auto norm = [](std::wstring s) {
        s = su::lower(su::trim(s));
        while (!s.empty() && s.back() == L'\\') s.pop_back();
        return s;
    };
    std::wstring want = norm(dir.wstring());
    std::vector<std::wstring> keep;
    bool removed = false;
    size_t pos = 0;
    while (pos <= cur.size()) {
        size_t next = cur.find(L';', pos);
        std::wstring item = su::trim(
            cur.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos));
        if (!item.empty()) {
            if (norm(item) == want) removed = true; // 丢弃匹配项
            else keep.push_back(item);
        }
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    if (!removed) {
        logx::line("PATH 中未找到该目录，无需移除");
        return true;
    }
    std::wstring joined;
    for (size_t i = 0; i < keep.size(); ++i) {
        if (i) joined += L";";
        joined += keep[i];
    }
    key = nullptr;
    rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        err = "打开注册表 HKCU\\Environment 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    if (joined.empty()) {
        RegDeleteValueW(key, L"Path");
    } else {
        rc = RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ, (const BYTE*)joined.c_str(),
                            (DWORD)((joined.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS && !joined.empty()) {
        err = "写回用户 PATH 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    DWORD_PTR resp = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 3000, &resp);
    logx::linef("已从用户 PATH 移除: %s", su::wide_to_utf8(dir.wstring()).c_str());
    return true;
}

// 删除用户环境变量（HKCU\Environment），并广播变更；不存在视为成功
inline bool remove_user_env(const std::string& name, std::string& err) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        err = "打开注册表 HKCU\\Environment 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    rc = RegDeleteValueW(key, su::utf8_to_wide(name).c_str());
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        err = "删除环境变量 " + name + " 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    DWORD_PTR resp = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 3000, &resp);
    logx::linef("已删除环境变量 %s", name.c_str());
    return true;
}

// ------------------------------------------------------- 受管安装记录（JSON 文件，不写注册表）
//
// 每种语言一个 JSON 文件，创建于应用所在目录（项目文件夹根目录，即 exe 同级）：
//   Environ-Manage-<语言名称>.json（如 Environ-Manage-Flutter.json）
//
// 文件结构：
// {
//   "app":        "Environ-Manage",
//   "id":         "flutter",             // 语言 id（与 Provider 一致）
//   "language":   "Flutter",             // 语言显示名（与文件名一致）
//   "last_root":  "D:\\DevEnv",          // 该语言最近一次使用的根目录（路径屏预填）
//   "roots":      ["D:\\DevEnv"],        // 受本工具管理的安装根目录
//   "values":     { "mirror": "1" },     // 语言级偏好（如 Flutter 下载镜像下标）
//   "updated_at": "2026-10-05 14:30:22"
// }
//
// 旧版本写在 HKCU\Software\EnvironManage 的记录会在首次读取时自动迁移到 JSON
// （迁移只读注册表），成功后清理对应旧键；此后语言管理不再写入注册表。

inline std::wstring managed_key(const std::string& lang) {
    return L"Software\\EnvironManage\\" + su::utf8_to_wide(lang);
}

// 应用所在目录（项目文件夹根目录）：记录文件 / 缓存 / 日志的基准目录
inline fs::path app_dir() {
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    if (!exe[0]) return fs::path(L".");
    return fs::path(exe).parent_path();
}

// 语言 id → 显示名（与 TUI 首页 kLangs 一致，用作记录文件名）
inline const char* managed_display_name(const std::string& lang) {
    static const struct { const char* id; const char* name; } kNames[] = {
        {"rust", "Rust"},         {"python", "Python"},
        {"node", "Node.js"},      {"jdk", "JDK (Temurin)"},
        {"go", "Go"},             {"dotnet", ".NET"},
        {"zig", "Zig"},           {"php", "PHP"},
        {"ruby", "Ruby"},         {"git", "Git For Windows"},
        {"flutter", "Flutter"},
    };
    for (const auto& e : kNames)
        if (lang == e.id) return e.name;
    return lang.c_str(); // 未知语言退回 id（仅在本次调用内有效）
}

// 记录文件名组件安全化（防御显示名引入 Windows 文件名非法字符）
inline std::wstring managed_filename_component(const std::string& utf8) {
    std::wstring w = su::utf8_to_wide(utf8);
    for (wchar_t& c : w)
        if (c < 0x20 || std::wcschr(L"<>:\"/\\|?*", c)) c = L'-';
    return w;
}

inline fs::path managed_json_path(const std::string& lang) {
    std::wstring name =
        L"Environ-Manage-" + managed_filename_component(managed_display_name(lang)) + L".json";
    return app_dir() / name;
}

struct ManagedRecord {
    std::string id;         // 语言 id
    std::string language;   // 语言显示名
    std::wstring last_root; // 最近一次使用的根目录
    std::vector<std::wstring> roots;                          // 受管根目录
    std::vector<std::pair<std::string, std::wstring>> values; // 语言级偏好（键 → 值）
};

// JSON 字符串转义（记录文件写出用）
inline std::string json_escape_str(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    char buf[8];
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

inline std::string managed_now_stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[48];
    snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    return ts;
}

inline std::string managed_to_json(const ManagedRecord& r) {
    std::string o;
    o += "{\n";
    o += "  \"app\": \"Environ-Manage\",\n";
    o += "  \"id\": \"" + json_escape_str(r.id) + "\",\n";
    o += "  \"language\": \"" + json_escape_str(r.language) + "\",\n";
    o += "  \"last_root\": \"" + json_escape_str(su::wide_to_utf8(r.last_root)) + "\",\n";
    o += "  \"roots\": [";
    for (size_t i = 0; i < r.roots.size(); ++i) {
        if (i) o += ", ";
        o += "\"" + json_escape_str(su::wide_to_utf8(r.roots[i])) + "\"";
    }
    o += "],\n";
    o += "  \"values\": {";
    for (size_t i = 0; i < r.values.size(); ++i) {
        if (i) o += ", ";
        o += "\"" + json_escape_str(r.values[i].first) +
             "\": \"" + json_escape_str(su::wide_to_utf8(r.values[i].second)) + "\"";
    }
    o += "},\n";
    o += "  \"updated_at\": \"" + managed_now_stamp() + "\"\n";
    o += "}\n";
    return o;
}

inline bool managed_from_json(const std::string& text, ManagedRecord& r, std::string& err) {
    json::Parser p(text);
    json::Val v;
    if (!p.parse(v, err)) return false;
    if (v.t != json::Val::Obj) {
        err = "记录文件根不是 JSON 对象";
        return false;
    }
    ManagedRecord t;
    if (const json::Val* x = v.get("id")) t.id = x->str_or();
    if (const json::Val* x = v.get("language")) t.language = x->str_or();
    if (const json::Val* x = v.get("last_root")) t.last_root = su::utf8_to_wide(x->str_or());
    if (const json::Val* x = v.get("roots")) {
        if (x->t == json::Val::Arr)
            for (const json::Val& e : x->arr)
                if (e.t == json::Val::Str && !e.s.empty())
                    t.roots.push_back(su::utf8_to_wide(e.s));
    }
    if (const json::Val* x = v.get("values")) {
        if (x->t == json::Val::Obj)
            for (const auto& kv : x->kv)
                if (kv.second.t == json::Val::Str)
                    t.values.emplace_back(kv.first, su::utf8_to_wide(kv.second.s));
    }
    r = std::move(t);
    return true;
}

inline bool managed_read_file(const fs::path& p, std::string& out) {
    std::error_code ec;
    uintmax_t sz = fs::file_size(p, ec);
    if (ec) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.resize((size_t)sz);
    if (sz > 0) f.read(out.data(), (std::streamsize)sz);
    out.resize((size_t)f.gcount());
    return true;
}

// 原子写出：先写 .tmp 再替换，避免检测线程读到半截文件
inline bool managed_write_file(const fs::path& p, const std::string& data, std::string& err) {
    std::error_code ec;
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "无法创建记录文件 " + su::wide_to_utf8(tmp.wstring());
            return false;
        }
        f.write(data.data(), (std::streamsize)data.size());
        f.flush();
        if (!f) {
            err = "写入记录文件失败 " + su::wide_to_utf8(tmp.wstring());
            f.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    ec.clear();
    fs::rename(tmp, p, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(p, ec2); // 目标已存在时先移除再替换
        fs::rename(tmp, p, ec);
        if (ec) {
            err = "替换记录文件失败 " + su::wide_to_utf8(p.wstring()) + "：" + ec.message();
            return false;
        }
    }
    return true;
}

inline std::mutex& managed_mu() {
    static std::mutex m;
    return m;
}

// 保存记录（需持锁）：全空记录直接删除文件；调用前补齐 id / language
inline bool managed_save_locked(const std::string& lang, ManagedRecord& r, std::string& err) {
    if (r.id.empty()) r.id = lang;
    if (r.language.empty()) r.language = managed_display_name(lang);
    if (r.roots.empty() && r.last_root.empty() && r.values.empty()) {
        std::error_code ec;
        fs::remove(managed_json_path(lang), ec);
        return true;
    }
    return managed_write_file(managed_json_path(lang), managed_to_json(r), err);
}

// 旧注册表迁移（需持锁，只读注册表）：HKCU\Software\EnvironManage\<lang> → JSON
// 迁移成功后清理旧键（语言管理自此不再使用注册表）；写 JSON 失败则保留注册表数据下次重试
inline bool managed_migrate_locked(const std::string& lang, ManagedRecord& r) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, managed_key(lang).c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return false; // 无旧记录

    ManagedRecord old;
    old.id = lang;
    old.language = managed_display_name(lang);

    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, L"roots", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        size >= sizeof(wchar_t) * 2) {
        std::wstring data;
        data.resize(size / 2);
        if (RegQueryValueExW(key, L"roots", nullptr, &type, (BYTE*)data.data(), &size) ==
            ERROR_SUCCESS) {
            size_t pos = 0;
            while (pos < data.size()) {
                size_t end = data.find(L'\0', pos);
                if (end == std::wstring::npos) end = data.size();
                std::wstring item = data.substr(pos, end - pos);
                if (!item.empty()) old.roots.push_back(item);
                if (end == data.size() || end + 1 >= data.size()) break;
                pos = end + 1;
            }
        }
    }
    {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD sz = sizeof(buf) - sizeof(wchar_t);
        if (RegQueryValueExW(key, L"last_root", nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS)
            old.last_root = buf;
    }
    // 其余 REG_SZ 值（如 flutter\mirror）一并迁移
    for (DWORD i = 0;; ++i) {
        wchar_t name[128] = {};
        DWORD nlen = 128, dsz = 1024, vtype = 0;
        BYTE data[1024] = {};
        if (RegEnumValueW(key, i, name, &nlen, nullptr, &vtype, data, &dsz) != ERROR_SUCCESS)
            break;
        std::wstring nm(name, nlen);
        if (nm == L"roots" || nm == L"last_root") continue;
        if (vtype != REG_SZ && vtype != REG_EXPAND_SZ) continue;
        std::wstring val((const wchar_t*)data, dsz / sizeof(wchar_t));
        while (!val.empty() && val.back() == L'\0') val.pop_back();
        old.values.emplace_back(su::wide_to_utf8(nm), std::move(val));
    }
    RegCloseKey(key);

    if (old.roots.empty() && old.last_root.empty() && old.values.empty()) return false;

    std::string err;
    if (!managed_save_locked(lang, old, err)) {
        logx::line("旧注册表记录迁移到 JSON 失败 [" + lang + "]：" + err +
                   "（保留注册表数据，下次自动重试）");
        return false;
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, managed_key(lang).c_str());
    logx::linef("旧注册表记录已迁移至 %s（旧注册表键已清理）",
                su::wide_to_utf8(managed_json_path(lang).wstring()).c_str());
    r = std::move(old);
    return true;
}

// 加载记录（需持锁）：优先 JSON；不存在时尝试旧注册表迁移
inline bool managed_load_locked(const std::string& lang, ManagedRecord& r) {
    r = ManagedRecord{};
    std::string text;
    if (managed_read_file(managed_json_path(lang), text)) {
        std::string err;
        if (managed_from_json(text, r, err)) {
            if (r.id.empty()) r.id = lang;
            if (r.language.empty()) r.language = managed_display_name(lang);
            return true;
        }
        logx::line("受管记录文件解析失败 " + su::wide_to_utf8(managed_json_path(lang).wstring()) +
                   "：" + err);
        return false; // 视为无记录；不自动覆盖，保留现场便于排查
    }
    return managed_migrate_locked(lang, r);
}

inline std::vector<std::wstring> managed_get_roots(const std::string& lang) {
    std::lock_guard<std::mutex> g(managed_mu());
    ManagedRecord r;
    managed_load_locked(lang, r);
    return r.roots;
}

inline bool managed_set_roots(const std::string& lang, const std::vector<std::wstring>& roots,
                              std::string& err) {
    std::lock_guard<std::mutex> g(managed_mu());
    ManagedRecord r;
    managed_load_locked(lang, r);
    r.roots = roots;
    if (!managed_save_locked(lang, r, err)) {
        err = "写入受管记录文件失败：" + err;
        return false;
    }
    return true;
}

inline bool managed_add_root(const std::string& lang, const fs::path& root) {
    std::wstring w = root.wstring();
    while (!w.empty() && w.back() == L'\\') w.pop_back();
    std::lock_guard<std::mutex> g(managed_mu());
    ManagedRecord r;
    managed_load_locked(lang, r);
    std::wstring low = su::lower(w);
    for (const std::wstring& x : r.roots)
        if (su::lower(x) == low) return true;
    r.roots.push_back(w);
    std::string err;
    if (!managed_save_locked(lang, r, err)) {
        logx::line("记录受管目录失败: " + err);
        return false;
    }
    logx::linef("已记录受管目录 [%s]: %s", lang.c_str(), su::wide_to_utf8(w).c_str());
    return true;
}

inline bool managed_remove_root(const std::string& lang, const fs::path& root) {
    std::wstring w = root.wstring();
    while (!w.empty() && w.back() == L'\\') w.pop_back();
    std::lock_guard<std::mutex> g(managed_mu());
    ManagedRecord r;
    managed_load_locked(lang, r);
    std::vector<std::wstring> keep;
    std::wstring low = su::lower(w);
    for (const std::wstring& x : r.roots)
        if (su::lower(x) != low) keep.push_back(x);
    if (keep.size() == r.roots.size()) return true;
    r.roots = std::move(keep);
    std::string err;
    if (!managed_save_locked(lang, r, err)) {
        logx::line("更新受管目录记录失败: " + err);
        return false;
    }
    logx::linef("已移除受管目录记录 [%s]: %s", lang.c_str(), su::wide_to_utf8(w).c_str());
    return true;
}

inline bool managed_set_last_root(const std::string& lang, const fs::path& root) {
    std::wstring w = root.wstring();
    while (!w.empty() && w.back() == L'\\') w.pop_back();
    std::lock_guard<std::mutex> g(managed_mu());
    ManagedRecord r;
    managed_load_locked(lang, r);
    r.last_root = w;
    std::string err;
    return managed_save_locked(lang, r, err);
}

// ---- 语言级偏好（记录文件 values 节，如 flutter mirror = 镜像下标） ----
// 空值语义为清除该偏好项
inline bool managed_set_value(const std::string& lang, const wchar_t* name,
                              const std::wstring& value, std::string& err) {
    std::lock_guard<std::mutex> g(managed_mu());
    ManagedRecord r;
    managed_load_locked(lang, r);
    std::string key = su::wide_to_utf8(name);
    bool found = false;
    for (auto it = r.values.begin(); it != r.values.end(); ++it) {
        if (it->first == key) {
            found = true;
            if (value.empty())
                r.values.erase(it);
            else
                it->second = value;
            break;
        }
    }
    if (!found && !value.empty()) r.values.emplace_back(key, value);
    if (!managed_save_locked(lang, r, err)) {
        err = "写入语言偏好失败：" + err;
        return false;
    }
    return true;
}

inline std::wstring managed_get_value(const std::string& lang, const wchar_t* name) {
    std::lock_guard<std::mutex> g(managed_mu());
    ManagedRecord r;
    managed_load_locked(lang, r);
    std::string key = su::wide_to_utf8(name);
    for (const auto& kv : r.values)
        if (kv.first == key) return kv.second;
    return {};
}

inline std::wstring managed_get_last_root(const std::string& lang) {
    std::lock_guard<std::mutex> g(managed_mu());
    ManagedRecord r;
    managed_load_locked(lang, r);
    return r.last_root;
}

// 在当前进程 PATH 中查找可执行文件，返回首个命中的完整路径（未找到返回空）
inline fs::path find_in_path(const std::wstring& exe_name) {
    wchar_t pathv[32768] = {};
    GetEnvironmentVariableW(L"PATH", pathv, 32768);
    std::wstring dirs(pathv);
    std::error_code ec;
    size_t pos = 0;
    while (pos <= dirs.size()) {
        size_t next = dirs.find(L';', pos);
        std::wstring d = su::trim(
            dirs.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos));
        if (!d.empty()) {
            fs::path p = fs::path(d) / exe_name;
            if (fs::is_regular_file(p, ec)) return p;
        }
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    return {};
}

// 体检用户 PATH：移除指向不存在目录的条目（先展开环境变量再判断存在性）
// total 为原始非空条目数；返回移除数量（0 = 无失效项，不动注册表）
inline int fix_user_path(int& total, std::string& err) {
    total = 0;
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        err = "打开注册表 HKCU\\Environment 失败（错误码 " + std::to_string(rc) + "）";
        return -1;
    }
    std::wstring cur;
    DWORD type = 0, size = 0;
    rc = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size);
    if (rc == ERROR_SUCCESS && size > 0) {
        cur.resize(size / 2);
        RegQueryValueExW(key, L"Path", nullptr, &type, (BYTE*)cur.data(), &size);
        while (!cur.empty() && cur.back() == L'\0') cur.pop_back();
    }
    RegCloseKey(key);

    std::vector<std::wstring> keep;
    int removed = 0;
    size_t pos = 0;
    while (pos <= cur.size()) {
        size_t next = cur.find(L';', pos);
        std::wstring item = su::trim(
            cur.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos));
        if (!item.empty()) {
            ++total;
            wchar_t buf[1024] = {};
            ExpandEnvironmentStringsW(item.c_str(), buf, 1024);
            std::error_code ec;
            if (fs::is_directory(fs::path(buf), ec))
                keep.push_back(item);
            else
                ++removed;
        }
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    if (removed == 0) return 0;

    std::wstring joined;
    for (size_t i = 0; i < keep.size(); ++i) {
        if (i) joined += L";";
        joined += keep[i];
    }
    key = nullptr;
    rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        err = "打开注册表 HKCU\\Environment 失败（错误码 " + std::to_string(rc) + "）";
        return -1;
    }
    if (joined.empty())
        RegDeleteValueW(key, L"Path");
    else
        rc = RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ, (const BYTE*)joined.c_str(),
                            (DWORD)((joined.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS && !joined.empty()) {
        err = "写回用户 PATH 失败（错误码 " + std::to_string(rc) + "）";
        return -1;
    }
    DWORD_PTR resp = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 3000, &resp);
    logx::linef("PATH 体检：清理失效目录 %d 项", removed);
    return removed;
}

// 递归移动/合并目录内容
inline bool move_tree(const fs::path& src, const fs::path& dst_parent, std::string& err) {
    fs::path dst = dst_parent / src.filename();
    std::error_code ec;
    if (fs::is_directory(src)) {
        fs::create_directories(dst, ec);
        if (ec) {
            err = "无法创建目录 " + su::wide_to_utf8(dst.wstring()) + "：" + ec.message();
            return false;
        }
        for (const fs::directory_entry& e : fs::directory_iterator(src))
            if (!move_tree(e.path(), dst, err)) return false;
        fs::remove(src, ec);
        return true;
    }
    fs::create_directories(dst.parent_path(), ec);
    if (fs::exists(dst)) fs::remove(dst, ec);
    fs::rename(src, dst, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove(src, ec);
    }
    if (ec) {
        err = "无法移动 " + su::wide_to_utf8(src.wstring()) + "：" + ec.message();
        return false;
    }
    return true;
}

} // namespace platform

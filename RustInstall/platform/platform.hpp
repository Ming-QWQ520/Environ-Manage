// platform/platform.hpp : 平台操作层 —— PATH、环境变量、目录 junction、进程执行
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

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

// ------------------------------------------------------- 受管安装记录（HKCU\Software\EnvironManage）
//
// 每种语言一个子键：<lang>（rust/python/node/jdk/go/dotnet）
//   roots     REG_MULTI_SZ  受本工具管理的安装根目录（统一布局 <root>\<版本> + <root>\current）
//   last_root REG_SZ        该语言最近一次使用的根目录（路径屏预填）

inline std::wstring managed_key(const std::string& lang) {
    return L"Software\\EnvironManage\\" + su::utf8_to_wide(lang);
}

inline std::vector<std::wstring> managed_get_roots(const std::string& lang) {
    std::vector<std::wstring> out;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, managed_key(lang).c_str(), 0, KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS)
        return out;
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
                if (!item.empty()) out.push_back(item);
                if (end == data.size() || end + 1 >= data.size()) break;
                pos = end + 1;
            }
        }
    }
    RegCloseKey(key);
    return out;
}

inline bool managed_set_roots(const std::string& lang, const std::vector<std::wstring>& roots,
                              std::string& err) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, managed_key(lang).c_str(), 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        err = "打开注册表 Software\\EnvironManage 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    if (roots.empty()) {
        RegDeleteValueW(key, L"roots");
        RegCloseKey(key);
        return true;
    }
    std::wstring data;
    for (const std::wstring& r : roots) data += r + L"\0";
    data += L"\0";
    rc = RegSetValueExW(key, L"roots", 0, REG_MULTI_SZ, (const BYTE*)data.c_str(),
                        (DWORD)(data.size() * sizeof(wchar_t)));
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        err = "写入受管目录记录失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    return true;
}

inline bool managed_add_root(const std::string& lang, const fs::path& root) {
    std::wstring w = root.wstring();
    while (!w.empty() && w.back() == L'\\') w.pop_back();
    std::vector<std::wstring> roots = managed_get_roots(lang);
    std::wstring low = su::lower(w);
    for (const std::wstring& r : roots)
        if (su::lower(r) == low) return true;
    roots.push_back(w);
    std::string err;
    if (!managed_set_roots(lang, roots, err)) {
        logx::line("记录受管目录失败: " + err);
        return false;
    }
    logx::linef("已记录受管目录 [%s]: %s", lang.c_str(), su::wide_to_utf8(w).c_str());
    return true;
}

inline bool managed_remove_root(const std::string& lang, const fs::path& root) {
    std::wstring w = root.wstring();
    while (!w.empty() && w.back() == L'\\') w.pop_back();
    std::vector<std::wstring> roots = managed_get_roots(lang);
    std::vector<std::wstring> keep;
    std::wstring low = su::lower(w);
    for (const std::wstring& r : roots)
        if (su::lower(r) != low) keep.push_back(r);
    if (keep.size() == roots.size()) return true;
    std::string err;
    if (!managed_set_roots(lang, keep, err)) {
        logx::line("更新受管目录记录失败: " + err);
        return false;
    }
    logx::linef("已移除受管目录记录 [%s]: %s", lang.c_str(), su::wide_to_utf8(w).c_str());
    return true;
}

inline bool managed_set_last_root(const std::string& lang, const fs::path& root) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, managed_key(lang).c_str(), 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) return false;
    std::wstring w = root.wstring();
    while (!w.empty() && w.back() == L'\\') w.pop_back();
    rc = RegSetValueExW(key, L"last_root", 0, REG_SZ, (const BYTE*)w.c_str(),
                        (DWORD)((w.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

inline std::wstring managed_get_last_root(const std::string& lang) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, managed_key(lang).c_str(), 0, KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS)
        return {};
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    std::wstring out;
    if (RegQueryValueExW(key, L"last_root", nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS)
        out = buf;
    RegCloseKey(key);
    return out;
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

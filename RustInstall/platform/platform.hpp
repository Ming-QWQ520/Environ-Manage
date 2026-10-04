// platform/platform.hpp : 平台操作层 —— PATH、环境变量、目录 junction、进程执行
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

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

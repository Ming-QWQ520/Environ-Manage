// providers/node_tools.hpp : Node.js 工具链管理（npm/pnpm/yarn/corepack）
// npm/npx 随 Node 安装自带；pnpm/yarn 推荐经 Corepack 管理（不单独下载二进制）；
// 存储位置规范化：pnpm global-dir/global-bin-dir/state-dir/cache-dir + npm prefix/cache。
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

#include "logger.hpp"
#include "strutil.hpp"

namespace nodetools {

namespace fs = std::filesystem;

// 在 node 目录下执行 .cmd 工具（npm/pnpm/yarn/corepack），捕获合并输出。
// .cmd 不能直接 CreateProcess，经 cmd.exe /d /s /c 解释；工作目录 = node 目录，
// 使 npm.cmd/pnpm.cmd（current junction 内）可以被解析。
inline bool run_tool(const fs::path& node_dir, const std::string& tool,
                     const std::wstring& args, std::string& out, std::string& err,
                     unsigned timeout_ms = 600000) {
    std::wstring cmdline = L"cmd.exe /d /s /c \" " + su::utf8_to_wide(tool) + L" " + args +
                           L" \"";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        err = "创建管道失败";
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, node_dir.c_str(), &si, &pi)) {
        CloseHandle(wr);
        CloseHandle(rd);
        err = "无法启动 " + tool + "（错误码 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    CloseHandle(wr);

    out.clear();
    DWORD waited = 0;
    bool exited = false;
    while (waited < timeout_ms) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD got = 0;
            if (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
            else break;
            if (out.size() > 1024 * 1024) break;
        }
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            exited = true;
            for (;;) {
                DWORD avail = 0;
                if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
                    break;
                char buf[4096];
                DWORD got = 0;
                if (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got)
                    out.append(buf, got);
                else break;
            }
            break;
        }
        waited += 100;
    }
    if (!exited) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(rd);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        err = tool + " 执行超时";
        logx::line(err);
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (code != 0) {
        err = tool + " 退出码 " + std::to_string(code) + "：" + out;
        logx::line(err);
        return false;
    }
    return true;
}

// 工具版本（首行）；未安装/失败返回空串
inline std::string tool_version(const fs::path& node_dir, const std::string& tool) {
    std::string out, err;
    if (!run_tool(node_dir, tool, L"--version", out, err)) return "";
    size_t end = out.find('\n');
    std::string line = su::trim(out.substr(0, end == std::string::npos ? out.size() : end));
    return line;
}

// Corepack 是否已启用（以 node 目录是否生成 pnpm shim 为准）
inline bool corepack_enabled(const fs::path& node_dir) {
    std::error_code ec;
    return fs::exists(node_dir / L"pnpm.cmd", ec);
}

// 启用/禁用 Corepack（disable 会移除 pnpm/yarn shim）
inline bool set_corepack(const fs::path& node_dir, bool enable, std::string& err) {
    std::string out;
    bool ok = run_tool(node_dir, "corepack", enable ? L"enable" : L"disable", out, err);
    if (ok)
        logx::linef("Corepack 已%s", enable ? "启用" : "禁用");
    return ok;
}

// 通过 Corepack 安装/更新工具（pnpm/yarn），全局（shim 落在 node 目录 = current 内）
inline bool install_via_corepack(const fs::path& node_dir, const std::string& tool,
                                 std::string& version_out, std::string& err) {
    std::string out;
    if (!corepack_enabled(node_dir))
        if (!run_tool(node_dir, "corepack", L"enable " + su::utf8_to_wide(tool), out, err))
            return false;
    logx::linef("Corepack: 准备 %s@latest…", tool.c_str());
    if (!run_tool(node_dir, "corepack",
                  L"prepare " + su::utf8_to_wide(tool) + L"@latest --activate", out, err,
                  600000))
        return false;
    version_out = tool_version(node_dir, tool);
    if (version_out.empty()) {
        err = tool + " 安装后无法获取版本";
        return false;
    }
    logx::linef("%s 安装完成: %s", tool.c_str(), version_out.c_str());
    return true;
}

// 存储位置配置（pnpm 四目录 + npm prefix/cache）
inline bool configure_storage(const fs::path& node_dir, const fs::path& base,
                              std::vector<std::wstring>& path_dirs, std::string& err) {
    std::error_code ec;
    fs::create_directories(base, ec);
    std::string out;
    struct Cfg {
        const char* tool;
        const wchar_t* key;
        std::wstring value;
    };
    std::vector<Cfg> cfgs = {
        {"pnpm", L"config set global-dir", base / L"global"},
        {"pnpm", L"config set global-bin-dir", base / L"bin"},
        {"pnpm", L"config set state-dir", base / L"state"},
        {"pnpm", L"config set cache-dir", base / L"cache"},
        {"npm", L"config set prefix", base / L"npm-global"},
        {"npm", L"config set cache", base / L"npm-cache"},
    };
    for (const Cfg& c : cfgs) {
        logx::linef("配置 %s %s = %s", c.tool, su::wide_to_utf8(c.key).c_str(),
                    su::wide_to_utf8(c.value.wstring()).c_str());
        if (!run_tool(node_dir, c.tool, c.key + L" \"" + c.value.wstring() + L"\"", out, err))
            return false;
    }
    path_dirs.push_back(base / L"bin");           // pnpm 全局 bin
    path_dirs.push_back(base / L"npm-global");    // npm 全局包目录
    return true;
}

} // namespace nodetools

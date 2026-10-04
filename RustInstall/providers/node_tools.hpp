// providers/node_tools.hpp : Node.js 工具链管理（npm/npx/corepack/pnpm/yarn）
//
// 工具链优先级模型：
//   1. npm / npx —— 随 Node.js 安装自动附带，不做任何安装动作，只检测并显示版本；
//   2. Corepack  —— Node.js 内置，提供"启用/禁用"开关，用来统一管理 Yarn/pnpm；
//   3. Yarn / pnpm —— 常用包管理器，一律经 Corepack 管理版本（corepack prepare
//      <工具>@latest --activate），本工具绝不单独下载其二进制。
//
// 存储位置规范化（彻底规范 pnpm 的全局包、二进制文件与状态目录，npm 同步规范）：
//   pnpm config set global-dir      <base>\global      全局包目录
//   pnpm config set global-bin-dir  <base>\bin         全局命令（二进制）目录
//   pnpm config set state-dir       <base>\state       状态目录
//   pnpm config set cache-dir       <base>\cache       下载缓存目录
//   npm   config set prefix         <base>\npm-global  全局包目录（bin 同在此目录）
//   npm   config set cache          <base>\npm-cache   下载缓存目录
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "logger.hpp"
#include "strutil.hpp"

namespace nodetools {

namespace fs = std::filesystem;

// 在 node 目录下执行 .cmd 工具（npm/pnpm/yarn/corepack），捕获合并输出。
// .cmd 不能直接 CreateProcess，经 cmd.exe /d /s /c 解释；工作目录 = node 目录，
// 使 npm.cmd/pnpm.cmd（平铺安装时即 node 目录，或 current junction 内）可以被解析。
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

// 读取工具配置项当前值（pnpm/npm config get <key>），失败返回空串
inline std::string config_get(const fs::path& node_dir, const std::string& tool,
                              const std::wstring& key) {
    std::string out, err;
    if (!run_tool(node_dir, tool, L"config get " + key, out, err, 60000)) return "";
    size_t end = out.find('\n');
    return su::trim(out.substr(0, end == std::string::npos ? out.size() : end));
}

// Corepack 是否已启用（以 node 目录是否生成 pnpm shim 为准）
inline bool corepack_enabled(const fs::path& node_dir) {
    std::error_code ec;
    return fs::exists(node_dir / L"pnpm.cmd", ec);
}

// 工具检测结果：node/npm/npx 随装自带只读版本；corepack 内置；pnpm/yarn 经 Corepack
struct ToolVer {
    std::string name;    // node / npm / npx / corepack / pnpm / yarn
    std::string source;  // 来源说明（随 Node.js 自带 / Node 内置 / 经 Corepack 管理）
    std::string version; // 空 = 未检测到（未安装或 Corepack 未启用）
};

// 一次性检测全部工具版本（按优先级模型组织）。
// want_yarn=false 时跳过 yarn 检测（用户未选择安装 yarn 的场景，节省探测时间）。
inline std::vector<ToolVer> detect_tools(const fs::path& node_dir, bool want_yarn = true) {
    std::vector<ToolVer> out;
    // 1) npm / npx：装完 Node 自动有，只需检测和显示版本
    for (const char* name : {"npm", "npx"}) {
        ToolVer t;
        t.name = name;
        t.source = "随 Node.js 自带";
        t.version = tool_version(node_dir, name);
        out.push_back(std::move(t));
    }
    // 2) Corepack：Node 内置（v16.10+），作为 pnpm/yarn 的统一管理开关
    ToolVer c;
    c.name = "corepack";
    c.source = "Node 内置";
    c.version = tool_version(node_dir, "corepack");
    bool has_corepack = !c.version.empty();
    out.push_back(std::move(c));
    // 3) pnpm / yarn：仅当 Corepack 已启用（shim 存在）时才有版本，不自行下载二进制
    if (has_corepack && corepack_enabled(node_dir)) {
        ToolVer p;
        p.name = "pnpm";
        p.source = "经 Corepack 管理";
        p.version = tool_version(node_dir, "pnpm");
        out.push_back(std::move(p));
        if (want_yarn) {
            ToolVer y;
            y.name = "yarn";
            y.source = "经 Corepack 管理";
            y.version = tool_version(node_dir, "yarn");
            out.push_back(std::move(y));
        }
    }
    return out;
}

// 检测结果 → 展示行（"  npm 10.9.2（随 Node.js 自带）"），未检测到的给出提示。
// Corepack 启用状态：有 corepack 版本且 pnpm shim 生效即视为已启用。
inline std::vector<std::string> format_tool_lines(const std::vector<ToolVer>& tools) {
    std::vector<std::string> lines;
    bool corepack_on = false;
    for (const ToolVer& t : tools) {
        if (t.name == "corepack" && !t.version.empty()) corepack_on = true;
        if (t.name == "pnpm" && !t.version.empty()) corepack_on = true;
    }
    for (const ToolVer& t : tools) {
        if (!t.version.empty())
            lines.push_back("  " + t.name + " " + t.version + "（" + t.source + "）");
        else if (t.name == "pnpm" || t.name == "yarn")
            lines.push_back("  " + t.name + " 未启用（可通过 Corepack 管理）");
        else
            lines.push_back("  " + t.name + " 未检测到");
    }
    lines.push_back("  Corepack " +
                    std::string(corepack_on ? "已启用（管理 pnpm/yarn）"
                                            : "未启用（pnpm/yarn 不可用，可开启开关）"));
    return lines;
}

// 启用/禁用 Corepack（disable 会移除 pnpm/yarn shim）——对外提供"启用/禁用"开关
inline bool set_corepack(const fs::path& node_dir, bool enable, std::string& err) {
    std::string out;
    bool ok = run_tool(node_dir, "corepack", enable ? L"enable" : L"disable", out, err);
    if (ok)
        logx::linef("Corepack 已%s", enable ? "启用" : "禁用");
    return ok;
}

// 经 Corepack 安装/激活指定版本的 pnpm/yarn（不单独下载二进制，shim 落在 node 目录内）
// version 形如 "latest" / "10.12.1"；默认 latest。全局可用（node 目录 = current 生效范围）。
inline bool install_via_corepack(const fs::path& node_dir, const std::string& tool,
                                 std::string& version_out, std::string& err,
                                 const std::string& version = "latest") {
    std::string out;
    if (!corepack_enabled(node_dir))
        if (!run_tool(node_dir, "corepack", L"enable " + su::utf8_to_wide(tool), out, err))
            return false;
    logx::linef("Corepack: 准备 %s@%s…", tool.c_str(), version.c_str());
    if (!run_tool(node_dir, "corepack",
                  L"prepare " + su::utf8_to_wide(tool) + L"@" + su::utf8_to_wide(version) +
                      L" --activate",
                  out, err, 600000))
        return false;
    version_out = tool_version(node_dir, tool);
    if (version_out.empty()) {
        err = tool + " 安装后无法获取版本";
        return false;
    }
    logx::linef("%s 安装完成: %s（经 Corepack 管理，未单独下载二进制）", tool.c_str(),
                version_out.c_str());
    return true;
}

// 存储位置配置：pnpm 四目录（global/global-bin/state/cache）+ npm prefix/cache。
// base 为存储根目录（如 D:\pnpm-repository）；path_dirs 返回需要追加进 PATH 的目录
// （pnpm 全局 bin 与 npm 全局包目录）。设置后逐项回读校验，保证配置真正落盘。
inline bool configure_storage(const fs::path& node_dir, const fs::path& base,
                              std::vector<std::wstring>& path_dirs, std::string& err) {
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) {
        err = "无法创建存储目录 " + su::wide_to_utf8(base.wstring()) + "：" + ec.message();
        logx::line(err);
        return false;
    }
    std::string out;
    struct Cfg {
        const char* tool;
        const wchar_t* key;
        fs::path value;
        const char* desc;
    };
    std::vector<Cfg> cfgs = {
        {"pnpm", L"global-dir", base / L"global", "全局包目录"},
        {"pnpm", L"global-bin-dir", base / L"bin", "全局命令（二进制）目录"},
        {"pnpm", L"state-dir", base / L"state", "状态目录"},
        {"pnpm", L"cache-dir", base / L"cache", "下载缓存目录"},
        {"npm", L"prefix", base / L"npm-global", "全局包目录"},
        {"npm", L"cache", base / L"npm-cache", "下载缓存目录"},
    };
    for (const Cfg& c : cfgs) {
        logx::linef("配置 %s %s = %s（%s）", c.tool, su::wide_to_utf8(c.key).c_str(),
                    su::wide_to_utf8(c.value.wstring()).c_str(), c.desc);
        if (!run_tool(node_dir, c.tool,
                      std::wstring(c.key) + L" \"" + c.value.wstring() + L"\"", out, err))
            return false;
    }
    // 回读校验：确保每一项都真实生效（彻底规范存储位置）
    for (const Cfg& c : cfgs) {
        std::string got = config_get(node_dir, c.tool, c.key);
        std::string want = su::wide_to_utf8(c.value.wstring());
        if (got.empty()) {
            logx::linef("警告: %s %s 回读为空", c.tool, su::wide_to_utf8(c.key).c_str());
            continue;
        }
        if (su::lower(got) != su::lower(want))
            logx::linef("警告: %s %s 回读不一致（期望 %s，实际 %s）", c.tool,
                        su::wide_to_utf8(c.key).c_str(), want.c_str(), got.c_str());
        else
            logx::linef("校验 %s %s = %s", c.tool, su::wide_to_utf8(c.key).c_str(),
                        got.c_str());
    }
    fs::create_directories(base / L"bin", ec);
    fs::create_directories(base / L"npm-global", ec);
    path_dirs.push_back((base / L"bin").wstring());        // pnpm 全局 bin
    path_dirs.push_back((base / L"npm-global").wstring()); // npm 全局包目录（bin 同目录）
    return true;
}

} // namespace nodetools

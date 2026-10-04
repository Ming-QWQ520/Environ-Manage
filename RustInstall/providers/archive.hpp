// providers/archive.hpp : 公共解压层（Windows 自带 tar.exe，支持 zip/tar.gz/tar.xz）
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "logger.hpp"
#include "platform/platform.hpp"
#include "strutil.hpp"

namespace archive {

namespace fs = std::filesystem;

// 解压压缩包到版本目录：自动识别“单顶层目录”（node-vX/、jdk-*/、go/）与“扁平”（.NET SDK）
inline bool extract_to_ver_dir(const fs::path& archive_file, const fs::path& ver_dir,
                               std::string& err) {
    wchar_t tar_exe[MAX_PATH * 2] = {};
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\tar.exe", tar_exe, MAX_PATH * 2);
    if (GetFileAttributesW(tar_exe) == INVALID_FILE_ATTRIBUTES) {
        err = "未找到 Windows 自带的 tar.exe";
        return false;
    }
    std::error_code ec;
    fs::path tmp = ver_dir.parent_path() / (L"_" + ver_dir.filename().wstring() + L"_tmp");
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    std::wstring args = L"-xf \"" + archive_file.wstring() + L"\" -C \"" + tmp.wstring() + L"\"";
    DWORD code = 0;
    if (!platform::run_hidden(tar_exe, args, code, err) || code != 0) {
        if (err.empty()) err = "解压失败（tar 退出码 " + std::to_string(code) + "）";
        logx::line("解压失败: " + err);
        fs::remove_all(tmp, ec);
        return false;
    }
    fs::create_directories(ver_dir, ec);
    std::vector<fs::path> entries;
    for (const fs::directory_entry& e : fs::directory_iterator(tmp)) entries.push_back(e.path());
    if (entries.size() == 1 && fs::is_directory(entries[0])) {
        // 单顶层目录 → 合并其内容到版本目录
        for (const fs::directory_entry& e : fs::directory_iterator(entries[0]))
            if (!platform::move_tree(e.path(), ver_dir, err)) {
                fs::remove_all(tmp, ec);
                return false;
            }
    } else {
        // 扁平结构 → 全部上移
        for (const fs::path& e : entries)
            if (!platform::move_tree(e, ver_dir, err)) {
                fs::remove_all(tmp, ec);
                return false;
            }
    }
    fs::remove_all(tmp, ec);
    logx::linef("已解压到版本目录: %s", su::wide_to_utf8(ver_dir.wstring()).c_str());
    return true;
}

} // namespace archive

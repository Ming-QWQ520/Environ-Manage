// providers/checksum.hpp : 公共校验层（SHA-256 / MD5 / 大小）
#pragma once

#include <filesystem>
#include <string>

#include "../md5.hpp"
#include "../sha256.hpp"
#include "../providers/provider.hpp"
#include "logger.hpp"
#include "strutil.hpp"

namespace checksum {

namespace fs = std::filesystem;

inline std::string sha256_hex(const fs::path& f) { return sha256::file_hex(f.wstring()); }
inline std::string md5_hex(const fs::path& f) { return md5::file_hex(f.wstring()); }

inline bool same_hex(const std::string& a, const std::string& b) {
    return !a.empty() && su::lower(a) == su::lower(b);
}

// 按 Artifact 的校验链校验：SHA-256 → MD5 → 大小 → 跳过（说明策略并记日志）
inline bool artifact_ok(const prov::Artifact& a, const fs::path& dest, std::string& err) {
    if (!a.sha256.empty()) {
        std::string got = sha256_hex(dest);
        if (!same_hex(got, a.sha256)) {
            err = "SHA-256 校验失败: 期望 " + a.sha256 + " 实际 " +
                  (got.empty() ? "(读取失败)" : got);
            logx::line(err);
            return false;
        }
        logx::linef("SHA-256 校验通过: %s", got.c_str());
        return true;
    }
    if (!a.md5.empty()) {
        std::string got = md5_hex(dest);
        if (!same_hex(got, a.md5)) {
            err = "MD5 校验失败: 期望 " + a.md5 + " 实际 " + (got.empty() ? "(读取失败)" : got);
            logx::line(err);
            return false;
        }
        logx::linef("MD5 校验通过: %s", got.c_str());
        return true;
    }
    if (a.size > 0) {
        std::error_code ec;
        uint64_t sz = fs::file_size(dest, ec);
        if (sz != a.size) {
            err = "文件大小不符: 期望 " + std::to_string(a.size) + " 实际 " + std::to_string(sz);
            logx::line(err);
            return false;
        }
        logx::linef("大小比对通过（%s）", su::human_size(sz).c_str());
        return true;
    }
    logx::line("提示：无预知校验和，完整性由下载字节数与 Content-Length 比对保证");
    return true;
}

} // namespace checksum

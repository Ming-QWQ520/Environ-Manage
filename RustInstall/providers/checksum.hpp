// providers/checksum.hpp : 公共校验层（SHA-256 / SHA-512 / MD5 / 大小 + 哈希计算进度）
#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "../md5.hpp"
#include "../sha256.hpp"
#include "../sha512.hpp"
#include "../providers/provider.hpp"
#include "logger.hpp"
#include "strutil.hpp"

namespace checksum {

namespace fs = std::filesystem;

// ---- 哈希计算进度回调（供 TUI 校验阶段进度条；进程内单安装流） ----
using HashProgress = std::function<void(uint64_t done, uint64_t total)>;
inline HashProgress g_hash_progress;

inline void set_hash_progress(HashProgress cb) { g_hash_progress = std::move(cb); }
inline void clear_hash_progress() { g_hash_progress = nullptr; }
// 当前全局回调（无回调时返回空函数，可直接传给 file_hex_progress）
inline const HashProgress& hash_progress() { return g_hash_progress; }

inline std::string sha256_hex(const fs::path& f) { return sha256::file_hex(f.wstring()); }
inline std::string sha512_hex(const fs::path& f) {
    return sha512::file_hex(f.wstring(), {});
}
inline std::string md5_hex(const fs::path& f) { return md5::file_hex(f.wstring()); }

// 带全局进度回调的哈希计算（UI 侧 set_hash_progress 后调用；无回调时与 *_hex 等价）
inline std::string sha256_hex_progress(const fs::path& f) {
    return sha256::file_hex_progress(f.wstring(), g_hash_progress);
}
inline std::string sha512_hex_progress(const fs::path& f) {
    return sha512::file_hex(f.wstring(), g_hash_progress);
}

inline bool same_hex(const std::string& a, const std::string& b) {
    return !a.empty() && su::lower(a) == su::lower(b);
}

// 按 Artifact 的校验链校验：SHA-256 → SHA-512 → MD5 → 大小 → 跳过（说明策略并记日志）
inline bool artifact_ok(const prov::Artifact& a, const fs::path& dest, std::string& err) {
    if (!a.sha256.empty()) {
        std::string got = sha256_hex_progress(dest);
        if (!same_hex(got, a.sha256)) {
            err = "SHA-256 校验失败: 期望 " + a.sha256 + " 实际 " +
                  (got.empty() ? "(读取失败)" : got);
            logx::line(err);
            return false;
        }
        logx::linef("SHA-256 校验通过: %s", got.c_str());
        return true;
    }
    if (!a.sha512.empty()) {
        std::string got = sha512_hex_progress(dest);
        if (!same_hex(got, a.sha512)) {
            err = "SHA-512 校验失败: 期望 " + a.sha512 + " 实际 " +
                  (got.empty() ? "(读取失败)" : got);
            logx::line(err);
            return false;
        }
        logx::linef("SHA-512 校验通过: %s", got.c_str());
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

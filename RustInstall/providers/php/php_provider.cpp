// providers/php/php_provider.cpp : PHP 实现
// 版本源：windows.php.net 官方下载目录（Apache autoindex HTML 解析）
//   releases/       最新版本（php-<ver>[-nts]-Win32-<vs>-<arch>.zip + sha256.sum）
//   releases/archives/  历史版本（同名文件布局），作为下载回退
// id 规则：TS 版为 "8.3.14"，NTS 版为 "8.3.14-nts"
#include "php_provider.hpp"

#include <algorithm>
#include <cctype>

#include "../../python.hpp"
#include "../../providers/archive.hpp"
#include "../../providers/checksum.hpp"
#include "../../providers/http_client.hpp"
#include "../../providers/registry.hpp"

namespace prov {

namespace {

const char* kReleasesDir = "https://windows.php.net/downloads/releases/";
const char* kArchivesDir = "https://windows.php.net/downloads/releases/archives/";

// php 架构令牌：x64 / x86 / arm64
std::string php_arch() {
    std::string a = py::detect_host_arch();
    return a == "amd64" ? "x64" : a;
}

struct PhpZip {
    std::string ver;
    bool nts = false;
    std::string arch;
    std::string filename;
};

// 解析 php-<ver>[-nts]-Win32-<comp>-<arch>.zip 文件名（兼容老命名 php-<ver>-<VC>-<arch>.zip）
bool parse_php_zip(const std::string& name, PhpZip& out) {
    if (!su::starts_with(name, "php-") || !su::ends_with(name, ".zip")) return false;
    std::string mid = name.substr(4, name.size() - 8);
    std::vector<std::string> tok;
    size_t pos = 0;
    while (pos <= mid.size()) {
        size_t next = mid.find('-', pos);
        std::string t = mid.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!t.empty()) tok.push_back(t);
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    if (tok.size() < 3) return false;
    std::string ver = tok[0];
    if (ver.empty() || !isdigit((unsigned char)ver[0])) return false;
    size_t idx = 1;
    bool nts = false;
    if (idx < tok.size() && tok[idx] == "nts") {
        nts = true;
        ++idx;
    }
    // Win32 布局: Win32 <comp> <arch>；老布局: <VCxx> <arch>；不允许多余 token
    if (idx + 1 >= tok.size()) return false;
    std::string arch;
    if (tok[idx] == "Win32") {
        if (idx + 2 != tok.size() - 1) return false;
        arch = tok[idx + 2];
    } else {
        if (idx + 1 != tok.size() - 1) return false;
        arch = tok[idx + 1];
    }
    if (arch != "x64" && arch != "x86" && arch != "arm64") return false;
    out.ver = ver;
    out.nts = nts;
    out.arch = arch;
    out.filename = name;
    return true;
}

} // namespace

std::string PhpProvider::id() const { return "php"; }
std::string PhpProvider::display() const { return "PHP"; }

bool PhpProvider::ensure_list(std::string& err) {
    if (!file_of_.empty()) return true;
    for (const char* dir : {kReleasesDir, kArchivesDir}) {
        std::string body = httpc::get_cached(std::string(dir), err);
        if (body.empty()) continue;
        // 扫描目录 HTML 中的文件名（仅保留合法字符段）
        for (size_t pos = body.find("php-"); pos != std::string::npos;
             pos = body.find("php-", pos + 4)) {
            size_t i = pos;
            while (i < body.size() &&
                   (isalnum((unsigned char)body[i]) || body[i] == '.' || body[i] == '-' ||
                    body[i] == '_'))
                ++i;
            std::string name = body.substr(pos, i - pos);
            PhpZip z;
            if (!parse_php_zip(name, z)) continue;
            std::string id = z.ver + (z.nts ? "-nts" : "");
            auto it = file_of_.find(id);
            if (it != file_of_.end()) continue;
            file_of_[id] = z.filename;
        }
        if (!file_of_.empty()) break; // releases 目录已覆盖最新版本
    }
    if (file_of_.empty()) {
        err = "未能从 windows.php.net 解析出任何 PHP 版本";
        logx::line(err);
        return false;
    }
    logx::linef("PHP 版本解析：%d 条", (int)file_of_.size());
    return true;
}

bool PhpProvider::list_versions(std::vector<VersionInfo>& out, std::string& err) {
    if (!ensure_list(err)) return false;
    std::string want = php_arch();
    std::vector<std::pair<std::string, bool>> ids; // (id, nts)
    for (const auto& kv : file_of_) {
        PhpZip z;
        if (!parse_php_zip(kv.second, z)) continue;
        if (z.arch != want) continue;
        ids.push_back({kv.first, z.nts});
    }
    if (ids.empty()) { // arm64 主机兜底 x64 包（可经模拟运行）
        want = "x64";
        for (const auto& kv : file_of_) {
            PhpZip z;
            if (!parse_php_zip(kv.second, z)) continue;
            if (z.arch == want) ids.push_back({kv.first, z.nts});
        }
    }
    std::stable_sort(ids.begin(), ids.end(),
                     [](const std::pair<std::string, bool>& a, const std::pair<std::string, bool>& b) {
                         int c = natural_cmp(a.first, b.first);
                         if (c != 0) return c > 0;
                         return !a.first.empty() && a.first.size() <= b.first.size(); // TS 在前
                     });
    for (const auto& p : ids) {
        VersionInfo v;
        v.id = p.first;
        v.display = p.first.substr(0, p.first.find("-nts"));
        v.level = SupportLevel::Stable;
        v.tag_label = p.second ? "NTS" : "TS";
        out.push_back(std::move(v));
    }
    return !out.empty();
}

bool PhpProvider::resolve(const std::string& version_id, Artifact& out, std::string& err) {
    if (!ensure_list(err)) return false;
    auto it = file_of_.find(version_id);
    if (it == file_of_.end()) {
        err = "未找到版本 " + version_id;
        logx::line(err);
        return false;
    }
    out.version_id = version_id;
    out.version = version_id;
    out.filename = it->second;
    out.url = std::string(kReleasesDir) + it->second;
    return true;
}

std::vector<std::pair<std::string, std::wstring>> PhpProvider::mirrors(const Artifact& a) const {
    std::wstring file = su::utf8_to_wide(a.filename);
    return {{"官方 releases", su::utf8_to_wide(kReleasesDir) + file},
            {"官方 archives", su::utf8_to_wide(kArchivesDir) + file}};
}

bool PhpProvider::verify(const Artifact& a, const fs::path& dest, std::string& err) {
    // sha256.sum（releases 目录，缓存 6h）；缺失/未命中时退回大小比对
    std::string sums = httpc::get_cached(std::string(kReleasesDir) + "sha256.sum", err);
    if (!sums.empty()) {
        size_t pos = sums.find(a.filename);
        if (pos != std::string::npos && pos >= 64) {
            std::string expect = su::trim(sums.substr(pos - 64, 64));
            std::string got = checksum::sha256_hex(dest);
            if (checksum::same_hex(got, expect)) {
                logx::linef("SHA-256 校验通过: %s", got.c_str());
                return true;
            }
            err = "SHA-256 校验失败: 期望 " + expect + " 实际 " +
                  (got.empty() ? "(读取失败)" : got);
            logx::line(err);
            return false;
        }
        logx::line("sha256.sum 中未找到该文件，退回大小比对");
    }
    return checksum::artifact_ok(a, dest, err);
}

bool PhpProvider::install(const Artifact& a, const fs::path& archive_file,
                          const fs::path& ver_dir, std::string& err) {
    (void)a;
    return archive::extract_to_ver_dir(archive_file, ver_dir, err); // zip 内为扁平结构
}

std::string PhpProvider::bin_subdir() const { return ""; }
std::vector<std::pair<std::string, std::string>> PhpProvider::envs() const { return {}; }
std::string PhpProvider::verify_exe() const { return "php.exe"; }

namespace {
struct PhpRegistrar {
    PhpRegistrar() {
        Registry::instance().add("php", [] { return std::make_unique<PhpProvider>(); });
    }
};
const PhpRegistrar g_php_registrar;
} // namespace

} // namespace prov

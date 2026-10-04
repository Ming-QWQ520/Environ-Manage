// providers/ruby/ruby_provider.cpp : Ruby 实现
// 版本源：oneclick/rubyinstaller2 GitHub Releases（rubyinstaller-<ver>-1-x64.7z）
// 解压：.7z 需 7zr.exe（7-Zip 精简版，约 600KB，缓存于 <exe目录>\cache\tools，
//       首次使用时从 ip7z/7zip GitHub Release（gh-proxy 加速）或 7-zip.org 官方下载）
#include "ruby_provider.hpp"

#include "../../python.hpp"
#include "../../providers/archive.hpp"
#include "../../providers/checksum.hpp"
#include "../../providers/gh_releases.hpp"
#include "../../providers/http_client.hpp"
#include "../../providers/registry.hpp"

namespace prov {

namespace {

const char* kRepo = "oneclick/rubyinstaller2";
const char* k7zrUrl = "https://github.com/ip7z/7zip/releases/download/24.08/7zr.exe";
const char* k7zrOfficial = "https://www.7-zip.org/a/7zr.exe";

// ruby 架构令牌：x64 / x86（RubyInstaller 无官方 arm64 包）
std::string ruby_arch() {
    std::string a = py::detect_host_arch();
    return a == "x86" ? "x86" : "x64";
}

// 取 <exe目录>\cache\tools（7zr.exe 缓存位置）
fs::path tools_cache_dir() {
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    return fs::path(exe).parent_path() / L"cache" / L"tools";
}

// 确保 7zr.exe 可用（缺失时下载），返回其路径（失败返回空）
fs::path ensure_7zr(std::string& err) {
    fs::path dst = tools_cache_dir() / L"7zr.exe";
    std::error_code ec;
    if (fs::is_regular_file(dst, ec) && fs::file_size(dst, ec) > 100000) return dst;
    fs::create_directories(dst.parent_path(), ec);
    auto noop = [](uint64_t, uint64_t) {};
    int used = -1;
    std::vector<std::pair<std::string, std::wstring>> mirrors = {
        {"gh-proxy.com 加速", L"https://gh-proxy.com/" + su::utf8_to_wide(k7zrUrl)},
        {"gh-proxy.org 加速", L"https://gh-proxy.org/" + su::utf8_to_wide(k7zrUrl)},
        {"GitHub 直连", su::utf8_to_wide(k7zrUrl)},
        {"7-zip.org 官方", su::utf8_to_wide(k7zrOfficial)},
    };
    if (!httpc::download_mirrors(mirrors, dst, noop, used, err, nullptr)) {
        err = "下载 7zr.exe 解压工具失败: " + err;
        logx::line(err);
        return {};
    }
    logx::linef("7zr.exe 就绪（%s）", mirrors[(size_t)used].first.c_str());
    return dst;
}

} // namespace

std::string RubyProvider::id() const { return "ruby"; }
std::string RubyProvider::display() const { return "Ruby (RubyInstaller)"; }

bool RubyProvider::ensure_list(std::string& err) {
    if (!url_of_.empty()) return true;
    std::vector<gh::RepoRelease> rels;
    if (!gh::fetch_repo_releases(kRepo, 1, 100, rels, err)) return false;
    std::string suffix = "-" + ruby_arch() + ".7z";
    for (const gh::RepoRelease& r : rels) {
        for (const gh::RepoAsset& a : r.assets) {
            const std::string& n = a.name;
            if (!su::starts_with(n, "rubyinstaller-") || !su::ends_with(n, suffix)) continue;
            if (n.find("dev") != std::string::npos ||
                n.find("autobuild") != std::string::npos)
                continue;
            std::string id = n.substr(14, n.size() - 14 - suffix.size()); // rubyinstaller-<id><suffix>
            if (id.empty() || url_of_.count(id)) continue;
            url_of_[id] = a.url;
            size_of_[id] = a.size;
        }
    }
    if (url_of_.empty()) {
        err = "未在 RubyInstaller2 Releases 中找到 " + suffix + " 安装包";
        logx::line(err);
        return false;
    }
    logx::linef("Ruby 版本：%d 条", (int)url_of_.size());
    return true;
}

bool RubyProvider::list_versions(std::vector<VersionInfo>& out, std::string& err) {
    if (!ensure_list(err)) return false;
    // GitHub API 新→旧返回，url_of_ 为 map 无序 —— 用插入序号还原顺序
    for (const auto& kv : url_of_) {
        VersionInfo v;
        v.id = kv.first;
        v.display = kv.first;
        v.level = SupportLevel::Stable;
        v.tag_label = "RubyInstaller";
        out.push_back(std::move(v));
    }
    // 版本自然序降序（3.3.5-1 > 3.3.4-1）
    std::stable_sort(out.begin(), out.end(), [](const VersionInfo& a, const VersionInfo& b) {
        return natural_cmp(a.id, b.id) > 0;
    });
    return !out.empty();
}

bool RubyProvider::resolve(const std::string& version_id, Artifact& out, std::string& err) {
    if (!ensure_list(err)) return false;
    auto it = url_of_.find(version_id);
    if (it == url_of_.end()) {
        err = "未找到版本 " + version_id;
        logx::line(err);
        return false;
    }
    out.version_id = version_id;
    out.version = version_id;
    out.url = it->second;
    out.size = size_of_[version_id];
    size_t slash = out.url.find_last_of('/');
    out.filename = slash == std::string::npos ? out.url : out.url.substr(slash + 1);
    return true;
}

std::vector<std::pair<std::string, std::wstring>> RubyProvider::mirrors(const Artifact& a) const {
    return gh::asset_mirrors(a.url);
}

bool RubyProvider::verify(const Artifact& a, const fs::path& dest, std::string& err) {
    // Release 资产无独立校验和文件 —— 大小比对（下载字节数已与 Content-Length 核对）
    return checksum::artifact_ok(a, dest, err);
}

bool RubyProvider::install(const Artifact& a, const fs::path& archive_file,
                           const fs::path& ver_dir, std::string& err) {
    (void)a;
    fs::path seven = ensure_7zr(err);
    if (seven.empty()) return false;
    std::error_code ec;
    fs::path tmp = ver_dir.parent_path() / (L"_" + ver_dir.filename().wstring() + L"_tmp");
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    std::wstring args = L"x \"" + archive_file.wstring() + L"\" -o\"" + tmp.wstring() + L"\" -y";
    DWORD code = 0;
    if (!platform::run_hidden(seven, args, code, err) || code != 0) {
        if (err.empty()) err = "解压失败（7zr 退出码 " + std::to_string(code) + "）";
        logx::line("解压失败: " + err);
        fs::remove_all(tmp, ec);
        return false;
    }
    if (!archive::merge_tmp_to_ver_dir(tmp, ver_dir, err)) {
        fs::remove_all(tmp, ec);
        return false;
    }
    fs::remove_all(tmp, ec);
    logx::linef("已解压到版本目录: %s", su::wide_to_utf8(ver_dir.wstring()).c_str());
    return true;
}

std::string RubyProvider::bin_subdir() const { return "bin"; }
std::vector<std::pair<std::string, std::string>> RubyProvider::envs() const { return {}; }
std::string RubyProvider::verify_exe() const { return "bin\\ruby.exe"; }

namespace {
struct RubyRegistrar {
    RubyRegistrar() {
        Registry::instance().add("ruby", [] { return std::make_unique<RubyProvider>(); });
    }
};
const RubyRegistrar g_ruby_registrar;
} // namespace

} // namespace prov

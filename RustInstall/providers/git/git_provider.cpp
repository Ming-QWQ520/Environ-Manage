// providers/git/git_provider.cpp : Git 实现（MinGit 便携版）
// 版本源：git-for-windows/git GitHub Releases（MinGit-<ver>-64-bit.zip，排除 busybox 变体）
// MinGit 为官方精简便携包（无安装器、无 GUI），解压即用；cmd/git.exe 为入口
#include "git_provider.hpp"

#include "../../python.hpp"
#include "../../providers/archive.hpp"
#include "../../providers/checksum.hpp"
#include "../../providers/gh_releases.hpp"
#include "../../providers/http_client.hpp"
#include "../../providers/registry.hpp"

namespace prov {

namespace {

const char* kRepo = "git-for-windows/git";

// MinGit 平台令牌：64-bit / 32-bit / arm64（无 arm64 包时回退 64-bit）
std::string git_suffix() {
    std::string a = py::detect_host_arch();
    if (a == "x86") return "32-bit";
    if (a == "arm64") return "arm64";
    return "64-bit";
}

} // namespace

std::string GitProvider::id() const { return "git"; }
std::string GitProvider::display() const { return "Git (MinGit)"; }

bool GitProvider::ensure_list(std::string& err) {
    if (!url_of_.empty()) return true;
    std::vector<gh::RepoRelease> rels;
    if (!gh::fetch_repo_releases(kRepo, 1, 100, rels, err)) return false;
    std::string primary = git_suffix();
    // 两轮扫描：先主选平台令牌，arm64 主机找不到时回退 64-bit（经仿真运行）
    for (int pass = 0; pass < 2 && url_of_.empty(); ++pass) {
        std::string suffix = pass == 0 ? primary : "64-bit";
        for (const gh::RepoRelease& r : rels) {
            for (const gh::RepoAsset& a : r.assets) {
                const std::string& n = a.name;
                if (!su::starts_with(n, "MinGit-") || !su::ends_with(n, "-" + suffix + ".zip"))
                    continue;
                if (n.find("busybox") != std::string::npos) continue;
                std::string ver =
                    n.substr(7, n.size() - 7 - (suffix.size() + 5)); // MinGit-<ver>-<suffix>.zip
                if (ver.empty() || url_of_.count(ver)) continue;
                url_of_[ver] = a.url;
            }
            if (url_of_.size() >= 40) break; // 每平台取最近版本即可
        }
    }
    if (url_of_.empty()) {
        err = "未在 git-for-windows Releases 中找到 MinGit 压缩包";
        logx::line(err);
        return false;
    }
    logx::linef("Git (MinGit) 版本：%d 条", (int)url_of_.size());
    return true;
}

bool GitProvider::list_versions(std::vector<VersionInfo>& out, std::string& err) {
    if (!ensure_list(err)) return false;
    for (const auto& kv : url_of_) {
        VersionInfo v;
        v.id = kv.first;
        v.display = kv.first;
        v.level = SupportLevel::Stable;
        v.tag_label = "MinGit";
        out.push_back(std::move(v));
    }
    std::stable_sort(out.begin(), out.end(), [](const VersionInfo& a, const VersionInfo& b) {
        return natural_cmp(a.id, b.id) > 0;
    });
    return !out.empty();
}

bool GitProvider::resolve(const std::string& version_id, Artifact& out, std::string& err) {
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
    size_t slash = out.url.find_last_of('/');
    out.filename = slash == std::string::npos ? out.url : out.url.substr(slash + 1);
    return true;
}

std::vector<std::pair<std::string, std::wstring>> GitProvider::mirrors(const Artifact& a) const {
    return gh::asset_mirrors(a.url);
}

bool GitProvider::verify(const Artifact& a, const fs::path& dest, std::string& err) {
    // 尝试官方同名 .sha256 资产；不可用时退回大小比对
    std::string sums = httpc::get_cached(a.url + ".sha256", err);
    if (!sums.empty()) {
        // 文件内容形如 "<hash>  <filename>" 或纯 hash
        std::string first = sums.substr(0, sums.find_first_of(" \r\n"));
        if (first.size() == 64) {
            std::string got = checksum::sha256_hex(dest);
            if (checksum::same_hex(got, first)) {
                logx::linef("SHA-256 校验通过: %s", got.c_str());
                return true;
            }
            err = "SHA-256 校验失败: 期望 " + first + " 实际 " +
                  (got.empty() ? "(读取失败)" : got);
            logx::line(err);
            return false;
        }
    }
    return checksum::artifact_ok(a, dest, err);
}

bool GitProvider::install(const Artifact& a, const fs::path& archive_file,
                          const fs::path& ver_dir, std::string& err) {
    (void)a;
    return archive::extract_to_ver_dir(archive_file, ver_dir, err); // zip 内为扁平结构
}

std::string GitProvider::bin_subdir() const { return "cmd"; }
std::vector<std::pair<std::string, std::string>> GitProvider::envs() const { return {}; }
std::string GitProvider::verify_exe() const { return "cmd\\git.exe"; }

namespace {
struct GitRegistrar {
    GitRegistrar() {
        Registry::instance().add("git", [] { return std::make_unique<GitProvider>(); });
    }
};
const GitRegistrar g_git_registrar;
} // namespace

} // namespace prov

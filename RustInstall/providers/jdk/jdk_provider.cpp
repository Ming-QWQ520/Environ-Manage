// providers/jdk/jdk_provider.cpp : Eclipse Temurin（Adoptium）实现
// 版本源：api.adoptium.net/v3/info/available_releases（LTS 大版本表）
//         api.adoptium.net/v3/assets/feature_releases/<major>/ga（package.checksum 即 SHA-256）
// 镜像：GitHub 直链经 gh-proxy 加速
#include "jdk_provider.hpp"

#include "providers/archive.hpp"
#include "providers/checksum.hpp"
#include "providers/http_client.hpp"
#include "python.hpp"
#include "providers/registry.hpp"

namespace prov {

namespace {

std::string host_arch_jdk() {
    std::string a = py::detect_host_arch(); // amd64 / arm64 / x86
    return a == "arm64" ? "aarch64" : a == "x86" ? "x86" : "x64";
}

} // namespace

std::string JdkProvider::id() const { return "jdk"; }
std::string JdkProvider::display() const { return "JDK (Temurin)"; }

bool JdkProvider::list_versions(std::vector<VersionInfo>& out, std::string& err) {
    std::string body =
        httpc::get_text("https://api.adoptium.net/v3/info/available_releases",
                        {su::utf8_to_wide("User-Agent: RustInstall/1.0")}, err);
    if (body.empty()) {
        err = "获取 Adoptium 版本表失败: " + err;
        logx::line(err);
        return false;
    }
    json::Val root;
    std::string perr;
    if (!json::Parser(body).parse(root, perr) || root.t != json::Val::Obj) {
        err = "Adoptium 响应解析失败";
        logx::line(err);
        return false;
    }
    std::vector<int> lts;
    if (const json::Val* a = root.get("available_lts_releases"))
        for (const json::Val& v : a->arr) lts.push_back((int)v.num);
    if (const json::Val* a = root.get("available_releases"))
        for (const json::Val& v : a->arr) {
            int major = (int)v.num;
            bool is_lts = std::find(lts.begin(), lts.end(), major) != lts.end();
            VersionInfo v2;
            v2.id = std::to_string(major);
            v2.display = "JDK " + v2.id;
            v2.level = is_lts ? SupportLevel::Lts : SupportLevel::Stable;
            v2.tag_label = is_lts ? "LTS" : "功能版";
            out.push_back(std::move(v2));
        }
    // 大版本降序（最新在前）
    std::sort(out.begin(), out.end(), [](const VersionInfo& a, const VersionInfo& b) {
        return std::stoi(a.id) > std::stoi(b.id);
    });
    logx::linef("Adoptium 可用大版本：%d 个（LTS %d 个）", (int)out.size(), (int)lts.size());
    return !out.empty();
}

bool JdkProvider::resolve(const std::string& version_id, Artifact& out, std::string& err) {
    std::string url = "https://api.adoptium.net/v3/assets/feature_releases/" + version_id +
                      "/ga?architecture=" + host_arch_jdk() +
                      "&image_type=jdk&os=windows&vendor=eclipse&page_size=1";
    std::string body = httpc::get_text(
        url, {su::utf8_to_wide("User-Agent: RustInstall/1.0")}, err);
    if (body.empty()) {
        err = "获取 JDK " + version_id + " 资产失败: " + err;
        logx::line(err);
        return false;
    }
    json::Val root;
    std::string perr;
    if (!json::Parser(body).parse(root, perr) || root.t != json::Val::Arr || root.arr.empty()) {
        err = "JDK " + version_id + " 没有可用资产";
        logx::line(err);
        return false;
    }
    const json::Val& rel = root.arr[0];
    std::string release_name =
        rel.get("release_name") ? rel.get("release_name")->str_or() : "";
    const json::Val* binaries = rel.get("binaries");
    if (!binaries || binaries->arr.empty()) {
        err = "JDK " + version_id + " 没有可用的二进制";
        logx::line(err);
        return false;
    }
    const json::Val* pkg = binaries->arr[0].get("package");
    if (!pkg) {
        err = "JDK " + version_id + " 资产缺少 package";
        return false;
    }
    out.version_id = version_id;
    out.version = release_name;
    out.filename = pkg->get("name") ? pkg->get("name")->str_or() : "";
    out.url = pkg->get("link") ? pkg->get("link")->str_or() : "";
    out.sha256 = pkg->get("checksum") ? pkg->get("checksum")->str_or() : "";
    out.size = (uint64_t)(pkg->get("size") ? pkg->get("size")->num : 0);
    if (out.url.empty() || out.filename.empty()) {
        err = "JDK " + version_id + " 资产信息不完整";
        logx::line(err);
        return false;
    }
    logx::linef("JDK %s → %s（%s）", version_id.c_str(), release_name.c_str(),
                su::human_size(out.size).c_str());
    return true;
}

std::vector<std::pair<std::string, std::wstring>> JdkProvider::mirrors(const Artifact& a) const {
    std::wstring u = su::utf8_to_wide(a.url);
    return {{"gh-proxy 加速", L"https://gh-proxy.com/" + u}, {"官方 GitHub", u}};
}

bool JdkProvider::verify(const Artifact& a, const fs::path& dest, std::string& err) {
    return checksum::artifact_ok(a, dest, err);
}

bool JdkProvider::install(const Artifact& a, const fs::path& archive_file,
                          const fs::path& ver_dir, std::string& err) {
    return archive::extract_to_ver_dir(archive_file, ver_dir, err);
}

std::string JdkProvider::bin_subdir() const { return "bin"; }
std::vector<std::pair<std::string, std::string>> JdkProvider::envs() const {
    return {{"JAVA_HOME", ""}};
}
std::string JdkProvider::verify_exe() const { return "bin\\java.exe"; }

namespace {
struct JdkRegistrar {
    JdkRegistrar() { Registry::instance().add("jdk", [] { return std::make_unique<JdkProvider>(); }); }
};
const JdkRegistrar g_jdk_registrar;
} // namespace

} // namespace prov

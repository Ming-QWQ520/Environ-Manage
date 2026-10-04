// providers/go/go_provider.cpp : Go 实现
// 版本源：go.dev/dl/?mode=json&include=all（镜像 golang.google.cn 同路径）
// 支持状态：Go 无 LTS —— 最新两个 minor 标记 Supported，更旧标记 EOL
#include "go_provider.hpp"

#include "providers/archive.hpp"
#include "providers/checksum.hpp"
#include "providers/http_client.hpp"
#include "python.hpp"
#include "providers/registry.hpp"

namespace prov {

namespace {

// go 架构令牌：amd64 / arm64 / 386
std::string go_arch() {
    std::string a = py::detect_host_arch();
    return a == "x86" ? "386" : a;
}

std::string minor_of(const std::string& ver) {
    size_t p1 = ver.find('.');
    if (p1 == std::string::npos) return ver;
    size_t p2 = ver.find('.', p1 + 1);
    return p2 == std::string::npos ? ver : ver.substr(0, p2);
}

bool load_all(std::vector<json::Val>& out, std::string& err) {
    std::string body = httpc::get_cached("https://go.dev/dl/?mode=json&include=all", err);
    if (body.empty())
        body = httpc::get_cached("https://golang.google.cn/dl/?mode=json&include=all", err);
    if (body.empty()) {
        err = "获取 Go 版本列表失败: " + err;
        logx::line(err);
        return false;
    }
    json::Val root;
    std::string perr;
    if (!json::Parser(body).parse(root, perr) || root.t != json::Val::Arr) {
        err = "Go 版本解析失败: " + perr;
        logx::line(err);
        return false;
    }
    out = std::move(root.arr);
    return true;
}

} // namespace

std::string GoProvider::id() const { return "go"; }
bool GoProvider::multi_version() const { return false; }
std::string GoProvider::display() const { return "Go"; }

bool GoProvider::list_versions(std::vector<VersionInfo>& out, std::string& err) {
    std::vector<json::Val> entries;
    if (!load_all(entries, err)) return false;
    // 最新两个 minor → Supported，其余 EOL
    std::vector<std::string> minors;
    for (const json::Val& e : entries) {
        bool stable = e.get("stable") && e.get("stable")->boolean_or(false);
        std::string ver = e.get("version") ? e.get("version")->str_or() : "";
        if (!stable || ver.empty()) continue;
        std::string minor = minor_of(ver);
        if (std::find(minors.begin(), minors.end(), minor) == minors.end())
            minors.push_back(minor);
        VersionInfo v;
        v.id = ver;
        v.display = ver;
        v.level = SupportLevel::Eol;
        v.tag_label = "EOL";
        out.push_back(std::move(v));
    }
    int marked = 0;
    std::string cur;
    for (VersionInfo& v : out) {
        std::string minor = minor_of(v.id);
        if (minor != cur) {
            cur = minor;
            if (++marked > 2) break;
        }
        v.level = SupportLevel::Supported;
        v.tag_label = "Supported";
    }
    logx::linef("Go 稳定版本：%d 条（前两个 minor 为 Supported）", (int)out.size());
    return !out.empty();
}

bool GoProvider::resolve(const std::string& version_id, Artifact& out, std::string& err) {
    std::vector<json::Val> entries;
    if (!load_all(entries, err)) return false;
    std::string arch = go_arch();
    for (const json::Val& e : entries) {
        std::string ver = e.get("version") ? e.get("version")->str_or() : "";
        if (ver != version_id) continue;
        const json::Val* fl = e.get("files");
        if (!fl) break;
        for (const json::Val& fe : fl->arr) {
            std::string os = fe.get("os") ? fe.get("os")->str_or() : "";
            std::string a = fe.get("arch") ? fe.get("arch")->str_or() : "";
            std::string kind = fe.get("kind") ? fe.get("kind")->str_or() : "";
            if (os == "windows" && a == arch && kind == "archive") {
                out.version_id = version_id;
                out.version = ver;
                out.filename = fe.get("filename") ? fe.get("filename")->str_or() : "";
                out.sha256 = fe.get("sha256") ? fe.get("sha256")->str_or() : "";
                out.size = (uint64_t)(fe.get("size") ? fe.get("size")->num : 0);
                out.url = "https://go.dev/dl/" + out.filename;
                if (out.filename.empty()) {
                    err = "资产信息不完整";
                    return false;
                }
                return true;
            }
        }
    }
    err = "版本 " + version_id + " 没有 windows/" + arch + " 的压缩包";
    logx::line(err);
    return false;
}

std::vector<std::pair<std::string, std::wstring>> GoProvider::mirrors(const Artifact& a) const {
    std::wstring path = su::utf8_to_wide(a.filename);
    return {{"阿里云镜像", L"https://mirrors.aliyun.com/golang/" + path},
            {"国内官方 golang.google.cn", L"https://golang.google.cn/dl/" + path},
            {"官方 go.dev", su::utf8_to_wide(a.url)}};
}

bool GoProvider::verify(const Artifact& a, const fs::path& dest, std::string& err) {
    return checksum::artifact_ok(a, dest, err);
}

bool GoProvider::install(const Artifact& a, const fs::path& archive_file,
                         const fs::path& ver_dir, std::string& err) {
    // Go 压缩包顶层目录名为 go/，extract_to_ver_dir 会自动合并其内容
    return archive::extract_to_ver_dir(archive_file, ver_dir, err);
}

std::string GoProvider::bin_subdir() const { return "bin"; }
std::vector<std::pair<std::string, std::string>> GoProvider::envs() const {
    return {{"GOROOT", ""}};
}
std::string GoProvider::verify_exe() const { return "bin\\go.exe"; }

namespace {
struct GoRegistrar {
    GoRegistrar() { Registry::instance().add("go", [] { return std::make_unique<GoProvider>(); }); }
};
const GoRegistrar g_go_registrar;
} // namespace

} // namespace prov

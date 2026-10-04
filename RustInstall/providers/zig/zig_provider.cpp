// providers/zig/zig_provider.cpp : Zig 实现
// 版本源：https://ziglang.org/download/index.json（对象键为版本号，"master" 为开发版）
// 每版本含 <arch>-windows 条目：{ tarball, size, shasum }，shasum 为 SHA-256
// 下载镜像：中科大 / npmmirror / 官方，自动切换
#include "zig_provider.hpp"

#include <algorithm>

#include "../../python.hpp"
#include "../../providers/archive.hpp"
#include "../../providers/checksum.hpp"
#include "../../providers/http_client.hpp"
#include "../../providers/registry.hpp"

namespace prov {

namespace {

const char* kIndexUrl = "https://ziglang.org/download/index.json";

// zig 架构键：x86_64-windows / aarch64-windows / x86-windows
std::string zig_arch_key() {
    std::string a = py::detect_host_arch();
    if (a == "amd64") return "x86_64-windows";
    if (a == "arm64") return "aarch64-windows";
    return "x86-windows";
}

} // namespace

std::string ZigProvider::id() const { return "zig"; }
std::string ZigProvider::display() const { return "Zig"; }

bool ZigProvider::list_versions(std::vector<VersionInfo>& out, std::string& err) {
    std::string body = httpc::get_cached(kIndexUrl, err);
    if (body.empty()) {
        err = "获取 Zig 版本索引失败: " + err;
        logx::line(err);
        return false;
    }
    json::Val root;
    std::string perr;
    if (!json::Parser(body).parse(root, perr) || root.t != json::Val::Obj) {
        err = "Zig 版本索引解析失败: " + perr;
        logx::line(err);
        return false;
    }
    for (const auto& kv : root.kv) {
        const std::string& ver = kv.first;
        if (ver.empty() || ver == "master") continue; // 跳过开发版
        const json::Val* e = &kv.second;
        VersionInfo v;
        v.id = ver;
        v.display = ver;
        v.date = e->get("date") ? e->get("date")->str_or() : std::string();
        v.level = SupportLevel::Stable;
        v.tag_label = "Stable";
        out.push_back(std::move(v));
    }
    std::stable_sort(out.begin(), out.end(), [](const VersionInfo& a, const VersionInfo& b) {
        return natural_cmp(a.id, b.id) > 0;
    });
    logx::linef("Zig 版本索引：%d 条", (int)out.size());
    return !out.empty();
}

bool ZigProvider::resolve(const std::string& version_id, Artifact& out, std::string& err) {
    std::string body = httpc::get_cached(kIndexUrl, err);
    if (body.empty()) {
        err = "获取 Zig 版本索引失败: " + err;
        return false;
    }
    json::Val root;
    std::string perr;
    if (!json::Parser(body).parse(root, perr) || root.t != json::Val::Obj) {
        err = "Zig 版本索引解析失败: " + perr;
        return false;
    }
    const json::Val* e = root.get(version_id.c_str());
    if (!e) {
        err = "未找到版本 " + version_id;
        logx::line(err);
        return false;
    }
    std::string key = zig_arch_key();
    const json::Val* arch = e->get(key.c_str());
    if (!arch && key != "x86_64-windows") arch = e->get("x86_64-windows"); // arm64 兜底 x64
    if (!arch) {
        err = "版本 " + version_id + " 没有 " + key + " 的压缩包";
        logx::line(err);
        return false;
    }
    std::string tarball = arch->get("tarball") ? arch->get("tarball")->str_or() : std::string();
    if (tarball.empty()) {
        err = "版本 " + version_id + " 的 " + key + " 条目缺少 tarball";
        return false;
    }
    out.version_id = version_id;
    out.version = version_id;
    out.url = tarball;
    out.sha256 = arch->get("shasum") ? arch->get("shasum")->str_or() : std::string();
    out.size = (uint64_t)(arch->get("size") ? arch->get("size")->num : 0);
    size_t slash = tarball.find_last_of('/');
    out.filename = slash == std::string::npos ? tarball : tarball.substr(slash + 1);
    return true;
}

std::vector<std::pair<std::string, std::wstring>> ZigProvider::mirrors(const Artifact& a) const {
    std::wstring file = su::utf8_to_wide(a.filename);
    return {{"中科大镜像", L"https://mirrors.ustc.edu.cn/zig/" + file},
            {"npmmirror 镜像", L"https://registry.npmmirror.com/-/binary/zig/" + file},
            {"官方 ziglang.org", su::utf8_to_wide(a.url)}};
}

bool ZigProvider::verify(const Artifact& a, const fs::path& dest, std::string& err) {
    return checksum::artifact_ok(a, dest, err);
}

bool ZigProvider::install(const Artifact& a, const fs::path& archive_file,
                          const fs::path& ver_dir, std::string& err) {
    // zip 顶层目录为 zig-windows-<arch>-<ver>/，extract 会自动合并内容
    (void)a;
    return archive::extract_to_ver_dir(archive_file, ver_dir, err);
}

std::string ZigProvider::bin_subdir() const { return ""; }
std::vector<std::pair<std::string, std::string>> ZigProvider::envs() const { return {}; }
std::string ZigProvider::verify_exe() const { return "zig.exe"; }

namespace {
struct ZigRegistrar {
    ZigRegistrar() {
        Registry::instance().add("zig", [] { return std::make_unique<ZigProvider>(); });
    }
};
const ZigRegistrar g_zig_registrar;
} // namespace

} // namespace prov

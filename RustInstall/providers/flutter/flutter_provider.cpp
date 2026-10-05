// providers/flutter/flutter_provider.cpp : Flutter SDK 实现（单版本平铺）
// 版本源：https://storage.googleapis.com/flutter_infra_release/releases/releases_windows.json
//   { base_url, current_release{stable,beta,dev}, releases[ {hash, channel, version,
//     release_date, archive, sha256} ] }，archive 为相对 base_url 的路径
// 仅枚举 stable 通道；下载镜像：flutter-io.cn / npmmirror / 官方，自动回退
#include "flutter_provider.hpp"

#include <algorithm>
#include <cstring>
#include <map>

#include "../../json.hpp"
#include "../../providers/archive.hpp"
#include "../../providers/checksum.hpp"
#include "../../providers/http_client.hpp"
#include "../../providers/registry.hpp"

namespace prov {

namespace {

const char* kIndexUrl =
    "https://storage.googleapis.com/flutter_infra_release/releases/releases_windows.json";
const char* kBaseFallback =
    "https://storage.googleapis.com/flutter_infra_release/releases";
const char* kArchiveMarker = "flutter_infra_release/releases/";

// 拉取并解析 releases_windows.json（get_cached 带本地缓存）
bool load_index(json::Val& root, std::string& err) {
    std::string body = httpc::get_cached(kIndexUrl, err);
    if (body.empty()) {
        err = "获取 Flutter 版本索引失败: " + err;
        logx::line(err);
        return false;
    }
    std::string perr;
    if (!json::Parser(body).parse(root, perr) || root.t != json::Val::Obj) {
        err = "Flutter 版本索引解析失败: " + perr;
        logx::line(err);
        return false;
    }
    return true;
}

const json::Val* releases_array(const json::Val& root) {
    const json::Val* rel = root.get("releases");
    if (!rel || rel->t != json::Val::Arr) return nullptr;
    return rel;
}

// 同一版本号可能出现多条记录（重新打包/hotfix），保留 release_date 最新一条
bool newer_entry(const json::Val& e, const std::string& prev_date) {
    std::string date = e.get("release_date") ? e.get("release_date")->str_or() : std::string();
    return prev_date.empty() || date > prev_date;
}

} // namespace

std::string FlutterProvider::id() const { return "flutter"; }
bool FlutterProvider::multi_version() const { return false; }
std::string FlutterProvider::display() const { return "Flutter"; }

bool FlutterProvider::list_versions(std::vector<VersionInfo>& out, std::string& err) {
    json::Val root;
    if (!load_index(root, err)) return false;
    const json::Val* rel = releases_array(root);
    if (!rel) {
        err = "Flutter 版本索引缺少 releases 数组";
        logx::line(err);
        return false;
    }
    std::map<std::string, size_t> by_ver; // version → out 下标（去重）
    for (const json::Val& e : rel->arr) {
        std::string channel = e.get("channel") ? e.get("channel")->str_or() : std::string();
        std::string ver = e.get("version") ? e.get("version")->str_or() : std::string();
        if (channel != "stable" || ver.empty()) continue;
        auto it = by_ver.find(ver);
        if (it != by_ver.end()) {
            // 同版本重复发布：仅刷新日期为最新（同样截断为日期）
            std::string date =
                e.get("release_date") ? e.get("release_date")->str_or() : std::string();
            if (date.size() > 10) date.resize(10);
            if (date > out[it->second].date) out[it->second].date = date;
            continue;
        }
        VersionInfo v;
        v.id = ver;
        v.display = ver;
        v.date = e.get("release_date") ? e.get("release_date")->str_or() : std::string();
        if (v.date.size() > 10) v.date.resize(10); // ISO 时间截断为日期
        v.level = SupportLevel::Stable;
        v.tag_label = "Stable";
        by_ver[ver] = out.size();
        out.push_back(std::move(v));
    }
    std::stable_sort(out.begin(), out.end(), [](const VersionInfo& a, const VersionInfo& b) {
        return natural_cmp(a.id, b.id) > 0;
    });
    logx::linef("Flutter stable 版本：%d 条", (int)out.size());
    return !out.empty();
}

bool FlutterProvider::resolve(const std::string& version_id, Artifact& out, std::string& err) {
    json::Val root;
    if (!load_index(root, err)) return false;
    const json::Val* rel = releases_array(root);
    if (!rel) {
        err = "Flutter 版本索引缺少 releases 数组";
        return false;
    }
    std::string base = root.get("base_url") ? root.get("base_url")->str_or()
                                            : std::string(kBaseFallback);
    const json::Val* best = nullptr;
    std::string best_date;
    for (const json::Val& e : rel->arr) {
        std::string channel = e.get("channel") ? e.get("channel")->str_or() : std::string();
        std::string ver = e.get("version") ? e.get("version")->str_or() : std::string();
        if (channel != "stable" || ver != version_id) continue;
        std::string date =
            e.get("release_date") ? e.get("release_date")->str_or() : std::string();
        if (!best || date > best_date) {
            best = &e;
            best_date = date;
        }
    }
    if (!best) {
        err = "未找到版本 " + version_id + " 的 stable 压缩包";
        logx::line(err);
        return false;
    }
    std::string archive = best->get("archive") ? best->get("archive")->str_or() : std::string();
    if (archive.empty()) {
        err = "版本 " + version_id + " 缺少 archive 字段";
        return false;
    }
    out.version_id = version_id;
    out.version = version_id;
    out.url = base + "/" + archive;
    out.sha256 = best->get("sha256") ? best->get("sha256")->str_or() : std::string();
    size_t slash = archive.find_last_of('/');
    out.filename = slash == std::string::npos ? archive : archive.substr(slash + 1);
    return true;
}

std::vector<std::pair<std::string, std::wstring>> FlutterProvider::mirrors(const Artifact& a) const {
    // a.url = <base>/<archive>；截取 flutter_infra_release/releases/ 之后的相对路径拼镜像
    std::string rel = a.url;
    size_t p = rel.find(kArchiveMarker);
    if (p == std::string::npos)
        return {{"官方 storage.googleapis.com", su::utf8_to_wide(a.url)}};
    rel = rel.substr(p + std::strlen(kArchiveMarker));
    std::wstring wrel = su::utf8_to_wide(rel);
    return {{"Flutter 国内镜像 flutter-io.cn",
             L"https://storage.flutter-io.cn/flutter_infra_release/releases/" + wrel},
            {"npmmirror 镜像", L"https://registry.npmmirror.com/-/binary/flutter/" + wrel},
            {"官方 storage.googleapis.com", su::utf8_to_wide(a.url)}};
}

bool FlutterProvider::verify(const Artifact& a, const fs::path& dest, std::string& err) {
    return checksum::artifact_ok(a, dest, err);
}

bool FlutterProvider::install(const Artifact& a, const fs::path& archive_file,
                              const fs::path& ver_dir, std::string& err) {
    // zip 顶层目录为 flutter/，extract_to_ver_dir 会自动合并其内容
    (void)a;
    return archive::extract_to_ver_dir(archive_file, ver_dir, err);
}

std::string FlutterProvider::bin_subdir() const { return "bin"; }
std::vector<std::pair<std::string, std::string>> FlutterProvider::envs() const {
    return {{"FLUTTER_ROOT", ""}}; // 空值 = 安装根目录本身
}
std::string FlutterProvider::verify_exe() const { return "bin\\flutter.bat"; }

namespace {
struct FlutterRegistrar {
    FlutterRegistrar() {
        Registry::instance().add("flutter",
                                [] { return std::make_unique<FlutterProvider>(); });
    }
};
const FlutterRegistrar g_flutter_registrar;
} // namespace

} // namespace prov

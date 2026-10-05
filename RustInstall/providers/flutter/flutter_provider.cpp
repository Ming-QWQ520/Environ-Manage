// providers/flutter/flutter_provider.cpp : Flutter SDK 实现（单版本平铺）
// 版本源：https://storage.googleapis.com/flutter_infra_release/releases/releases_windows.json
//   { base_url, current_release{stable,beta,dev}, releases[ {hash, channel, version,
//     release_date, archive, sha256} ] }，archive 为相对 base_url 的路径
// 仅枚举 stable 通道；下载镜像站可选（官方 / 清华 TUNA / 中科大 USTC / 中国社区旧镜像），
// 所选站优先下载，失败自动回退其余镜像；索引 JSON 同样优先走所选镜像
#include "flutter_provider.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

#include "../../json.hpp"
#include "../../platform/platform.hpp"
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

// ---- 镜像站（SDK 下载 base + 用户环境变量） ----
// storage：flutter_infra_release 的各站等效 base（拼 /releases/releases_windows.json 索引
// 与 /releases/<archive> SDK zip）；TUNA/USTC 的 FLUTTER_STORAGE_BASE_URL 本身即该映射根；
// flutter-io.cn 官方同步路径为 storage.flutter-io.cn/flutter_infra_release
// env_pub / env_storage：按镜像站官方说明写入用户环境变量（空 = 官方源，安装后清除变量）
struct FlutterMirror {
    const char* name;
    const char* storage;
    const char* env_pub;     // PUB_HOSTED_URL
    const char* env_storage; // FLUTTER_STORAGE_BASE_URL
};
const FlutterMirror kFlutterMirrors[] = {
    {"官方源 storage.googleapis.com", "https://storage.googleapis.com/flutter_infra_release",
     "", ""},
    {"清华大学 TUNA", "https://mirrors.tuna.tsinghua.edu.cn/flutter",
     "https://mirrors.tuna.tsinghua.edu.cn/dart-pub",
     "https://mirrors.tuna.tsinghua.edu.cn/flutter"},
    {"中科大 USTC", "https://mirrors.ustc.edu.cn/flutter",
     "https://mirrors.ustc.edu.cn/dart-pub", "https://mirrors.ustc.edu.cn/flutter"},
    {"Flutter 中国社区旧镜像", "https://storage.flutter-io.cn/flutter_infra_release",
     "https://pub.flutter-io.cn", "https://storage.flutter-io.cn"},
};
constexpr int kFlutterMirrorCount = (int)(sizeof(kFlutterMirrors) / sizeof(kFlutterMirrors[0]));

// 拉取并解析 releases_windows.json（get_cached 带本地缓存；优先镜像，失败回退官方）
bool load_index(json::Val& root, std::string& err, const char* mirror_storage) {
    std::string body;
    if (mirror_storage && mirror_storage[0]) {
        std::string merr;
        body = httpc::get_cached(std::string(mirror_storage) + "/releases/releases_windows.json",
                                 merr);
        if (body.empty()) logx::line("镜像索引不可用，回退官方源: " + merr);
    }
    if (body.empty()) body = httpc::get_cached(kIndexUrl, err);
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

FlutterProvider::FlutterProvider() {
    // 恢复上次选择的镜像站（注册表 flutter\mirror；非法值回退默认社区镜像）
    std::wstring v = platform::managed_get_value("flutter", L"mirror");
    if (!v.empty()) {
        int idx = _wtoi(v.c_str());
        if (idx >= 0 && idx < kFlutterMirrorCount) mirror_ = idx;
    }
}

std::string FlutterProvider::id() const { return "flutter"; }
bool FlutterProvider::multi_version() const { return false; }
std::string FlutterProvider::display() const { return "Flutter"; }

bool FlutterProvider::list_versions(std::vector<VersionInfo>& out, std::string& err) {
    json::Val root;
    if (!load_index(root, err, kFlutterMirrors[mirror_].storage)) return false;
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
    if (!load_index(root, err, kFlutterMirrors[mirror_].storage)) return false;
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
    // a.url = <base>/<archive>；截取 flutter_infra_release/releases/ 之后的相对路径，
    // 按各镜像站 storage base 重拼下载 URL；所选镜像置顶，其余按序跟随（失败自动回退）
    std::string rel = a.url;
    size_t p = rel.find(kArchiveMarker);
    if (p == std::string::npos)
        return {{"官方 storage.googleapis.com", su::utf8_to_wide(a.url)}};
    rel = rel.substr(p + std::strlen(kArchiveMarker));
    std::wstring wrel = su::utf8_to_wide(rel);
    std::vector<std::pair<std::string, std::wstring>> all;
    for (const FlutterMirror& m : kFlutterMirrors)
        all.push_back({m.name, su::utf8_to_wide(m.storage) + L"/releases/" + wrel});
    int sel = mirror_ >= 0 && mirror_ < kFlutterMirrorCount ? mirror_ : 0;
    std::pair<std::string, std::wstring> pick = all[(size_t)sel];
    all.erase(all.begin() + sel);
    all.insert(all.begin(), std::move(pick));
    return all;
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

// ---- 镜像站选择 ----
std::vector<std::string> FlutterProvider::mirror_options() const {
    std::vector<std::string> out;
    for (const FlutterMirror& m : kFlutterMirrors) out.push_back(m.name);
    return out;
}

int FlutterProvider::mirror_selected() const { return mirror_; }

void FlutterProvider::set_mirror_selected(int idx) {
    if (idx < 0 || idx >= kFlutterMirrorCount || idx == mirror_) return;
    mirror_ = idx;
    std::string err;
    if (!platform::managed_set_value("flutter", L"mirror", std::to_wstring(idx), err))
        logx::line("保存镜像选择失败: " + err);
    logx::linef("下载镜像已切换: %s", kFlutterMirrors[idx].name);
}

std::vector<std::pair<std::string, std::string>> FlutterProvider::mirror_env_vars() const {
    const FlutterMirror& m = kFlutterMirrors[mirror_];
    if (!m.env_pub[0]) return {}; // 官方源：调用方应清除旧镜像变量
    return {{"PUB_HOSTED_URL", m.env_pub}, {"FLUTTER_STORAGE_BASE_URL", m.env_storage}};
}

std::vector<std::string> FlutterProvider::env_cleanup_names() const {
    return {"PUB_HOSTED_URL", "FLUTTER_STORAGE_BASE_URL"};
}

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

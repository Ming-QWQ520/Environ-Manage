// providers/node/node_provider.cpp : Node.js 实现
// 版本源：nodejs.org/dist/index.json（镜像 npmmirror.com/mirrors/node/，重定向至 cdn）
// lts 字段：false → Current；字符串 → LTS（代号）。不能只看版本号奇偶。
#include "node_provider.hpp"

#include "providers/archive.hpp"
#include "providers/checksum.hpp"
#include "providers/http_client.hpp"
#include "python.hpp"
#include "providers/registry.hpp"

namespace prov {

namespace {

const char* kIndexOfficial = "https://nodejs.org/dist/index.json";
const char* kIndexMirror = "https://npmmirror.com/mirrors/node/index.json";
const char* kDistOfficial = "https://nodejs.org/dist/";
const char* kDistMirror = "https://npmmirror.com/mirrors/node/";

struct Entry {
    std::string version, date, lts;
    bool has_lts = false;
    std::vector<std::string> files; // 例: win-x64-zip / win-x64-exe / linux-x64-tar.gz
};

// node 架构令牌：x64 / x86 / arm64
std::string node_arch() {
    std::string a = py::detect_host_arch();
    return a == "amd64" ? "x64" : a;
}

bool load_index(std::vector<Entry>& out, std::string& err) {
    std::string body = httpc::get_cached(kIndexMirror, err);
    if (body.empty()) body = httpc::get_cached(kIndexOfficial, err);
    if (body.empty()) {
        err = "获取 Node.js 版本索引失败: " + err;
        logx::line(err);
        return false;
    }
    json::Val root;
    std::string perr;
    if (!json::Parser(body).parse(root, perr) || root.t != json::Val::Arr) {
        err = "Node.js 索引解析失败: " + perr;
        logx::line(err);
        return false;
    }
    for (const json::Val& e : root.arr) {
        Entry en;
        en.version = e.get("version") ? e.get("version")->str_or() : "";
        en.date = e.get("date") ? e.get("date")->str_or() : "";
        const json::Val* lts = e.get("lts");
        if (lts && lts->t == json::Val::Str) {
            en.lts = lts->s;
            en.has_lts = true;
        }
        const json::Val* fl = e.get("files");
        if (fl && fl->t == json::Val::Arr)
            for (const json::Val& x : fl->arr) en.files.push_back(x.str_or());
        if (!en.version.empty()) out.push_back(std::move(en));
    }
    return true;
}

} // namespace

std::string NodeProvider::id() const { return "node"; }
std::string NodeProvider::display() const { return "Node.js"; }

bool NodeProvider::list_versions(std::vector<VersionInfo>& out, std::string& err) {
    std::vector<Entry> entries;
    if (!load_index(entries, err)) return false;
    for (Entry& e : entries) {
        VersionInfo v;
        v.id = e.version;
        v.display = e.version;
        v.date = e.date;
        if (e.has_lts) {
            v.level = SupportLevel::Lts;
            v.tag_label = "LTS · " + e.lts;
        } else {
            v.level = SupportLevel::Current;
            v.tag_label = "Current";
        }
        out.push_back(std::move(v));
    }
    logx::linef("Node.js 版本索引：%d 条", (int)out.size());
    return true;
}

bool NodeProvider::resolve(const std::string& version_id, Artifact& out, std::string& err) {
    std::vector<Entry> entries;
    if (!load_index(entries, err)) return false;
    std::string arch = node_arch();
    std::string key = "win-" + arch + "-zip";
    for (Entry& e : entries) {
        if (e.version != version_id) continue;
        if (std::find(e.files.begin(), e.files.end(), key) == e.files.end()) {
            err = "版本 " + version_id + " 没有 " + key + " 文件";
            logx::line(err);
            return false;
        }
        out.version_id = version_id;
        out.version = version_id;
        out.filename = "node-" + version_id + "-win-" + arch + ".zip";
        out.url = std::string(kDistOfficial) + version_id + "/" + out.filename;
        return true;
    }
    err = "未找到版本 " + version_id;
    logx::line(err);
    return false;
}

std::vector<std::pair<std::string, std::wstring>> NodeProvider::mirrors(const Artifact& a) const {
    std::string rel = a.version_id + "/" + a.filename;
    return {{"npmmirror 镜像", su::utf8_to_wide(kDistMirror) + su::utf8_to_wide(rel)},
            {"官方 nodejs.org", su::utf8_to_wide(std::string(kDistOfficial) + rel)}};
}

bool NodeProvider::verify(const Artifact& a, const fs::path& dest, std::string& err) {
    // SHASUMS256.txt（镜像优先）
    std::string rel = a.version_id + "/SHASUMS256.txt";
    std::string sums;
    for (const wchar_t* base : {L"https://npmmirror.com/mirrors/node/", L"https://nodejs.org/dist/"}) {
        http::Result r = http::get_text(std::wstring(base) + su::utf8_to_wide(rel),
                                        {http::kUserAgent});
        if (r.ok) {
            sums = std::move(r.body);
            break;
        }
    }
    if (sums.empty()) {
        logx::line("未获取到 SHASUMS256.txt，跳过哈希校验（下载完整性已比对）");
        return true;
    }
    std::string got = checksum::sha256_hex(dest);
    size_t pos = sums.find(got);
    if (got.empty() || pos == std::string::npos ||
        sums.find(a.filename, pos) == std::string::npos) {
        err = "SHASUMS256 校验失败: " + a.filename + "（实际 " + got + "）";
        logx::line(err);
        return false;
    }
    logx::linef("SHASUMS256 校验通过: %s", got.c_str());
    return true;
}

bool NodeProvider::install(const Artifact& a, const fs::path& archive_file,
                           const fs::path& ver_dir, std::string& err) {
    return archive::extract_to_ver_dir(archive_file, ver_dir, err);
}

std::string NodeProvider::bin_subdir() const { return ""; }
std::vector<std::pair<std::string, std::string>> NodeProvider::envs() const { return {}; }
std::string NodeProvider::verify_exe() const { return "node.exe"; }

namespace {
struct NodeRegistrar {
    NodeRegistrar() { Registry::instance().add("node", [] { return std::make_unique<NodeProvider>(); }); }
};
const NodeRegistrar g_node_registrar;
} // namespace

} // namespace prov

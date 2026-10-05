// providers/dotnet/dotnet_provider.cpp : .NET SDK 实现
// 版本源：builds.dotnet.microsoft.com/dotnet/release-metadata/releases-index.json
//        （旧 dotnetcli.blob 域名已失效；响应带 UTF-8 BOM）
// 支持状态：support-phase = active/go-live → 按 release-type 标 LTS/STS；
//          eol → EOL；preview → Preview。以 support-phase/release-type 为准。
#include "dotnet_provider.hpp"

#include "providers/archive.hpp"
#include "providers/checksum.hpp"
#include "providers/http_client.hpp"
#include "python.hpp"
#include "providers/registry.hpp"

namespace prov {

namespace {

const char* kIndexUrl =
    "https://builds.dotnet.microsoft.com/dotnet/release-metadata/releases-index.json";

// 通道详情 JSON（release-metadata/<channel>/releases.json）：
// releases[].release-version == sdkv → sdk.files[]（rid=win-<arch> 且 .zip）→ hash（SHA-512，
// 128 个十六进制字符）；无网络/无缓存/未命中时返回空串，校验退回大小比对
std::string fetch_sdk_sha512(const std::string& channel, const std::string& sdkv,
                             const std::string& arch) {
    std::string url = "https://builds.dotnet.microsoft.com/dotnet/release-metadata/" +
                      channel + "/releases.json";
    std::string perr;
    std::string body = httpc::strip_bom(httpc::get_cached(url, perr));
    if (body.empty()) {
        logx::line("获取 .NET 通道详情失败（SHA-512 预校验不可用）: " + perr);
        return {};
    }
    json::Val root;
    if (!json::Parser(body).parse(root, perr) || root.t != json::Val::Obj) return {};
    const json::Val* rels = root.get("releases");
    if (!rels || rels->t != json::Val::Arr) return {};
    for (const json::Val& r : rels->arr) {
        std::string rv = r.get("release-version") ? r.get("release-version")->str_or() : "";
        if (rv != sdkv) continue;
        const json::Val* sdk = r.get("sdk");
        if (!sdk || sdk->t != json::Val::Obj) continue;
        const json::Val* files = sdk->get("files");
        if (!files || files->t != json::Val::Arr) continue;
        for (const json::Val& f : files->arr) {
            std::string rid = f.get("rid") ? f.get("rid")->str_or() : "";
            std::string name = f.get("name") ? f.get("name")->str_or() : "";
            std::string hash = f.get("hash") ? f.get("hash")->str_or() : "";
            if (rid == "win-" + arch && su::ends_with(name, ".zip") && hash.size() == 128)
                return hash;
        }
    }
    logx::line("通道详情中未找到该 SDK zip 的 SHA-512，退回大小比对");
    return {};
}

bool load_index(std::vector<json::Val>& out, std::string& err) {
    std::string body = httpc::strip_bom(httpc::get_cached(kIndexUrl, err));
    if (body.empty()) {
        err = "获取 .NET 通道索引失败: " + err;
        logx::line(err);
        return false;
    }
    json::Val root;
    std::string perr;
    if (!json::Parser(body).parse(root, perr) || root.t != json::Val::Obj) {
        err = ".NET 通道索引解析失败: " + perr;
        logx::line(err);
        return false;
    }
    const json::Val* ch = root.get("releases-index");
    if (!ch || ch->t != json::Val::Arr) {
        err = ".NET 通道索引缺少 releases-index";
        logx::line(err);
        return false;
    }
    out = ch->arr;
    return true;
}

} // namespace

std::string DotnetProvider::id() const { return "dotnet"; }
bool DotnetProvider::multi_version() const { return false; }
std::string DotnetProvider::display() const { return ".NET"; }

bool DotnetProvider::list_versions(std::vector<VersionInfo>& out, std::string& err) {
    std::vector<json::Val> channels;
    if (!load_index(channels, err)) return false;
    for (const json::Val& c : channels) {
        std::string phase = c.get("support-phase") ? c.get("support-phase")->str_or() : "";
        std::string rtype = c.get("release-type") ? c.get("release-type")->str_or() : "";
        std::string cv = c.get("channel-version") ? c.get("channel-version")->str_or() : "";
        std::string sdkv = c.get("latest-sdk") ? c.get("latest-sdk")->str_or() : "";
        if (cv.empty()) continue;
        VersionInfo v;
        v.id = cv;
        v.display = cv + "  (SDK " + sdkv + ")";
        if (phase == "eol") {
            v.level = SupportLevel::Eol;
            v.tag_label = "EOL";
        } else if (phase == "preview") {
            v.level = SupportLevel::Preview;
            v.tag_label = "Preview";
        } else if (rtype == "lts") {
            v.level = SupportLevel::Lts;
            v.tag_label = "LTS";
        } else {
            v.level = SupportLevel::Sts;
            v.tag_label = "STS";
        }
        v.extra = sdkv;
        out.push_back(std::move(v));
    }
    logx::linef(".NET 通道：%d 个", (int)out.size());
    return !out.empty();
}

bool DotnetProvider::resolve(const std::string& version_id, Artifact& out, std::string& err) {
    std::vector<json::Val> channels;
    if (!load_index(channels, err)) return false;
    for (const json::Val& c : channels) {
        std::string cv = c.get("channel-version") ? c.get("channel-version")->str_or() : "";
        if (cv != version_id) continue;
        std::string sdkv = c.get("latest-sdk") ? c.get("latest-sdk")->str_or() : "";
        if (sdkv.empty()) {
            err = "通道 " + version_id + " 没有 SDK 版本";
            logx::line(err);
            return false;
        }
        std::string arch = py::detect_host_arch() == "arm64" ? "arm64" : "x64";
        out.version_id = version_id;
        out.version = sdkv;
        out.filename = "dotnet-sdk-" + sdkv + "-win-" + arch + ".zip";
        out.url = "https://builds.dotnet.microsoft.com/dotnet/Sdk/" + sdkv + "/" + out.filename;
        out.sha512 = fetch_sdk_sha512(version_id, sdkv, arch); // 官方 SHA-512（可用时硬校验）
        logx::linef(".NET %s → %s", version_id.c_str(), sdkv.c_str());
        return true;
    }
    err = "未找到通道 " + version_id;
    logx::line(err);
    return false;
}

std::vector<std::pair<std::string, std::wstring>> DotnetProvider::mirrors(const Artifact& a) const {
    return {{"官方 builds.dotnet.microsoft.com", su::utf8_to_wide(a.url)}};
}

bool DotnetProvider::verify(const Artifact& a, const fs::path& dest, std::string& err) {
    // release-metadata 提供官方 SHA-512 时硬校验；否则退回大小比对
    return checksum::artifact_ok(a, dest, err);
}

bool DotnetProvider::install(const Artifact& a, const fs::path& archive_file,
                             const fs::path& ver_dir, std::string& err) {
    // .NET SDK zip 为扁平结构，extract_to_ver_dir 自动处理
    return archive::extract_to_ver_dir(archive_file, ver_dir, err);
}

std::string DotnetProvider::bin_subdir() const { return ""; }
std::vector<std::pair<std::string, std::string>> DotnetProvider::envs() const {
    return {{"DOTNET_ROOT", ""}};
}
std::vector<std::pair<std::string, std::string>> DotnetProvider::env_literals() const {
    return {{"DOTNET_CLI_TELEMETRY_OPTOUT", "1"}};
}
std::string DotnetProvider::verify_exe() const { return "dotnet.exe"; }

namespace {
struct DotnetRegistrar {
    DotnetRegistrar() {
        Registry::instance().add("dotnet",
                                 [] { return std::make_unique<DotnetProvider>(); });
    }
};
const DotnetRegistrar g_dotnet_registrar;
} // namespace

} // namespace prov

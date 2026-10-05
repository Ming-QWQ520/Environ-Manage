// providers/gh_releases.hpp : 通用 GitHub Releases 查询（rust-lang/rust 之外的仓库）
// 复用 github::api_get 的线路记忆（直连 → gh-proxy.com → gh-proxy.org），
// 返回 tag / 发布日期 / assets（名称、直链、大小），供各语言 Provider 解析。
#pragma once

#include <string>
#include <vector>

#include "../github.hpp"
#include "../json.hpp"
#include "../logger.hpp"
#include "../strutil.hpp"

namespace gh {

struct RepoAsset {
    std::string name;   // 资产文件名
    std::string url;    // browser_download_url 直链
    std::string digest; // GitHub 官方摘要（"sha256:…"，部分资产可用；空 = 未知）
    uint64_t size = 0;
};

struct RepoRelease {
    std::string tag;  // 如 v2.47.1.windows.1
    std::string date; // YYYY-MM-DD
    std::vector<RepoAsset> assets;
};

// 解析 releases 数组（保留 assets 详情）
inline bool parse_repo_releases(const std::string& body, std::vector<RepoRelease>& out,
                                std::string& err) {
    json::Val root;
    if (!json::Parser(body).parse(root, err)) return false;
    if (root.t != json::Val::Arr) {
        if (const json::Val* m = root.get("message"))
            err = "GitHub：" + m->str_or("未知错误");
        else
            err = "GitHub API 响应格式异常";
        return false;
    }
    for (const json::Val& e : root.arr) {
        if (e.get("draft") && e.get("draft")->boolean_or(false)) continue; // 草稿不可见
        RepoRelease r;
        r.tag = e.get("tag_name") ? e.get("tag_name")->str_or() : std::string();
        if (r.tag.empty()) continue;
        std::string pub = e.get("published_at") ? e.get("published_at")->str_or() : std::string();
        r.date = pub.size() >= 10 ? pub.substr(0, 10) : std::string();
        const json::Val* assets = e.get("assets");
        if (assets && assets->t == json::Val::Arr)
            for (const json::Val& a : assets->arr) {
                RepoAsset sa;
                sa.name = a.get("name") ? a.get("name")->str_or() : std::string();
                sa.url = a.get("browser_download_url")
                             ? a.get("browser_download_url")->str_or()
                             : std::string();
                sa.digest = a.get("digest") ? a.get("digest")->str_or() : std::string();
                // 仅接受 sha256 摘要（前缀剥出 hex 部分）
                if (su::lower(sa.digest).rfind("sha256:", 0) == 0)
                    sa.digest = sa.digest.substr(7);
                else
                    sa.digest.clear();
                sa.size = (uint64_t)(a.get("size") ? a.get("size")->num : 0);
                if (!sa.name.empty() && !sa.url.empty()) r.assets.push_back(std::move(sa));
            }
        out.push_back(std::move(r));
    }
    return true;
}

// 分页获取某仓库的 releases（page 从 1 开始；per_page 通常 100）
inline bool fetch_repo_releases(const std::string& repo, int page, int per_page,
                                std::vector<RepoRelease>& out, std::string& err) {
    std::wstring api = L"/repos/" + su::utf8_to_wide(repo) +
                       L"/releases?per_page=" + std::to_wstring(per_page) +
                       L"&page=" + std::to_wstring(page);
    std::string body;
    if (!github::api_get(api, body, err)) return false;
    return parse_repo_releases(body, out, err);
}

// 下载加速候选：按 API 已记住的可用线路优先排列 gh-proxy 前缀
inline std::vector<std::pair<std::string, std::wstring>> asset_mirrors(const std::string& url) {
    std::wstring wurl = su::utf8_to_wide(url);
    std::vector<std::pair<std::string, std::wstring>> all = {
        {"gh-proxy.com 加速", L"https://gh-proxy.com/" + wurl},
        {"gh-proxy.org 加速", L"https://gh-proxy.org/" + wurl},
        {"GitHub 直连", wurl},
    };
    int first = github::detail::route();
    if (first > 0 && first < (int)all.size()) {
        std::pair<std::string, std::wstring> pick = all[(size_t)first];
        all.erase(all.begin() + first);
        all.insert(all.begin(), std::move(pick));
    }
    return all;
}

} // namespace gh

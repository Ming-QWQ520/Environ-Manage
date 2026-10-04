// github.hpp : 通过 GitHub Releases API 检索 rust-lang/rust 的版本列表
// 直连失败时自动改走 gh-proxy 镜像加速（gh-proxy.com，备用 gh-proxy.org）。
#pragma once

#include <string>
#include <vector>

#include "http.hpp"
#include "json.hpp"
#include "logger.hpp"

namespace github {

struct Release {
    std::string tag;  // 形如 "1.99.0"
    std::string date; // 发布日期 YYYY-MM-DD
};

namespace detail {

// 路由 0=直连 1=gh-proxy.com 2=gh-proxy.org；成功后记住可用线路
inline int& route() {
    static int r = 0;
    return r;
}

inline const wchar_t* proxy_prefix(int idx) {
    switch (idx) {
        case 1: return L"https://gh-proxy.com/";
        case 2: return L"https://gh-proxy.org/";
        default: return L"";
    }
}

} // namespace detail

// 调用 GitHub API。api 为路径+查询串，如 L"/repos/rust-lang/rust/releases?per_page=50&page=1"
inline bool api_get(const std::wstring& api, std::string& body, std::string& err) {
    const std::wstring direct = L"https://api.github.com" + api;
    const std::vector<std::wstring> headers = {http::kUserAgent,
                                               L"Accept: application/vnd.github+json"};

    int order[3] = {0, 1, 2};
    int first = detail::route();
    order[0] = first;
    order[1] = (first + 1) % 3;
    order[2] = (first + 2) % 3;

    std::string last_err;
    for (int attempt = 0; attempt < 3; ++attempt) {
        int idx = order[attempt];
        std::wstring url = idx == 0 ? direct : std::wstring(detail::proxy_prefix(idx)) + direct;
        http::Result r = http::get_text(url, headers);
        logx::linef("GitHub API 线路[%d] %s → HTTP %lu%s", idx,
                    idx == 0 ? "直连" : "gh-proxy 镜像", r.status,
                    r.ok ? "（成功）" : "");
        if (r.ok) {
            detail::route() = idx;
            body = std::move(r.body);
            return true;
        }
        if (r.status == 404) {
            err = "GitHub API 404：" + su::wide_to_utf8(api);
            return false;
        }
        last_err = r.error;
        // 403/429 多为限流，5xx 与网络错误同理 —— 换下一条线路重试
    }
    err = "GitHub API 访问失败（已尝试直连与 gh-proxy 镜像）：" + last_err;
    logx::line("GitHub API 全部线路失败: " + err);
    return false;
}

// 解析 releases 数组
inline bool parse_releases(const std::string& body, std::vector<Release>& out, std::string& err) {
    json::Val root;
    if (!json::Parser(body).parse(root, err)) return false;
    if (root.t != json::Val::Arr) {
        // API 出错时返回对象 {"message": "..."}
        if (const json::Val* m = root.get("message")) err = "GitHub：" + m->str_or("未知错误");
        else err = "GitHub API 响应格式异常";
        return false;
    }
    for (const json::Val& e : root.arr) {
        if (e.get("draft") && e.get("draft")->boolean_or(false)) continue; // 草稿不可见
        Release r;
        r.tag = e.get("tag_name") ? e.get("tag_name")->str_or() : std::string();
        if (r.tag.empty()) continue;
        std::string pub = e.get("published_at") ? e.get("published_at")->str_or() : std::string();
        r.date = pub.size() >= 10 ? pub.substr(0, 10) : std::string();
        out.push_back(std::move(r));
    }
    return true;
}

// 分页获取版本列表（page 从 1 开始）
inline bool fetch_releases(int page, int per_page, std::vector<Release>& out, std::string& err) {
    std::wstring api = L"/repos/rust-lang/rust/releases?per_page=" + std::to_wstring(per_page) +
                       L"&page=" + std::to_wstring(page);
    std::string body;
    if (!api_get(api, body, err)) return false;
    return parse_releases(body, out, err);
}

// 获取单个版本的信息（用于很老的版本补齐发布日期）
inline bool fetch_release(const std::string& tag, Release& out, std::string& err) {
    std::string body;
    std::wstring api = L"/repos/rust-lang/rust/releases/tags/" + su::utf8_to_wide(tag);
    if (!api_get(api, body, err)) return false;
    json::Val root;
    if (!json::Parser(body).parse(root, err)) return false;
    if (root.t != json::Val::Obj) {
        err = "GitHub API 响应格式异常";
        return false;
    }
    out.tag = root.get("tag_name") ? root.get("tag_name")->str_or() : tag;
    std::string pub = root.get("published_at") ? root.get("published_at")->str_or() : std::string();
    out.date = pub.size() >= 10 ? pub.substr(0, 10) : std::string();
    return true;
}

} // namespace github

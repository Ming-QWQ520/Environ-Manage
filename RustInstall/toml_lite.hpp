// toml_lite.hpp : 轻量解析 Rust 发行清单 channel-rust-<版本>.toml
// 只需要按行扫描 [pkg.<名称>.target.<三元组>] 节内的简单 key = value。
#pragma once

#include <string>
#include <vector>

#include "strutil.hpp"

namespace toml {

struct PkgTarget {
    bool available = false;
    std::string url;     // 完整下载 URL（官方源，含日期路径）
    std::string hash;    // sha256（tar.gz 包）
    std::string version; // 包自身版本描述
};

namespace detail {

struct Section {
    bool found = false;
    std::vector<std::pair<std::string, std::string>> kv;
};

// 提取 header 完全匹配的节；遇到下一个 '[' 结束
inline Section section(const std::string& toml, const std::string& header) {
    Section out;
    bool in = false;
    size_t pos = 0;
    while (pos <= toml.size()) {
        size_t eol = toml.find('\n', pos);
        std::string line = su::trim(toml.substr(pos, (eol == std::string::npos ? toml.size() : eol) - pos));
        pos = (eol == std::string::npos) ? toml.size() + 1 : eol + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            if (in) break; // 进入下一节
            in = (line == header);
            if (in) out.found = true;
            continue;
        }
        if (!in) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = su::trim(line.substr(0, eq));
        std::string v = su::trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        out.kv.emplace_back(std::move(k), std::move(v));
    }
    return out;
}

inline std::string get(const Section& s, const char* key) {
    for (auto& e : s.kv)
        if (e.first == key) return e.second;
    return {};
}

} // namespace detail

// 查询某包在某目标平台上的下载信息
inline bool find_pkg_target(const std::string& toml, const std::string& pkg,
                            const std::string& triple, PkgTarget& out) {
    detail::Section s = detail::section(toml, "[pkg." + pkg + ".target." + triple + "]");
    if (!s.found) return false;
    out.available = detail::get(s, "available") == "true";
    out.url = detail::get(s, "url");
    out.hash = detail::get(s, "hash");
    out.version = detail::get(s, "version");
    return true;
}

// 包的描述版本（[pkg.<名称>] 节的 version 字段）
inline std::string pkg_version(const std::string& toml, const std::string& pkg) {
    return detail::get(detail::section(toml, "[pkg." + pkg + "]"), "version");
}

// 列出在某目标平台上 available=true 的所有包名（rust 排最前）
inline std::vector<std::string> list_pkgs(const std::string& toml, const std::string& triple) {
    std::vector<std::string> out;
    const std::string mid = ".target." + triple + "]";
    bool in = false;
    std::string cur;
    bool cur_avail = false;
    size_t pos = 0;
    while (pos <= toml.size()) {
        size_t eol = toml.find('\n', pos);
        std::string line = su::trim(toml.substr(pos, (eol == std::string::npos ? toml.size() : eol) - pos));
        pos = (eol == std::string::npos) ? toml.size() + 1 : eol + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            if (in && cur_avail) out.push_back(cur);
            in = false;
            // 形如 [pkg.<名称>.target.<三元组>]
            if (su::starts_with(line, "[pkg.") && su::ends_with(line, mid)) {
                std::string name = line.substr(5, line.size() - 5 - mid.size());
                if (!name.empty()) {
                    cur = name;
                    in = true;
                    cur_avail = false;
                }
            }
            continue;
        }
        if (in && su::starts_with(line, "available")) {
            size_t eq = line.find('=');
            if (eq != std::string::npos && su::trim(line.substr(0, eq)) == "available")
                cur_avail = su::trim(line.substr(eq + 1)) == "true";
        }
    }
    if (in && cur_avail) out.push_back(cur);
    // rust 包（完整工具链）放最前，其余按名称排序
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == "rust" && i != 0) {
            std::string r = out[i];
            out.erase(out.begin() + i);
            out.insert(out.begin(), r);
            break;
        }
    }
    return out;
}

// 某包所有 available=true 的目标三元组（用于报错提示）
inline std::vector<std::string> list_targets(const std::string& toml, const std::string& pkg) {
    std::vector<std::string> out;
    const std::string prefix = "[pkg." + pkg + ".target.";
    bool in = false;
    std::string cur;
    bool cur_avail = false;
    size_t pos = 0;
    while (pos <= toml.size()) {
        size_t eol = toml.find('\n', pos);
        std::string line = su::trim(toml.substr(pos, (eol == std::string::npos ? toml.size() : eol) - pos));
        pos = (eol == std::string::npos) ? toml.size() + 1 : eol + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            if (in && cur_avail) out.push_back(cur);
            in = false;
            if (su::starts_with(line, prefix) && line.back() == ']') {
                cur = line.substr(prefix.size(), line.size() - prefix.size() - 1);
                in = true;
                cur_avail = false;
            }
            continue;
        }
        if (in && su::starts_with(line, "available")) {
            size_t eq = line.find('=');
            if (eq != std::string::npos && su::trim(line.substr(0, eq)) == "available")
                cur_avail = su::trim(line.substr(eq + 1)) == "true";
        }
    }
    if (in && cur_avail) out.push_back(cur);
    return out;
}

} // namespace toml

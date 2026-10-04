// rust_dist.hpp : Rust 发行文件（static.rust-lang.org）下载与完整安装
// GitHub Releases 只提供版本信息；安装包位于 static.rust-lang.org/dist/，
// 由 channel-rust-<版本>.toml 清单索引（含 sha256），并可经国内镜像加速。
#pragma once

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "http.hpp"
#include "logger.hpp"
#include "sha256.hpp"
#include "strutil.hpp"
#include "toml_lite.hpp"

namespace dist {

namespace fs = std::filesystem;

// --------------------------------------------------------------- 镜像源

struct Mirror {
    const char* name;
    const wchar_t* base; // 以 / 结尾，后接 dist/...
};

// 下载优先级：中科大 → 官方 → 上交（国内镜像通常更快；失败自动切换）
inline const Mirror kMirrors[] = {
    {"官方 static.rust-lang.org", L"https://static.rust-lang.org/"},
    {"中科大 mirrors.ustc.edu.cn", L"https://mirrors.ustc.edu.cn/rust-static/"},
    {"上交 mirror.sjtu.edu.cn", L"https://mirror.sjtu.edu.cn/rust-static/"},
};
inline constexpr int kMirrorCount = 3;

// 自动模式（mirror_hint < 0）的镜像尝试顺序
inline void build_order(int mirror_hint, int (&order)[kMirrorCount]) {
    static const int kAuto[kMirrorCount] = {1, 0, 2};
    if (mirror_hint >= 0) {
        order[0] = mirror_hint;
        order[1] = (mirror_hint + 1) % kMirrorCount;
        order[2] = (mirror_hint + 2) % kMirrorCount;
    } else {
        for (int i = 0; i < kMirrorCount; ++i) order[i] = kAuto[i];
    }
}

inline std::wstring url_for(int mirror, const std::string& rel_path) {
    return std::wstring(kMirrors[mirror].base) + su::utf8_to_wide(rel_path);
}

inline std::string mirror_desc(int mirror) { return kMirrors[mirror].name; }

// --------------------------------------------------------------- 清单与工件

struct Artifact {
    std::string rel_path; // 相对镜像根，形如 "dist/2026-10-01/rust-1.99.0-x86_64-pc-windows-msvc.tar.gz"
    std::string sha256;   // 已知则为空串跳过校验
    uint64_t size = 0;    // 预先探测到的文件大小（可能为 0）
    std::string date;     // dist 日期（已知时）
};

inline std::string strip_base(const std::string& url) {
    // 官方 URL → 相对路径；兼容任意镜像前缀
    size_t p = url.find("/dist/");
    if (p != std::string::npos) return url.substr(p + 1);
    return url;
}

// 从清单解析某包某平台的工件（tar.gz）。mirror_hint：首选镜像（-1 自动）。
inline bool resolve_from_manifest(const std::string& toml, const std::string& pkg,
                                  const std::string& triple, Artifact& out, std::string& err) {
    toml::PkgTarget t;
    if (!toml::find_pkg_target(toml, pkg, triple, t)) {
        err = "清单中不存在包 " + pkg + "（平台 " + triple + "）";
        return false;
    }
    if (!t.available || t.url.empty()) {
        err = "包 " + pkg + " 在平台 " + triple + " 上不可用";
        return false;
    }
    out = Artifact{};
    out.rel_path = strip_base(t.url);
    out.sha256 = t.hash;
    logx::linef("工件解析: 包=%s 平台=%s url=%s sha256=%s", pkg.c_str(), triple.c_str(),
                out.rel_path.c_str(), out.sha256.c_str());
    // 从 URL 提取 dist 日期
    size_t p = out.rel_path.find("dist/");
    if (p != std::string::npos) {
        std::string rest = out.rel_path.substr(p + 5);
        size_t slash = rest.find('/');
        if (slash != std::string::npos) out.date = rest.substr(0, slash);
    }
    return true;
}

// 通过 HEAD 探测把文件大小补齐（尽力而为）
inline void probe_size(Artifact& art, int mirror) {
    uint64_t size = 0;
    unsigned long status = 0;
    std::string err;
    if (http::head(url_for(mirror, art.rel_path), size, status, err)) art.size = size;
}

// 旧版本的 .sha256 伴随文件（内容为 "<hex>  <文件名>"）
inline std::string fetch_sidecar_sha256(const std::string& rel_path, int mirror) {
    http::Result r = http::get_text(url_for(mirror, rel_path + ".sha256"), {http::kUserAgent});
    if (!r.ok) return {};
    std::string hex = su::trim(r.body.substr(0, r.body.find(' ')));
    return hex.size() == 64 ? hex : std::string();
}

namespace detail {

// 民用日期 ↔ 天数（Howard Hinnant 算法），用于老版本日期探测
inline int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

inline void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)yoe + (int)era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}

inline std::string date_after(const std::string& ymd, int add_days) {
    if (ymd.size() < 10) return ymd;
    int y = std::atoi(ymd.substr(0, 4).c_str());
    unsigned m = (unsigned)std::atoi(ymd.substr(5, 2).c_str());
    unsigned d = (unsigned)std::atoi(ymd.substr(8, 2).c_str());
    int64_t z = days_from_civil(y, m, d) + add_days;
    civil_from_days(z, y, m, d);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
    return buf;
}

// 按 Rust 六周发布节奏推算 1.x.0 的发布日期（2015-05-15 为 1.0.0 发布日）。
// GitHub 对 2020 年前的 release 重新导入过，published_at 均为 2020-09-10，不可信。
inline std::string cadence_date(const std::string& ver) {
    int part[3] = {0, 0, 0};
    int n = 0;
    size_t i = 0;
    while (i <= ver.size() && n < 3) {
        int v = 0;
        bool any = false;
        while (i < ver.size() && ver[i] >= '0' && ver[i] <= '9') {
            v = v * 10 + (ver[i] - '0');
            ++i;
            any = true;
        }
        if (!any) break;
        part[n++] = v;
        if (i < ver.size() && ver[i] == '.') ++i;
        else break;
    }
    if (n < 3 || part[0] != 1) return {};
    int64_t z = days_from_civil(2015, 5, 15) + 42 * part[1];
    int y;
    unsigned m, d;
    civil_from_days(z, y, m, d);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
    return buf;
}

} // namespace detail

// --------------------------------------------------------------- 清单获取

// 下载 channel-rust-<版本>.toml（镜像依次尝试；used 返回实际使用的镜像下标）
inline bool fetch_manifest(const std::string& ver, int mirror_hint, std::string& toml, int& used,
                           std::string& err) {
    const std::string rel = "dist/channel-rust-" + ver + ".toml";
    int order[kMirrorCount];
    build_order(mirror_hint, order);
    bool saw_404 = false;
    logx::linef("获取发行清单 %s（mirror=%d）", rel.c_str(), mirror_hint);
    for (int i = 0; i < kMirrorCount; ++i) {
        int idx = order[i];
        http::Result r = http::get_text(url_for(idx, rel), {http::kUserAgent});
        if (r.ok) {
            toml = std::move(r.body);
            used = idx;
            logx::linef("发行清单获取成功（%s，%s）", kMirrors[idx].name,
                        su::human_size(toml.size()).c_str());
            return true;
        }
        logx::linef("发行清单尝试 %s → HTTP %lu", kMirrors[idx].name, r.status);
        if (r.status == 404) saw_404 = true;
    }
    if (saw_404) err = "该版本没有发行清单（1.10 之前的版本走旧版探测）";
    else err = "无法下载发行清单 channel-rust-" + ver + ".toml";
    return false;
}

// 老版本（无清单）：按候选日期探测 dist 文件。
// 日期来源：① 版本号推算（1.x.0 = 2015-05-15 + 42x 天，±10/±45 天窗口）
//           ② GitHub published_at ±7 天（对老版本通常是重导入的错误日期，仅兜底）
inline bool resolve_legacy(const std::string& ver, const std::string& triple,
                           const std::string& pub_date, const std::string& filename,
                           Artifact& out, std::string& err) {
    std::vector<std::string> bases;
    std::vector<int> offsets;
    std::string cd = detail::cadence_date(ver);
    if (!cd.empty()) {
        bases.push_back(cd);
        int minor_point = 0;
        {
            size_t p1 = ver.find('.');
            size_t p2 = p1 == std::string::npos ? std::string::npos : ver.find('.', p1 + 1);
            if (p2 != std::string::npos)
                minor_point = std::atoi(ver.c_str() + p2 + 1);
        }
        int span = minor_point == 0 ? 10 : 45; // 点版本在 x.0 之后一至两周发布
        for (int k = 0; k <= span; ++k) {
            offsets.push_back(k);
            if (k > 0 && k <= 10) offsets.push_back(-k);
        }
    }
    if (!pub_date.empty()) {
        bases.push_back(pub_date);
        for (int k = 0; k <= 7; ++k) offsets.push_back(k);
    }

    std::string tried;
    logx::linef("旧版探测开始: 版本=%s 文件=%s 发布日期=%s", ver.c_str(), filename.c_str(),
                pub_date.c_str());
    for (int off : offsets) {
        for (const std::string& base : bases) {
            std::string date = detail::date_after(base, off);
            Artifact a;
            a.rel_path = "dist/" + date + "/" + filename;
            a.date = date;
            unsigned long status = 0;
            uint64_t size = 0;
            std::string head_err;
            if (http::head(url_for(0, a.rel_path), size, status, head_err)) {
                a.size = size;
                a.sha256 = fetch_sidecar_sha256(a.rel_path, 0);
                logx::linef("旧版探测命中: %s（%s，sha256=%s）", a.rel_path.c_str(),
                            su::human_size(a.size).c_str(),
                            a.sha256.empty() ? "无" : a.sha256.c_str());
                out = std::move(a);
                return true;
            }
            tried += date + " ";
        }
    }
    logx::line("旧版探测未命中，已试日期: " + tried);
    err = "未找到 " + filename + "（已探测日期：" + tried + "）";
    return false;
}

// --------------------------------------------------------------- 下载

// 按镜像优先级下载（失败自动切换下一个镜像）；used 返回实际使用的镜像下标。
// cancelled() 返回 true 时中止（保留断点）。
inline bool download(const Artifact& art, int mirror_hint, const fs::path& dest,
                     const std::function<void(uint64_t, uint64_t)>& progress, int& used,
                     std::string& err, const std::function<bool()>& cancelled = nullptr) {
    int order[kMirrorCount];
    build_order(mirror_hint, order);
    std::string last_err;
    for (int i = 0; i < kMirrorCount; ++i) {
        if (cancelled && cancelled()) {
            err = "已取消";
            logx::line("下载已取消（用户）");
            return false;
        }
        int idx = order[i];
        uint64_t resume_at = 0;
        {
            std::error_code ec;
            if (fs::exists(dest, ec)) resume_at = (uint64_t)fs::file_size(dest, ec);
        }
        logx::linef("下载尝试 %d/%d [%s] %s（断点 %s）", i + 1, kMirrorCount,
                    kMirrors[idx].name, su::wide_to_utf8(dest.wstring()).c_str(),
                    su::human_size(resume_at).c_str());
        auto t0 = GetTickCount64();
        http::DownloadResult r = http::download(url_for(idx, art.rel_path), dest.wstring(), true,
                                                {http::kUserAgent}, progress, cancelled);
        uint64_t ms = GetTickCount64() - t0;
        if (r.ok) {
            used = idx;
            logx::linef("下载完成 [%s] %s 用时 %.1fs%s", kMirrors[idx].name,
                        su::human_size(r.written).c_str(), ms / 1000.0,
                        r.resumed ? "（断点续传）" : "");
            return true;
        }
        if (r.cancelled) {
            err = "已取消";
            logx::linef("下载取消 [%s] 已下载 %s", kMirrors[idx].name,
                        su::human_size(r.written).c_str());
            return false;
        }
        logx::linef("下载失败 [%s] HTTP %lu: %s", kMirrors[idx].name, r.status, r.error.c_str());
        last_err = r.error;
    }
    err = "下载失败（官方/中科大/上交镜像均已尝试）：" + last_err;
    return false;
}

// --------------------------------------------------------------- 进程执行工具

inline bool run_hidden(const fs::path& exe, const std::wstring& args, DWORD& exit_code,
                       std::string& err) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        err = "无法启动 " + su::wide_to_utf8(exe.wstring());
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// 运行命令并捕获首行输出（用于 rustc/cargo --version 验证）
inline std::string run_capture_first_line(const fs::path& exe, const std::wstring& args,
                                          unsigned timeout_ms = 15000) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return {};
    }

    std::string out;
    DWORD waited = 0;
    bool exited = false;
    while (waited < timeout_ms) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD got = 0;
            if (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
            else break;
            if (out.size() > 64 * 1024) break;
        }
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            exited = true;
            // 进程退出后再把剩余输出读干净
            for (;;) {
                DWORD avail = 0;
                if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
                char buf[4096];
                DWORD got = 0;
                if (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
                else break;
            }
            break;
        }
        waited += 100;
    }
    if (!exited) TerminateProcess(pi.hProcess, 1);
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    size_t end = out.find('\n');
    std::string line = su::trim(out.substr(0, end == std::string::npos ? out.size() : end));
    return line;
}

// --------------------------------------------------------------- 完整安装

// 移动单个文件到目标位置（父目录自动创建，目标存在则覆盖；跨卷退回复制）
inline bool move_file_into(const fs::path& src, const fs::path& dst, std::string& err) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (fs::exists(dst)) fs::remove(dst, ec);
    fs::rename(src, dst, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove(src, ec);
    }
    if (ec) {
        err = "无法移动 " + su::wide_to_utf8(src.wstring()) + "：" + ec.message();
        return false;
    }
    return true;
}

// 递归把 src 合并进 dst_parent（文件级覆盖）
inline bool move_tree(const fs::path& src, const fs::path& dst_parent, std::string& err) {
    fs::path dst = dst_parent / src.filename();
    std::error_code ec;
    if (fs::is_directory(src)) {
        fs::create_directories(dst, ec);
        if (ec) {
            err = "无法创建目录 " + su::wide_to_utf8(dst.wstring()) + "：" + ec.message();
            return false;
        }
        for (const fs::directory_entry& e : fs::directory_iterator(src))
            if (!move_tree(e.path(), dst, err)) return false;
        fs::remove(src, ec);
        return true;
    }
    return move_file_into(src, dst, err);
}

// 按 rust-installer 的 manifest.in 安装组件目录：把 file:<路径> / dir:<路径> 指向的
// 内容从组件目录合并进安装根目录（与官方 install.sh 的放置规则一致）
inline bool install_component_dir(const fs::path& comp, const fs::path& install_dir,
                                  std::string& err) {
    std::ifstream in(comp / L"manifest.in");
    if (!in) {
        err = "无法读取 " + su::wide_to_utf8((comp / L"manifest.in").wstring());
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        line = su::trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string kind = line.substr(0, colon);
        std::string rel = line.substr(colon + 1);
        if (rel.empty()) continue;
        fs::path src = comp / su::utf8_to_wide(rel);
        fs::path dst = install_dir / su::utf8_to_wide(rel);
        std::error_code ec;
        if (kind == "file") {
            if (!move_file_into(src, dst, err)) return false;
        } else if (kind == "dir") {
            if (!fs::exists(src)) continue; // 清单允许空缺
            fs::path dst_parent = dst.parent_path();
            fs::create_directories(dst_parent, ec);
            if (fs::exists(dst)) {
                // 目标已存在：逐项合并后删除源目录
                for (const fs::directory_entry& e : fs::directory_iterator(src))
                    if (!move_tree(e.path(), dst, err)) return false;
                fs::remove(src, ec);
            } else {
                fs::rename(src, dst, ec);
                if (ec) {
                    ec.clear();
                    if (!move_tree(src, dst_parent, err)) return false;
                }
            }
        }
    }
    return true;
}

// 解压 tar.gz 并合并到安装目录，返回 bin 目录是否存在
inline bool install_tar_gz(const fs::path& archive, const fs::path& install_dir, std::string& err) {
    wchar_t tar_exe[MAX_PATH * 2] = {};
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\tar.exe", tar_exe, MAX_PATH * 2);
    if (GetFileAttributesW(tar_exe) == INVALID_FILE_ATTRIBUTES) {
        err = "未找到 Windows 自带的 tar.exe（需要 Windows 10 1803+）";
        return false;
    }

    std::error_code ec;
    fs::path tmp = install_dir / L"_rust_extract_tmp";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) {
        err = "无法创建临时解压目录：" + ec.message();
        return false;
    }

    logx::linef("解压开始: %s → %s", su::wide_to_utf8(archive.wstring()).c_str(),
                su::wide_to_utf8(install_dir.wstring()).c_str());
    std::wstring args = L"-xzf \"" + archive.wstring() + L"\" -C \"" + tmp.wstring() + L"\"";
    DWORD code = 0;
    if (!run_hidden(tar_exe, args, code, err) || code != 0) {
        if (err.empty()) err = "解压失败（tar 退出码 " + std::to_string(code) + "，压缩包可能损坏）";
        logx::line("解压失败: " + err);
        fs::remove_all(tmp, ec);
        return false;
    }
    logx::line("解压完成（tar 退出码 0）");

    // 找到压缩包内的顶层目录（rust-<版本>-<三元组>/）
    fs::path top;
    int count = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(tmp)) {
        top = e.path();
        ++count;
    }
    if (count != 1 || !fs::is_directory(top)) {
        err = "压缩包结构异常（顶层目录数 " + std::to_string(count) + "）";
        fs::remove_all(tmp, ec);
        return false;
    }

    for (const fs::directory_entry& e : fs::directory_iterator(top))
        if (!move_tree(e.path(), install_dir, err)) {
            fs::remove_all(tmp, ec);
            return false;
        }
    fs::remove_all(tmp, ec);

    // 完整包：顶层为各组件目录（cargo/、rustc/、rust-std-*/…），按其 manifest.in
    // 合并安装（等价于 rust-installer 的 install.sh）；组件包（如 rustfmt-preview）
    // 的载荷在 <组件名>/ 下一层，同样由 manifest.in 覆盖。
    std::vector<fs::path> comp_dirs;
    for (const fs::directory_entry& e : fs::directory_iterator(install_dir))
        if (e.is_directory() && fs::exists(e.path() / L"manifest.in")) comp_dirs.push_back(e.path());
    for (const fs::path& comp : comp_dirs) {
        logx::linef("合并组件: %s", su::wide_to_utf8(comp.filename().wstring()).c_str());
        if (!install_component_dir(comp, install_dir, err)) {
            logx::line("组件合并失败: " + err);
            return false;
        }
        fs::remove_all(comp, ec);
    }

    // 兜底：无 manifest.in 但恰好存在一个含 bin 的子目录 → 整体上移
    if (!fs::exists(install_dir / L"bin")) {
        fs::path payload;
        int candidates = 0;
        for (const fs::directory_entry& e : fs::directory_iterator(install_dir))
            if (e.is_directory() && fs::exists(e.path() / L"bin")) {
                payload = e.path();
                ++candidates;
            }
        if (candidates == 1) {
            for (const fs::directory_entry& e : fs::directory_iterator(payload))
                if (!move_tree(e.path(), install_dir, err)) return false;
            fs::remove(payload, ec);
        }
    }
    if (!fs::exists(install_dir / L"bin")) {
        err = "解压后未在安装目录找到 bin 目录";
        logx::line("安装失败: " + err);
        return false;
    }
    logx::linef("安装完成: %s", su::wide_to_utf8(install_dir.wstring()).c_str());
    return true;
}

// 把 bin 目录加入用户 PATH（HKCU\Environment），已存在则直接返回成功
inline bool add_to_user_path(const fs::path& bin, std::string& err) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        err = "打开注册表 HKCU\\Environment 失败（错误码 " + std::to_string(rc) + "）";
        return false;
    }
    std::wstring cur;
    DWORD type = 0, size = 0;
    rc = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size);
    if (rc == ERROR_SUCCESS && size > 0) {
        cur.resize(size / 2);
        RegQueryValueExW(key, L"Path", nullptr, &type, (BYTE*)cur.data(), &size);
        while (!cur.empty() && cur.back() == L'\0') cur.pop_back();
    }
    if (su::path_list_contains(cur, bin.wstring())) {
        RegCloseKey(key);
        logx::line("PATH 已包含该目录，无需修改");
        return true;
    }
    if (!cur.empty() && cur.back() != L';') cur += L";";
    cur += bin.wstring();
    rc = RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ, (const BYTE*)cur.c_str(),
                        (DWORD)((cur.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        err = "写入用户 PATH 失败（错误码 " + std::to_string(rc) + "）";
        logx::line("PATH 写入失败: " + err);
        return false;
    }
    logx::linef("已写入用户 PATH: %s", su::wide_to_utf8(bin.wstring()).c_str());
    // 通知系统环境变量已变更（资源管理器据此刷新）
    DWORD_PTR resp = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 3000, &resp);
    return true;
}

} // namespace dist

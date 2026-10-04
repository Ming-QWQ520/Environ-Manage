// providers/provider.cpp : Provider 公共流程
//   安装    下载 → 校验 → 解压（多版本：<root>\<版本>；平铺：<root>）→ junction（多版本）
//           → PATH/环境变量 → 验证
//   卸载    多版本：删除版本目录 → 维护 junction → 清理 PATH/环境变量与受管记录
//           平铺：删除整个根目录 → 清理 PATH/环境变量与受管记录
#include "provider.hpp"

#include "providers/archive.hpp"
#include "providers/checksum.hpp"
#include "providers/http_client.hpp"
#include "platform/platform.hpp"

namespace prov {

namespace {

// 平铺布局版本捕获：运行 verify_exe --version，取输出中第一个含数字的 token
// （go1.22.0 / 8.0.425 / 0.13.0 / 8.3.14 / 3.3.5 / 2.47.1）
std::string flat_capture_version(Provider& p, const fs::path& root) {
    std::error_code ec;
    fs::path exe = root / su::utf8_to_wide(p.verify_exe());
    if (!fs::is_regular_file(exe, ec)) return {};
    std::wstring flag = p.id() == "jdk" ? L"-version" : L"--version";
    std::string line = platform::capture_first_line(exe, flag);
    size_t i = 0;
    while (i < line.size()) {
        size_t b = line.find_first_not_of(" \t\r\n", i);
        if (b == std::string::npos) break;
        size_t e = line.find_first_of(" \t\r\n", b);
        if (e == std::string::npos) e = line.size();
        std::string tok = line.substr(b, e - b);
        for (char c : tok)
            if (isdigit((unsigned char)c)) return tok;
        i = e;
    }
    return {};
}

} // namespace

bool install_to_root(Provider& p, const Artifact& a, const fs::path& root, bool add_path,
                     bool repair, bool switch_current,
                     const std::function<void(uint64_t, uint64_t)>& progress,
                     const std::function<bool()>& cancelled, std::string& verify_line,
                     std::string& err) {
    // 布局分派：多版本 <root>\<版本>（JDK/Node.js）；单版本平铺直接 <root>（其余语言）
    bool multi = p.multi_version();
    fs::path ver_dir = multi ? root / su::utf8_to_wide(a.version) : root;
    fs::path archives = root / (multi ? "archives" : "_archives");
    if (!platform::ensure_dir(archives, err)) return false;
    fs::path dest = archives / a.filename;

    // 修复模式：本地已有压缩包则离线重解压，缺失时自动下载
    std::error_code ec0;
    if (repair && fs::exists(dest, ec0))
        logx::line("修复模式：使用本地已有安装包（跳过下载）");

    int used = -1;
    if (!(repair && fs::exists(dest, ec0))) {
        if (!httpc::download_mirrors(p.mirrors(a), dest, progress, used, err, cancelled))
            return false;
    }
    logx::linef("下载源: %s", p.mirrors(a)[(size_t)used].first.c_str());
    if (!p.verify(a, dest, err)) return false;
    if (!p.install(a, dest, ver_dir, err)) return false;

    // current junction（仅多版本语言：<root>\current → <root>\<版本>；平铺无 junction）
    fs::path base = multi ? root / L"current" : root;
    if (multi && switch_current)
        if (!platform::make_junction(base, ver_dir, err)) return false;

    if (add_path) {
        fs::path bin = base;
        if (!p.bin_subdir().empty()) bin /= su::utf8_to_wide(p.bin_subdir());
        if (!platform::add_path_dir(bin, err)) return false;
        for (auto& ev : p.envs()) {
            fs::path v = base;
            if (!ev.second.empty()) v /= su::utf8_to_wide(ev.second);
            if (!platform::set_user_env(ev.first, v.wstring(), err)) return false;
        }
        for (auto& ev : p.env_literals())
            if (!platform::set_user_env(ev.first, su::utf8_to_wide(ev.second), err))
                return false;
    }

    platform::managed_add_root(p.id(), root); // 记录受管根目录（供检测/卸载使用）

    fs::path vexe = base / su::utf8_to_wide(p.verify_exe());
    if (fs::exists(vexe)) {
        std::wstring flag = p.id() == "jdk" ? L"-version" : L"--version";
        verify_line = platform::capture_first_line(vexe, flag);
        if (!verify_line.empty()) logx::line("验证: " + verify_line);
    }
    return true;
}

// ------------------------------------------------------- 已装扫描与卸载

// 扫描单个根目录下的版本子目录（校验 verify_exe 存在），返回（版本名, 版本目录）
std::vector<std::pair<std::string, fs::path>> scan_root_versions(Provider& p,
                                                                 const fs::path& root) {
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory()) continue;
        std::string name = su::wide_to_utf8(e.path().filename().wstring());
        if (name.empty() || name == "current" || name == "archives" ||
            name == "pnpm-repository" || name[0] == '_' || name[0] == '.')
            continue;
        if (!fs::exists(e.path() / su::utf8_to_wide(p.verify_exe()), ec)) continue;
        out.push_back({name, e.path()});
    }
    return out;
}

// 扫描注册表记录的全部受管根目录；
// 多版本语言：返回各版本子目录；平铺语言：根目录本体（版本经 --version 捕获）+ 历史版本子目录
std::vector<ManagedInstall> managed_installs(Provider& p) {
    std::vector<ManagedInstall> out;
    bool multi = p.multi_version();
    for (const std::wstring& r : platform::managed_get_roots(p.id())) {
        fs::path root(r);
        if (!multi) {
            // 平铺：根目录即安装目录
            std::error_code fec;
            if (fs::exists(root / su::utf8_to_wide(p.verify_exe()), fec))
                out.push_back({flat_capture_version(p, root), root, true});
        }
        for (auto& v : scan_root_versions(p, root))
            out.push_back({v.first, v.second, false});
    }
    return out;
}

// 卸载指定版本；平铺语言 root 即安装目录（version 仅用于日志）
bool uninstall_version(Provider& p, const fs::path& root, const std::string& version,
                       std::string& err) {
    std::error_code ec;
    if (!p.multi_version()) {
        // ---- 平铺布局：整个根目录即安装目录 ----
        if (!fs::is_directory(root, ec) ||
            !fs::exists(root / su::utf8_to_wide(p.verify_exe()), ec)) {
            err = "目录不是受本工具管理的 " + p.display() + " 安装目录：" +
                  su::wide_to_utf8(root.wstring());
            return false;
        }
        logx::linef("正在删除 %s 安装目录: %s", p.display().c_str(),
                    su::wide_to_utf8(root.wstring()).c_str());
        // 旧版工具在此目录留下过 junction 时一并清理
        fs::path legacy = root / L"current";
        if (fs::exists(legacy, ec)) fs::remove_all(legacy, ec);
        fs::remove_all(root, ec);
        if (ec) {
            err = "删除安装目录失败：" + ec.message() +
                  "（可能有程序正在使用该目录，请关闭后重试）";
            return false;
        }
        fs::path bin = root;
        if (!p.bin_subdir().empty()) bin /= su::utf8_to_wide(p.bin_subdir());
        std::string perr;
        platform::remove_path_dir(bin, perr);
        for (auto& ev : p.envs()) {
            std::string e2;
            platform::remove_user_env(ev.first, e2);
        }
        for (auto& ev : p.env_literals()) {
            std::string e2;
            platform::remove_user_env(ev.first, e2);
        }
        platform::managed_remove_root(p.id(), root);
        logx::linef("已卸载 %s（%s）", p.display().c_str(), version.c_str());
        return true;
    }

    fs::path ver_dir = root / su::utf8_to_wide(version);
    if (!fs::is_directory(ver_dir, ec)) {
        err = "未找到版本目录 " + su::wide_to_utf8(ver_dir.wstring());
        return false;
    }
    if (!fs::exists(ver_dir / su::utf8_to_wide(p.verify_exe()), ec)) {
        err = "目录不是受本工具管理的 " + p.display() + " 版本目录：" +
              su::wide_to_utf8(ver_dir.wstring());
        return false;
    }

    // junction 指向该版本时先移除链接本身（不动目标目录）
    fs::path junction = root / L"current";
    bool junction_here = false;
    if (fs::exists(junction, ec)) {
        std::error_code ec2;
        junction_here = fs::equivalent(junction, ver_dir, ec2);
        if (junction_here) {
            fs::remove(junction, ec2);
            logx::linef("已移除 junction: %s", su::wide_to_utf8(junction.wstring()).c_str());
        }
    }

    logx::linef("正在删除版本目录: %s", su::wide_to_utf8(ver_dir.wstring()).c_str());
    fs::remove_all(ver_dir, ec);
    if (ec) {
        err = "删除版本目录失败：" + ec.message() +
              "（可能有程序正在使用该目录，请关闭后重试）";
        return false;
    }

    // 剩余版本 → junction 重新指向最近使用的版本；无剩余 → 清理 PATH/环境变量/受管记录
    auto remaining = scan_root_versions(p, root);
    if (!remaining.empty()) {
        fs::path best;
        std::error_code tec;
        fs::file_time_type best_time{};
        for (auto& v : remaining) {
            fs::file_time_type t = fs::last_write_time(v.second, tec);
            if (tec || best.empty() || best_time < t) {
                best = v.second;
                best_time = t;
            }
        }
        if (!platform::make_junction(junction, best, err)) return false;
        logx::linef("current 已切换到: %s", su::wide_to_utf8(best.filename().wstring()).c_str());
        return true;
    }

    fs::remove(junction, ec); // 清理残留 junction（可能指向被删目录）
    fs::path bin = junction;
    if (!p.bin_subdir().empty()) bin /= su::utf8_to_wide(p.bin_subdir());
    std::string perr;
    platform::remove_path_dir(bin, perr);
    for (auto& ev : p.envs()) {
        std::string e2;
        platform::remove_user_env(ev.first, e2);
    }
    for (auto& ev : p.env_literals()) {
        std::string e2;
        platform::remove_user_env(ev.first, e2);
    }
    platform::managed_remove_root(p.id(), root);
    return true;
}

// 版本名自然比较（数字段按数值比较）：供各 Provider 排序版本列表使用
int natural_cmp(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && isdigit((unsigned char)a[i2])) ++i2;
            while (j2 < b.size() && isdigit((unsigned char)b[j2])) ++j2;
            long long va = atoll(a.substr(i, i2 - i).c_str());
            long long vb = atoll(b.substr(j, j2 - j).c_str());
            if (va != vb) return va < vb ? -1 : 1;
            i = i2;
            j = j2;
        } else {
            if (a[i] != b[j]) return (unsigned char)a[i] < (unsigned char)b[j] ? -1 : 1;
            ++i;
            ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

} // namespace prov

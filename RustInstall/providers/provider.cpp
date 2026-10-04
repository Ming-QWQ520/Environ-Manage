// providers/provider.cpp : Provider 公共安装流程（下载 → 校验 → 解压 → current → PATH/环境变量）
#include "provider.hpp"

#include "providers/archive.hpp"
#include "providers/checksum.hpp"
#include "providers/http_client.hpp"
#include "platform/platform.hpp"

namespace prov {

bool install_to_root(Provider& p, const Artifact& a, const fs::path& root, bool add_path,
                     bool repair, bool switch_current,
                     const std::function<void(uint64_t, uint64_t)>& progress,
                     const std::function<bool()>& cancelled, std::string& verify_line,
                     std::string& err) {
    fs::path ver_dir = p.multi_version()
                           ? root / "versions" / su::utf8_to_wide(p.id()) /
                                 su::utf8_to_wide(a.version)
                           : root;
    fs::path archives = root / "archives";
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

    fs::path base = p.multi_version()
                        ? root / su::utf8_to_wide(p.id()) / L"current"
                        : root;
    if (p.multi_version() && switch_current)
        if (!platform::make_junction(base, ver_dir, err)) return false;

    if (add_path) {
        fs::path bin = base;
        if (!p.bin_subdir().empty()) bin /= su::utf8_to_wide(p.bin_subdir());
        if (!platform::add_path_dir(bin, err)) return false;
        for (auto& ev : p.envs()) {
            fs::path v = base;
            if (!ev.second.empty()) v /= su::utf8_to_wide(ev.second);
            if (!platform::set_user_env(ev.first, v, err)) return false;
        }
        for (auto& ev : p.env_literals())
            if (!platform::set_user_env(ev.first, su::utf8_to_wide(ev.second), err))
                return false;
    }

    fs::path vexe = base / su::utf8_to_wide(p.verify_exe());
    if (fs::exists(vexe)) {
        std::wstring flag = p.id() == "jdk" ? L"-version" : L"--version";
        verify_line = platform::capture_first_line(vexe, flag);
        if (!verify_line.empty()) logx::line("验证: " + verify_line);
    }
    return true;
}

} // namespace prov

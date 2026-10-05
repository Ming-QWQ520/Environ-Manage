// providers/flutter/flutter_provider.hpp : Flutter Provider（Flutter 官方 releases_windows.json）
#pragma once

#include "../provider.hpp"

namespace prov {

class FlutterProvider : public Provider {
public:
    FlutterProvider(); // 从注册表恢复上次选择的下载镜像

    std::string id() const override;
    std::string display() const override;
    bool list_versions(std::vector<VersionInfo>& out, std::string& err) override;
    bool resolve(const std::string& version_id, Artifact& out, std::string& err) override;
    std::vector<std::pair<std::string, std::wstring>> mirrors(const Artifact& a) const override;
    bool verify(const Artifact& a, const fs::path& dest, std::string& err) override;
    bool install(const Artifact& a, const fs::path& archive_file, const fs::path& ver_dir,
                 std::string& err) override;
    bool multi_version() const override; // 单版本平铺：直接安装于根目录
    std::string bin_subdir() const override;
    std::vector<std::pair<std::string, std::string>> envs() const override;
    std::string verify_exe() const override;

    // ---- 镜像站选择（官方 / 清华 TUNA / 中科大 USTC / 中国社区旧镜像） ----
    std::vector<std::string> mirror_options() const override;
    int mirror_selected() const override;
    void set_mirror_selected(int idx) override;
    // 所选镜像的 PUB_HOSTED_URL / FLUTTER_STORAGE_BASE_URL（官方源返回空 = 调用方清除）
    std::vector<std::pair<std::string, std::string>> mirror_env_vars() const override;
    // 卸载时清理这两个镜像环境变量
    std::vector<std::string> env_cleanup_names() const override;

private:
    int mirror_ = 3; // 默认：Flutter 中国社区旧镜像（原回退链首选项）
};

} // namespace prov

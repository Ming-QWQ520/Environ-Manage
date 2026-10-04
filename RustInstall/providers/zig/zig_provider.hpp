// providers/zig/zig_provider.hpp : Zig Provider（ziglang.org/download/index.json）
#pragma once

#include "../provider.hpp"

namespace prov {

class ZigProvider : public Provider {
public:
    std::string id() const override;
    std::string display() const override;
    bool list_versions(std::vector<VersionInfo>& out, std::string& err) override;
    bool resolve(const std::string& version_id, Artifact& out, std::string& err) override;
    std::vector<std::pair<std::string, std::wstring>> mirrors(const Artifact& a) const override;
    bool verify(const Artifact& a, const fs::path& dest, std::string& err) override;
    bool install(const Artifact& a, const fs::path& archive_file, const fs::path& ver_dir,
                 std::string& err) override;
    std::string bin_subdir() const override;
    std::vector<std::pair<std::string, std::string>> envs() const override;
    std::string verify_exe() const override;
};

} // namespace prov

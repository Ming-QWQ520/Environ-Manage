// providers/provider.hpp : Provider 接口与公共数据结构（UI 只依赖本文件与 registry.hpp）
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "logger.hpp"
#include "strutil.hpp"

namespace prov {

namespace fs = std::filesystem;

// 统一支持级别（各 Provider 自行判定，对外输出一致）
enum class SupportLevel {
    Lts,      // 长期支持（绿色）
    Sts,      // 标准期限支持（蓝色）
    Supported,// 仍在支持期（绿色，如 Go 前两个 minor）
    Stable,   // 稳定版（蓝色，如 JDK 非 LTS 功能版）
    Current,  // 当前线（青色，如 Node 非 LTS）
    Eol,      // 已停止支持（灰色）
    Preview,  // 预览版（黄色）
    Unknown,
};

struct VersionInfo {
    std::string id;       // 选择器（node: v22.14.0；jdk: 21；dotnet: 8.0）
    std::string display;  // 版本号列
    std::string date;
    SupportLevel level = SupportLevel::Unknown;
    std::string tag_label; // 完整标签（"LTS · Iron" / "LTS" / "Supported" / "EOL" / "STS"…）
    std::string extra;     // 附加信息（如 .NET 的 SDK 版本号）
};

struct Artifact {
    std::string version_id; // 所属 VersionInfo::id
    std::string version;    // 具体版本号（如 21.0.12+1 / 8.0.425 / v22.14.0 / go1.22.0）
    std::string filename;
    std::string url;        // 官方直链
    std::string sha256, sha512, md5;
    uint64_t size = 0;
};

class Provider {
public:
    virtual ~Provider() = default;

    virtual std::string id() const = 0;
    virtual std::string display() const = 0;

    // 版本枚举（支持级别由 Provider 判定）
    virtual bool list_versions(std::vector<VersionInfo>& out, std::string& err) = 0;
    // 解析某版本在 Windows 上的具体文件（按主机架构）
    virtual bool resolve(const std::string& version_id, Artifact& out, std::string& err) = 0;
    // 镜像候选：(名称, 完整下载 URL)，按优先级排序 —— 回退逻辑由公共层驱动
    virtual std::vector<std::pair<std::string, std::wstring>> mirrors(const Artifact& a) const = 0;
    // 完整性校验（SHA-256/MD5/大小；无校验和时由实现说明策略）
    virtual bool verify(const Artifact& a, const fs::path& dest, std::string& err) = 0;
    // 解压到版本目录（各语言布局差异在此实现）；archive_file 为已下载的压缩包
    virtual bool install(const Artifact& a, const fs::path& archive_file,
                         const fs::path& ver_dir, std::string& err) = 0;

    // ---- 安装布局 ----
    // 多版本（JDK/Node.js）： <root>\<版本> + current junction，支持共存/切换/按版本卸载
    // 单版本平铺（Go/.NET/Zig/PHP/Ruby/Git/Flutter）：直接安装于 <root>，更新即覆盖，卸载删整个根目录
    virtual bool multi_version() const { return true; }

    // ---- 可选下载镜像站（非空时确认页提供 M 键切换；选择由实现持久化记忆） ----
    virtual std::vector<std::string> mirror_options() const { return {}; }
    virtual int mirror_selected() const { return 0; }
    virtual void set_mirror_selected(int) {}

    // ---- 镜像相关用户环境变量 ----
    // 安装成功后由调用方设置（值空 = 清除该变量，如选择官方源时清除镜像指向）
    virtual std::vector<std::pair<std::string, std::string>> mirror_env_vars() const
        { return {}; }
    // 卸载时额外清理的用户环境变量名（无论当前选择如何都尝试删除）
    virtual std::vector<std::string> env_cleanup_names() const { return {}; }

    // ---- 布局描述（多版本：current 下相对路径；平铺：根目录下相对路径） ----
    virtual std::string bin_subdir() const = 0;                                // PATH 子目录
    virtual std::vector<std::pair<std::string, std::string>> envs() const = 0; // 名 → current 相对值
    virtual std::vector<std::pair<std::string, std::string>> env_literals() const
        { return {}; }                                                         // 字面量环境变量
    virtual std::string verify_exe() const = 0;                                // 版本目录下验证可执行文件
};

// 公共安装流程（provider.cpp）：下载 → 校验 → 解压 → junction → PATH/环境变量 → 验证
// 统一版本化布局：<root>\<版本>，<root>\current junction 指向当前版本（所有语言一致）
bool install_to_root(Provider& p, const Artifact& a, const fs::path& root, bool add_path,
                     bool repair, bool switch_current,
                     const std::function<void(uint64_t, uint64_t)>& progress,
                     const std::function<bool()>& cancelled, std::string& verify_line,
                     std::string& err);

// ---- 安装管线阶段通知（供 TUI 步骤条 / 批处理提示） ----
// 阶段常量：0=下载 1=校验 2=解压 3=合并 4=配置 5=完成
// 由 install_to_root 在进入各阶段时触发；UI 经 set_stage_hook 接收
using StageHook = std::function<void(int)>;
void set_stage_hook(StageHook cb);
void clear_stage_hook();
void stage_notify(int stage);

// ---- 已装扫描与卸载 ----
// 一条受管安装记录：多版本语言为版本子目录；平铺语言 flat_root=true（dir 即安装根目录）
struct ManagedInstall {
    std::string version;
    fs::path dir;
    bool flat_root = false;
};
// 扫描单个根目录下的版本子目录（校验 verify_exe 存在），返回（版本名, 版本目录）
std::vector<std::pair<std::string, fs::path>> scan_root_versions(Provider& p,
                                                                 const fs::path& root);
// 扫描注册表记录的全部受管根目录（HKCU\Software\EnvironManage\<id>\roots）；
// 平铺语言同时返回根目录本体（flat_root=true，版本经 --version 捕获）与历史版本子目录
std::vector<ManagedInstall> managed_installs(Provider& p);
// 卸载指定版本：多版本删除版本目录并维护 junction；平铺删除整个根目录；
// PATH/环境变量与受管记录一并清理
bool uninstall_version(Provider& p, const fs::path& root, const std::string& version,
                       std::string& err);

// 版本名自然比较（数字段按数值比较）：支持 "3.12.6" / "v22.14.0" / "0.13.0" / "3.3.5-1"
int natural_cmp(const std::string& a, const std::string& b);

// 支持级别 → 终端颜色（FTXUI 之外的批处理输出也使用）
inline const char* support_ansi(SupportLevel l) {
    switch (l) {
        case SupportLevel::Lts: return "\x1b[1;32m";       // 绿
        case SupportLevel::Sts: return "\x1b[1;34m";       // 蓝
        case SupportLevel::Supported: return "\x1b[1;32m"; // 绿
        case SupportLevel::Stable: return "\x1b[1;34m";    // 蓝
        case SupportLevel::Current: return "\x1b[1;36m";   // 青
        case SupportLevel::Eol: return "\x1b[2m";          // 灰
        case SupportLevel::Preview: return "\x1b[1;33m";   // 黄
        default: return "";
    }
}

} // namespace prov

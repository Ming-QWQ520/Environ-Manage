// RustInstall.cpp : Rust 环境下载安装器
//
//   交互模式  全屏 TUI：页眉状态栏 + 步骤面板 + 底部键位栏，Esc/退格返回上一步
//   脚本模式  命令行参数驱动（-p/-v/-t/...），行为完全非交互
//   版本检索  GitHub Releases API（rust-lang/rust），直连失败自动经 gh-proxy 镜像加速
//   文件下载  static.rust-lang.org 官方源 + 中科大/上交镜像，自动切换、断点续传
//   完整安装  下载 → SHA-256 校验 → 解压到指定目录 → 验证 rustc/cargo → 可选加入 PATH
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <mutex>
#include <thread>

#include "console_ui.hpp"
#include "github.hpp"
#include "logger.hpp"
#include "python.hpp"
#include "providers/archive.hpp"
#include "providers/checksum.hpp" // checksum::g_hash_progress（哈希计算进度回调）
#include "providers/registry.hpp"
#include "providers/node_tools.hpp"
#include "platform/platform.hpp"
#include "http.hpp"
#include "rust_dist.hpp"
#include "sha256.hpp"
#include "strutil.hpp"
#include "toml_lite.hpp"

#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

// --------------------------------------------------------------- 安装进度钩子

// 工作线程进度钩子守卫：线程退出（含任意失败/取消提前 return）时自动清理全局回调，
// 避免悬空引用与下一次安装残留旧状态
struct WorkHookGuard {
    ~WorkHookGuard() {
        archive::clear_hook();
        prov::clear_stage_hook();
        checksum::clear_hash_progress();
    }
};

// 解压进度 TUI 回调（捕获 mutable 节流状态，std::function 存储副本内持久）
// phase: 0=解压中（进度条+速度） 1=合并中（提示） 2=完成（定格终值）
template <typename Self>
inline auto make_extract_hook(Self self) {
    return [self, x_draw = std::chrono::steady_clock::now(),
            x_bps = 0.0, x_bytes = (uint64_t)0](int phase, uint64_t done, uint64_t total,
                                                double bps) mutable {
        if (phase == 1) {
            std::lock_guard<std::mutex> lk(self->wmu);
            self->w_note = "整理文件结构…";
            if (!self->exiting) self->screen.PostEvent(ftxui::Event::Custom);
            return;
        }
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - x_draw).count();
        if (phase == 0) {
            if (dt < 0.15 && !(total > 0 && done >= total)) return;
            double inst = dt > 0 ? (double)(done - x_bytes) / dt : 0;
            x_bps = x_bps == 0 ? inst : x_bps * 0.7 + inst * 0.3;
            x_bytes = done;
            x_draw = now;
            bps = x_bps;
        }
        {
            std::lock_guard<std::mutex> lk(self->wmu);
            self->w_done = done;
            self->w_total = total;
            if (phase == 2) self->w_note.clear();
            self->w_bar = ui::progress_line(done, total, bps);
        }
        if (!self->exiting) self->screen.PostEvent(ftxui::Event::Custom);
    };
}

// 哈希计算进度 TUI 回调（校验阶段进度条：done/total 为已读/总字节，速率单位与下载一致）
template <typename Self>
inline auto make_hash_progress(Self self) {
    return [self, h_draw = std::chrono::steady_clock::now(),
            h_bytes = (uint64_t)0](uint64_t done, uint64_t total) mutable {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - h_draw).count();
        if (dt < 0.15 && !(total > 0 && done >= total)) return;
        double bps = dt > 0 ? (double)(done - h_bytes) / dt : 0;
        h_draw = now;
        h_bytes = done;
        {
            std::lock_guard<std::mutex> lk(self->wmu);
            self->w_done = done;
            self->w_total = total;
            self->w_bar = ui::progress_line(done, total, bps);
        }
        if (!self->exiting) self->screen.PostEvent(ftxui::Event::Custom);
    };
}

// 安装管线阶段通知 → 步骤条推进（经 w_stage_step 映射；未映射的阶段忽略）
template <typename Self>
inline auto make_stage_hook(Self self) {
    return [self](int stage) {
        if (stage < 0 || stage > 5) return;
        int target = -1;
        {
            std::lock_guard<std::mutex> lk(self->wmu);
            if (stage < (int)self->w_stage_step.size()) target = self->w_stage_step[stage];
            if (target >= 0) self->w_step = target;
            if (stage == 1)
                self->w_note = "正在计算哈希校验值…"; // 校验阶段（进度由哈希钩子驱动）
            else if (stage == 2)
                self->w_note.clear(); // 进入解压，由解压钩子接管进度条
        }
        if (target >= 0 && !self->exiting) self->screen.PostEvent(ftxui::Event::Custom);
    };
}

// --------------------------------------------------------------- 杂项

static std::string sfmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static void print_usage() {
    printf(
        "用法: RustInstall [选项]\n"
        "  无参数启动全屏 TUI 交互界面（↑↓/W S 选择，←→/A D 翻页，Enter 确认，\n"
        "  Esc/退格 返回上一步）\n"
        "\n"
        "  -p, --path <目录>       下载/安装目录（默认: 当前目录\\Rust）\n"
        "  -v, --version <版本>    latest 或具体版本号（如 1.99.0）\n"
        "  -t, --target <三元组>   目标平台（默认: 当前架构，如 x86_64-pc-windows-msvc）\n"
        "      --package <包名>    组件包，默认 rust=完整工具链（可选 cargo/rustc/rust-std 等）\n"
        "  -f, --format <格式>     tar.gz（解压即用，默认）或 msi（Windows 安装器）\n"
        "  -m, --mirror <源>       auto / official / ustc / sjtu（默认 auto 自动切换）\n"
        "      --list              仅列出全部可用版本后退出\n"
        "      --no-install        仅下载，不解压安装\n"
        "      --keep-archive      安装后保留压缩包\n"
        "      --no-path-setup     不询问、不修改用户 PATH\n"
        "      --log <文件>        日志输出位置（默认: <exe 目录>\\log\\RustInstall.log）\n"
        "  -y, --yes               所有询问采用默认值（检测到新版本时自动更新/重装到原目录，"
        "保留压缩包、加入 PATH）\n"
        "\n"
        "Python 管理:\n"
        "      --python            管理 Python 而不是 Rust\n"
        "      --py-list           仅列出 Python 版本后退出\n"
        "      --py-version <版本> 指定 Python 版本（默认 latest）\n"
        "      --py-kind <类型>    installer（默认）/ embed / all\n"
        "      --py-arch <架构>    amd64（默认自动）/ arm64 / x86\n"
        "      --py-install        下载后静默安装（exe）/ 解压（zip）到 -p 目录\n"
        "      --py-token <Token>  可选：python.org API Token（元数据增强，匿名 API 已限流）\n"
        "\n"
        "SDK 管理:\n"
        "      --sdk <id>          node / jdk / go / dotnet / zig / php / ruby / git / flutter\n"
        "      --sdk-list          仅列出该 SDK 的版本后退出\n"
        "      --sdk-version <版本> 指定版本（默认最新；jdk 为大版本，dotnet 为通道）\n"
        "      布局: node/jdk 多版本 <目录>\\<版本> + current junction；\n"
        "            go/dotnet/zig/php/ruby/git/flutter 单版本平铺（直接安装于 <目录>）\n"
        "\n"
        "卸载（批处理模式）:\n"
        "      --uninstall [版本|all]\n"
        "                          卸载受管安装（配合 --sdk <id> / --python；Rust 直接\n"
        "                          --uninstall all）\n"
        "\n"
        "Node.js 工具链（--sdk node，优先级: npm/npx 随装自带 → Corepack 开关 →\n"
        "pnpm/yarn 经 Corepack 管理，不单独下载二进制）:\n"
        "      --with-pnpm         安装后经 Corepack 全局安装 pnpm（默认全局启用）\n"
        "      --with-yarn         安装后经 Corepack 全局安装 yarn\n"
        "      --corepack <开关>   enable（默认，启用 Corepack 管理 pnpm/yarn）/\n"
        "                          disable（禁用，移除 pnpm/yarn shim）\n"
        "      --pnpm-home <目录>  pnpm/npm 存储根目录（默认 <目录>\\pnpm-repository）\n"
        "                          pnpm: global-dir/global-bin-dir/state-dir/cache-dir\n"
        "                                → <根目录>\\global|bin|state|cache\n"
        "                          npm : prefix/cache → <根目录>\\npm-global|npm-cache\n"
        "Flutter 下载镜像站（--sdk flutter，默认记忆上次选择；TUI 确认页 M 键切换）:\n"
        "      --flutter-mirror <站> official（官方）/ tuna（清华）/ ustc（中科大）/\n"
        "                          cn（Flutter 中国社区旧镜像，默认）；亦可填 0-3\n"
        "                          安装后按所选镜像设置 PUB_HOSTED_URL /\n"
        "                          FLUTTER_STORAGE_BASE_URL（选 official 时清除）\n"
        "\n"
        "示例:\n"
        "  RustInstall.exe                            TUI 交互模式\n"
        "  RustInstall.exe -p D:\\Rust -v latest       最新版完整安装到 D:\\Rust（脚本模式）\n"
        "  RustInstall.exe -p D:\\Rust -v 1.85.0 -t x86_64-pc-windows-gnu -m ustc\n"
        "  RustInstall.exe --python -p D:\\Python -py-version 3.13.0 -m huawei --py-install\n"
        "  RustInstall.exe --sdk node -p D:\\Sdk                       Node.js LTS/Current\n"
        "  RustInstall.exe --sdk node -p D:\\Sdk --with-pnpm --pnpm-home D:\\pnpm-repository\n"
        "                                             Node.js + pnpm（存储规范化到指定目录）\n"
        "  RustInstall.exe --sdk jdk  -p D:\\Sdk --sdk-version 21      Temurin JDK 21\n"
        "  RustInstall.exe --sdk zig  -p D:\\Sdk                       Zig 最新稳定版\n"
        "  RustInstall.exe --sdk git  -p D:\\Sdk                       Git For Windows 便携版\n"
        "  RustInstall.exe --sdk flutter -p D:\\Sdk --flutter-mirror tuna\n"
        "                                             Flutter SDK（清华 TUNA 镜像加速）\n");
}

struct Options {
    std::wstring path;
    std::string version; // "" = 交互选择
    std::string target;
    std::string pkg = "rust";
    std::string format = "tar.gz";
    int mirror = -1; // -1 = auto
    bool list = false;
    bool keep = false;
    bool no_install = false;
    bool no_path = false;
    bool yes = false;
    bool path_given = false;
    std::wstring log_path;
    bool log_given = false;
    // Python 管理
    bool python = false;
    bool py_list = false;
    std::string py_version;            // "" = latest
    std::string py_kind = "installer"; // installer | embed | all
    std::string py_arch;               // "" = 自动检测
    bool py_install = false;           // 批处理模式：下载后静默安装/解压
    std::string py_token;              // 可选：python.org API Token（元数据增强）
    std::string mirror_raw;            // -m 原始值（Rust/Python 各自解析）
    // SDK 管理（Node.js / JDK / Go / .NET）
    std::string sdk;                   // node | jdk | go | dotnet
    bool sdk_list = false;
    std::string sdk_version;           // "" = 最新
    // Node.js 工具链：优先级 npm/npx（随装自带，仅检测显示）→ Corepack（内置，启/禁开关）
    // → pnpm/yarn（经 Corepack 管理，不单独下载二进制）
    bool with_pnpm = false;            // node 安装后经 Corepack 全局安装 pnpm
    bool with_yarn = false;            // node 安装后经 Corepack 全局安装 yarn
    std::string corepack_mode;         // "" / enable（默认）| disable
    std::string pnpm_home;             // pnpm/npm 存储根目录（默认 <root>\pnpm-repository）
    std::string flutter_mirror;        // Flutter 下载镜像站（official/tuna/ustc/cn 或 0-3）
    // 卸载（批处理模式）：--uninstall [版本|all]
    bool uninstall = false;
    std::string uninstall_target = "all";
};

static bool valid_triple(const std::string& t) {
    if (t.empty() || t.size() > 64) return false;
    for (char c : t)
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

static int parse_mirror_name(const std::string& s) {
    std::string m = su::lower(s);
    if (m == "auto") return -1;
    if (m == "official") return 0;
    if (m == "ustc") return 1;
    if (m == "sjtu") return 2;
    return -2; // 无效
}

// Flutter 下载镜像站取值：名称或下标；-1 = 无法识别（0=官方 1=TUNA 2=USTC 3=社区旧镜像）
static int parse_flutter_mirror(const std::string& s) {
    std::string k = su::lower(s);
    if (k == "official" || k == "官方") return 0;
    if (k == "tuna" || k == "清华") return 1;
    if (k == "ustc" || k == "中科大") return 2;
    if (k == "cn" || k == "社区" || k == "flutter-io.cn") return 3;
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '3') return k[0] - '0';
    return -1;
}

static Options parse_args(int argc, wchar_t** argv, bool& ok) {
    Options o;
    ok = true;
    auto need_value = [&](int& i, const char* name) -> wchar_t* {
        if (i + 1 >= argc) {
            printf("%s: 缺少参数值\n", name);
            ok = false;
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = su::lower(su::wide_to_utf8(argv[i]));
        if (a == "-p" || a == "--path") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.path = v;
            o.path_given = true;
        } else if (a == "-v" || a == "--version") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.version = su::wide_to_utf8(v);
        } else if (a == "-t" || a == "--target") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.target = su::wide_to_utf8(v);
        } else if (a == "--package") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.pkg = su::wide_to_utf8(v);
        } else if (a == "-f" || a == "--format") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.format = su::lower(su::wide_to_utf8(v));
        } else if (a == "-m" || a == "--mirror") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.mirror_raw = su::wide_to_utf8(v);
            int m = parse_mirror_name(o.mirror_raw);
            if (m != -2) {
                o.mirror = m;
            } else if (py::parse_mirror_name(o.mirror_raw) == -2) {
                printf("--mirror 无效: %s（Rust: auto/official/ustc/sjtu；Python: "
                       "auto/official/huawei/npmmirror）\n",
                       o.mirror_raw.c_str());
                ok = false;
                return o;
            }
        } else if (a == "--python") {
            o.python = true;
        } else if (a == "--py-list") {
            o.py_list = true;
        } else if (a == "--py-version") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.py_version = su::wide_to_utf8(v);
        } else if (a == "--py-kind") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.py_kind = su::lower(su::wide_to_utf8(v));
            if (o.py_kind != "installer" && o.py_kind != "embed" && o.py_kind != "all") {
                printf("--py-kind 仅支持 installer/embed/all\n");
                ok = false;
                return o;
            }
        } else if (a == "--py-arch") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.py_arch = su::lower(su::wide_to_utf8(v));
            if (o.py_arch != "amd64" && o.py_arch != "arm64" && o.py_arch != "x86") {
                printf("--py-arch 仅支持 amd64/arm64/x86\n");
                ok = false;
                return o;
            }
        } else if (a == "--py-install") {
            o.py_install = true;
        } else if (a == "--py-token") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.py_token = su::wide_to_utf8(v);
        } else if (a == "--sdk") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.sdk = su::lower(su::wide_to_utf8(v));
            if (!prov::Registry::instance().create(o.sdk)) {
                printf("--sdk 无效: %s（可选 node/jdk/go/dotnet/zig/php/ruby/git/flutter）\n",
                       o.sdk.c_str());
                ok = false;
                return o;
            }
        } else if (a == "--sdk-list") {
            o.sdk_list = true;
        } else if (a == "--sdk-version") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.sdk_version = su::wide_to_utf8(v);
        } else if (a == "--with-pnpm") {
            o.with_pnpm = true;
        } else if (a == "--with-yarn") {
            o.with_yarn = true;
        } else if (a == "--corepack") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.corepack_mode = su::lower(su::wide_to_utf8(v));
            if (o.corepack_mode != "enable" && o.corepack_mode != "disable") {
                printf("--corepack 仅支持 enable/disable\n");
                ok = false;
                return o;
            }
        } else if (a == "--pnpm-home") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.pnpm_home = su::wide_to_utf8(v);
        } else if (a == "--flutter-mirror") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.flutter_mirror = su::wide_to_utf8(v);
        } else if (a == "--uninstall") {
            o.uninstall = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-')
                o.uninstall_target = su::wide_to_utf8(argv[++i]);
        } else if (a == "--list") {
            o.list = true;
        } else if (a == "--no-install") {
            o.no_install = true;
        } else if (a == "--keep-archive") {
            o.keep = true;
        } else if (a == "--no-path-setup") {
            o.no_path = true;
        } else if (a == "--log") {
            wchar_t* v = need_value(i, a.c_str());
            if (!v) return o;
            o.log_path = v;
            o.log_given = true;
        } else if (a == "-y" || a == "--yes") {
            o.yes = true;
        } else if (a == "-h" || a == "--help" || a == "/?") {
            ok = false; // 仅显示用法
            return o;
        } else {
            printf("未知参数: %s\n", su::wide_to_utf8(argv[i]).c_str());
            ok = false;
            return o;
        }
    }
    if (o.format != "tar.gz" && o.format != "tgz" && o.format != "msi") {
        printf("--format 仅支持 tar.gz 或 msi\n");
        ok = false;
    }
    if (o.format == "tgz") o.format = "tar.gz";
    if (!o.target.empty() && !valid_triple(o.target)) {
        printf("--target 无效: %s\n", o.target.c_str());
        ok = false;
    }
    return o;
}

// 当前主机对应的默认目标三元组
static std::string detect_triple() {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    if (HMODULE k = GetModuleHandleW(L"kernel32.dll")) {
        auto fn = (IsWow64Process2Fn)(void*)GetProcAddress(k, "IsWow64Process2");
        USHORT proc = 0, machine = 0;
        if (fn && fn(GetCurrentProcess(), &proc, &machine)) {
            if (machine == IMAGE_FILE_MACHINE_ARM64 || proc == IMAGE_FILE_MACHINE_ARM64)
                return "aarch64-pc-windows-msvc";
        }
    }
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) return "aarch64-pc-windows-msvc";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) return "i686-pc-windows-msvc";
    return "x86_64-pc-windows-msvc";
}

static bool target_is_windows(const std::string& t) {
    return su::lower(t).find("windows") != std::string::npos;
}

// --------------------------------------------------------------- 版本列表

static constexpr size_t kFetchPerPage = 50;
static constexpr size_t kShowPerPage = 15;

struct ReleasePages {
    std::vector<github::Release> all;
    bool end_of_list = false;

    // 确保已加载至少 count 个版本
    bool ensure(size_t count, std::string& err) {
        while (all.size() < count && !end_of_list) {
            std::vector<github::Release> v;
            if (!github::fetch_releases((int)(all.size() / kFetchPerPage) + 1, (int)kFetchPerPage, v,
                                        err))
                return false;
            if (v.size() < kFetchPerPage) end_of_list = true;
            for (auto& r : v) all.push_back(std::move(r));
        }
        return true;
    }
};

static void print_banner() {
    printf("%s"
           "==================================================================\n"
           "   Environ Manage（环境管理器）               By:Ming-QWQ520(明)\n"
           "   版本来源  github.com/rust-lang/rust  (Releases API)\n"
           "   下载来源  static.rust-lang.org 官方源 + 国内镜像自动加速\n"
           "==================================================================\n"
           "%s\n",
           ui::kCyan, ui::kReset);
}

// 检测本机是否已安装 Rust：在 PATH 与常见安装位置查找 rustc.exe 并取其版本
static fs::path find_installed_rust(std::string& version_line) {
    auto probe = [&](const fs::path& exe) -> fs::path {
        std::error_code ec;
        if (!fs::is_regular_file(exe, ec)) return {};
        std::string v = dist::run_capture_first_line(exe, L"--version", 5000);
        if (su::starts_with(v, "rustc")) {
            version_line = v;
            return exe;
        }
        return {};
    };
    // 1) PATH 中可直接调用的 rustc
    wchar_t pathv[32768] = {};
    GetEnvironmentVariableW(L"PATH", pathv, 32768);
    std::wstring dirs(pathv);
    size_t pos = 0;
    while (pos <= dirs.size()) {
        size_t next = dirs.find(L';', pos);
        std::wstring d = su::trim(dirs.substr(
            pos, next == std::wstring::npos ? std::wstring::npos : next - pos));
        if (!d.empty()) {
            fs::path p = probe(fs::path(d) / L"rustc.exe");
            if (!p.empty()) return p;
        }
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    // 2) rustup 默认安装位置 %USERPROFILE%\.cargo\bin
    wchar_t up[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH * 2)) {
        fs::path p = probe(fs::path(up) / L".cargo" / L"bin" / L"rustc.exe");
        if (!p.empty()) return p;
    }
    return {};
}

// 语义化版本解析：从 "rustc 1.99.0 (b940084d7 2026-09-28)" 或 "1.99.0" 等文本中
// 提取第一处 N.N[.N] 数字段；解析失败 ok=false
struct SemVer {
    int maj = 0, min = 0, pat = 0;
    bool ok = false;
    std::string text;
};

static SemVer parse_semver(const std::string& line) {
    SemVer v;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && !isdigit((unsigned char)line[i])) ++i;
        size_t b = i;
        while (i < line.size() && (isdigit((unsigned char)line[i]) || line[i] == '.')) ++i;
        if (b == i) break;
        std::string tok = line.substr(b, i - b);
        int part[3] = {0, 0, 0};
        int n = 0;
        size_t p = 0;
        for (; n < 3; ++n) {
            int val = 0;
            bool any = false;
            while (p < tok.size() && isdigit((unsigned char)tok[p])) {
                val = val * 10 + (tok[p] - '0');
                ++p;
                any = true;
            }
            if (!any) break;
            part[n] = val;
            if (p < tok.size() && tok[p] == '.') ++p;
            else break;
        }
        if (n >= 2 && p == tok.size()) {
            v.maj = part[0];
            v.min = part[1];
            v.pat = part[2];
            v.text = tok;
            v.ok = true;
            return v;
        }
    }
    return v;
}

static int semver_cmp(const SemVer& a, const SemVer& b) {
    if (a.maj != b.maj) return a.maj < b.maj ? -1 : 1;
    if (a.min != b.min) return a.min < b.min ? -1 : 1;
    if (a.pat != b.pat) return a.pat < b.pat ? -1 : 1;
    return 0;
}

static std::string mirror_mode_desc(int m) {
    if (m < 0) return "自动（中科大 → 官方 → 上交，失败自动切换）";
    return dist::mirror_desc(m);
}

// 默认日志文件：<exe 所在目录>\log\RustInstall.log
static fs::path exe_log_file() {
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    fs::path logdir = fs::path(exe).parent_path() / L"log";
    std::error_code ec;
    fs::create_directories(logdir, ec);
    return logdir / L"RustInstall.log";
}

static bool input_is_console() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD m = 0;
    return h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &m) != 0;
}

static bool interactive() {
    return input_is_console();
}

// 展开环境变量并取绝对路径
static fs::path resolve_dir(const std::wstring& raw) {
    wchar_t buf[1024] = {};
    ExpandEnvironmentStringsW(raw.c_str(), buf, 1024);
    return fs::absolute(fs::path(su::trim(std::wstring(buf))));
}

// --------------------------------------------------------------- --list

static int run_list() {
    printf("正在从 GitHub 获取版本列表…\n");
    ReleasePages rp;
    std::string err;
    if (!rp.ensure((size_t)-1, err)) {
        printf("%s获取版本列表失败：%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        printf("提示：GitHub 直连失败时会自动尝试 gh-proxy 镜像；请检查网络后重试。\n");
        return 1;
    }
    printf("  %-16s%s\n", "版本", "发布日期");
    for (size_t i = 0; i < rp.all.size(); ++i) {
        printf("%s%-16s%s%s%s\n", i == 0 ? ui::kGreen : "", rp.all[i].tag.c_str(), ui::kReset,
               rp.all[i].date.c_str(), i == 0 ? "  ← 最新版本" : "");
    }
    printf("\n共 %s 个版本。\n", std::to_string(rp.all.size()).c_str());
    return 0;
}

// --------------------------------------------------------------- 下载+安装（TUI/批处理共用）

struct InstallOutcome {
    bool ok = false;
    bool cancelled = false;
    std::string rustc_v, cargo_v;
    fs::path dest;
};

// 下载 → SHA-256 校验 → 解压安装（msi 仅下载+校验，由调用方决定是否运行安装器）
static bool download_and_install(const Options& opts, const dist::Artifact& art,
                                 const fs::path& dir, int& used_mirror,
                                 const std::function<void(uint64_t, uint64_t)>& progress,
                                 const std::function<bool()>& cancelled, InstallOutcome& out,
                                 std::string& err) {
    std::string filename = art.rel_path;
    size_t slash = filename.rfind('/');
    if (slash != std::string::npos) filename = filename.substr(slash + 1);
    fs::path dest = dir / su::utf8_to_wide(filename);
    out.dest = dest;

    if (!dist::download(art, opts.mirror, dest, progress, used_mirror, err, cancelled)) {
        if (err == "已取消") out.cancelled = true;
        return false;
    }
    logx::linef("使用镜像: %s", dist::mirror_desc(used_mirror).c_str());
    out.ok = true;

    // SHA-256 校验（大文件计算耗时，经全局回调驱动校验阶段进度条）
    prov::stage_notify(1);
    std::string expect = art.sha256;
    if (expect.empty()) expect = dist::fetch_sidecar_sha256(art.rel_path, used_mirror);
    if (!expect.empty()) {
        std::string got = sha256::file_hex_progress(dest.wstring(), checksum::g_hash_progress);
        if (got.empty() || su::lower(got) != su::lower(expect)) {
            logx::linef("SHA-256 校验失败: 期望 %s 实际 %s", expect.c_str(), got.c_str());
            err = "SHA-256 校验失败\n  期望: " + expect + "\n  实际: " +
                  (got.empty() ? "(读取失败)" : got) + "\n  请删除压缩包后重试：" +
                  su::wide_to_utf8(dest.wstring());
            out.ok = false;
            return false;
        }
        logx::linef("SHA-256 校验通过: %s", expect.c_str());
    }
    return true;
}

// tar.gz 解压安装并验证 rustc/cargo；结果写入 out
static bool install_and_verify(const Options& opts, const fs::path& dir, const std::string& pkg,
                               InstallOutcome& out, std::string& err) {
    prov::stage_notify(2); // 解压
    if (!dist::install_tar_gz(out.dest, dir, err)) return false;
    out.rustc_v = dist::run_capture_first_line(dir / L"bin" / L"rustc.exe", L"--version");
    out.cargo_v = dist::run_capture_first_line(dir / L"bin" / L"cargo.exe", L"--version");
    if (!out.rustc_v.empty()) logx::line("验证 rustc: " + out.rustc_v);
    if (!out.cargo_v.empty()) logx::line("验证 cargo: " + out.cargo_v);
    if (out.rustc_v.empty() && out.cargo_v.empty() && pkg == "rust")
        err = "bin 目录下未找到 rustc/cargo，安装可能不完整";
    return out.rustc_v.empty() && out.cargo_v.empty() && pkg == "rust" ? false : true;
}

// --------------------------------------------------------------- 批处理引擎（非交互）

static int run_batch(const Options& opts) {
    print_banner();

    // 检测已安装 Rust（有则显示）
    std::string installed_line;
    fs::path rustc_exe = find_installed_rust(installed_line);
    if (!rustc_exe.empty())
        printf("检测到已安装的 Rust: %s%s%s  %s(%s)%s\n", ui::kGreen, installed_line.c_str(),
               ui::kReset, ui::kDim, su::wide_to_utf8(rustc_exe.wstring()).c_str(), ui::kReset);

    // 更新/重装：仅在 -y 下自动执行（非交互无询问）
    bool preselected = false;
    if (!rustc_exe.empty() && opts.version.empty() && opts.yes) {
        SemVer cur = parse_semver(installed_line);
        ReleasePages rp0;
        std::string err;
        if (cur.ok && rp0.ensure(1, err)) {
            SemVer lat = parse_semver(rp0.all[0].tag);
            if (lat.ok && semver_cmp(cur, lat) <= 0) {
                fs::path root = rustc_exe.parent_path().parent_path();
                printf("%s%s%s\n", ui::kYellow,
                       semver_cmp(cur, lat) < 0
                           ? sfmt("检测到新版本 %s（当前 %s），将更新到 %s",
                                  lat.text.c_str(), cur.text.c_str(),
                                  su::wide_to_utf8(root.wstring()).c_str()).c_str()
                           : sfmt("已安装最新版本 %s，将重装/修复到 %s", cur.text.c_str(),
                                  su::wide_to_utf8(root.wstring()).c_str()).c_str(),
                       ui::kReset);
                preselected = true;
            }
        }
    }

    // 下载/安装目录（版本校验通过后再创建，避免报错退出时留下空目录）
    fs::path dir = opts.path_given ? resolve_dir(opts.path)
                                   : fs::path([] {
                                         wchar_t cwd[MAX_PATH * 2];
                                         GetCurrentDirectoryW(MAX_PATH * 2, cwd);
                                         return std::wstring(cwd);
                                     }()) /
                                         L"Rust";
    std::error_code ec;

    // 版本
    printf("正在从 GitHub 获取版本列表…\n");
    ReleasePages rp;
    std::string err;
    if (!rp.ensure(1, err)) {
        printf("%s获取版本列表失败：%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }
    if (rp.all.empty()) {
        printf("%sGitHub 上没有任何版本。%s\n", ui::kRed, ui::kReset);
        return 1;
    }
    std::string want = opts.version;
    std::string ver, rel_date;
    if (want.empty() && preselected) want = rp.all[0].tag;
    if (want.empty()) {
        printf("%s非交互模式必须指定 --version（latest 或版本号）。%s\n", ui::kRed, ui::kReset);
        printf("示例: RustInstall.exe -p D:\\Rust -v latest\n");
        return 2;
    }
    fs::create_directories(dir, ec);
    if (ec) {
        printf("%s无法创建目录 %s：%s%s\n", ui::kRed, su::wide_to_utf8(dir.wstring()).c_str(),
               ec.message().c_str(), ui::kReset);
        return 1;
    }
    printf("下载/安装目录: %s%s%s\n", ui::kGreen, su::wide_to_utf8(dir.wstring()).c_str(),
           ui::kReset);
    logx::linef("目标目录: %s", su::wide_to_utf8(dir.wstring()).c_str());
    if (su::lower(want) == "latest" || want == "最新") {
        ver = rp.all[0].tag;
        rel_date = rp.all[0].date;
    } else {
        ver = want;
        for (const github::Release& r : rp.all)
            if (r.tag == ver) {
                rel_date = r.date;
                break;
            }
        if (rel_date.empty() && rp.ensure((size_t)-1, err))
            for (const github::Release& r : rp.all)
                if (r.tag == ver) {
                    rel_date = r.date;
                    break;
                }
        if (rel_date.empty()) {
            github::Release r;
            if (github::fetch_release(ver, r, err)) rel_date = r.date;
        }
        if (rel_date.empty())
            printf("%s警告：在 GitHub Releases 中未找到版本 %s（将按输入值继续尝试）%s\n",
                   ui::kYellow, ver.c_str(), ui::kReset);
    }
    printf("已选版本: %s%s%s\n", ui::kGreen, ver.c_str(), ui::kReset);
    logx::linef("已选版本: %s (%s)", ver.c_str(), rel_date.c_str());

    // 目标平台
    std::string target = !opts.target.empty() ? opts.target : detect_triple();
    printf("目标平台: %s%s%s\n", ui::kGreen, target.c_str(), ui::kReset);
    logx::line("目标平台: " + target);

    // 发行清单
    printf("正在获取发行清单 channel-rust-%s.toml…\n", ver.c_str());
    std::string toml;
    int manifest_mirror = -1;
    bool have_manifest = dist::fetch_manifest(ver, opts.mirror, toml, manifest_mirror, err);
    if (have_manifest)
        printf("清单获取成功（%s）。\n", dist::mirror_desc(manifest_mirror).c_str());
    else
        printf("%s无发行清单（%s），将按发布日期推算探测旧版文件。%s\n", ui::kYellow, err.c_str(),
               ui::kReset);

    // 组件包
    std::string pkg = opts.pkg;
    if (have_manifest) {
        toml::PkgTarget t;
        if (!toml::find_pkg_target(toml, pkg, target, t) || !t.available) {
            printf("%s包 %s 在平台 %s 上不可用。可用平台：%s\n", ui::kRed, pkg.c_str(),
                   target.c_str(), ui::kReset);
            for (const std::string& t2 : toml::list_targets(toml, pkg)) printf("  %s\n", t2.c_str());
            return 1;
        }
    } else if (pkg != "rust") {
        printf("%s旧版本仅有 rust 完整包，忽略 --package %s。%s\n", ui::kYellow, pkg.c_str(),
               ui::kReset);
        pkg = "rust";
    }
    printf("组件包: %s%s%s\n", ui::kGreen, pkg.c_str(), ui::kReset);
    logx::line("组件包: " + pkg);

    // 格式
    std::string format = opts.format;
    if (format == "msi" && !target_is_windows(target)) {
        printf("%smsi 仅适用于 Windows 目标平台。%s\n", ui::kRed, ui::kReset);
        return 1;
    }
    printf("包格式: %s%s%s\n", ui::kGreen, format.c_str(), ui::kReset);
    logx::line("包格式: " + format);

    // 解析工件
    dist::Artifact art;
    if (have_manifest && format == "tar.gz") {
        if (!dist::resolve_from_manifest(toml, pkg, target, art, err)) {
            printf("%s%s\n可用平台：%s\n", ui::kRed, err.c_str(), ui::kReset);
            for (const std::string& t : toml::list_targets(toml, pkg)) printf("  %s\n", t.c_str());
            return 1;
        }
    } else {
        std::string date;
        if (have_manifest) {
            dist::Artifact base;
            if (dist::resolve_from_manifest(toml, "rust", target, base, err)) date = base.date;
        }
        if (date.empty() && !rel_date.empty()) date = rel_date;
        if (date.empty()) {
            printf("%s无法确定 %s 的发布日期，无法定位下载文件。%s\n", ui::kRed, ver.c_str(),
                   ui::kReset);
            return 1;
        }
        std::string filename = "rust-" + ver + "-" + target + (format == "msi" ? ".msi" : ".tar.gz");
        if (!dist::resolve_legacy(ver, target, date, filename, art, err)) {
            printf("%s%s%s\n提示：老版本（1.10 之前）平台支持有限，可尝试其他目标三元组；"
                   "msi 包也可改用 tar.gz 格式。\n",
                   ui::kRed, err.c_str(), ui::kReset);
            return 1;
        }
    }
    dist::probe_size(art, manifest_mirror >= 0 ? manifest_mirror : 0);

    // 下载（断点续传 + 镜像自动切换）
    std::string filename = art.rel_path;
    {
        size_t slash = filename.rfind('/');
        if (slash != std::string::npos) filename = filename.substr(slash + 1);
    }
    fs::path dest = dir / su::utf8_to_wide(filename);
    if (fs::exists(dest)) {
        uintmax_t sz = fs::file_size(dest, ec);
        if (!ec && sz > 0)
            printf("%s发现未完成的下载（%s），将断点续传。%s\n", ui::kYellow,
                   su::human_size((uint64_t)sz).c_str(), ui::kReset);
    }
    printf("开始下载: %s\n", filename.c_str());
    if (art.size > 0) printf("文件大小: %s\n", su::human_size(art.size).c_str());

    int used_mirror = -1;
    double bps = 0;
    uint64_t last_bytes = 0;
    auto last_draw = std::chrono::steady_clock::now();
    auto progress_fn = [&](uint64_t done, uint64_t total) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_draw).count();
        if (dt >= 0.15 || (total > 0 && done >= total)) {
            double inst = dt > 0 ? (double)(done - last_bytes) / dt : 0;
            bps = bps == 0 ? inst : bps * 0.7 + inst * 0.3;
            last_bytes = done;
            last_draw = now;
            ui::progress(done, total, bps);
        }
    };
    InstallOutcome outcome;
    if (!download_and_install(opts, art, dir, used_mirror, progress_fn, nullptr, outcome, err)) {
        ui::progress_done(0, false);
        printf("%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }
    ui::progress_done(art.size ? art.size : (uint64_t)fs::file_size(dest), true);
    printf("下载源: %s%s%s\n", ui::kGreen, dist::mirror_desc(used_mirror).c_str(), ui::kReset);
    if (art.sha256.empty() && dist::fetch_sidecar_sha256(art.rel_path, used_mirror).empty())
        printf("%s未找到校验值，已跳过 SHA-256 校验。%s\n", ui::kYellow, ui::kReset);

    // 仅下载模式 / 跨平台包
    if (opts.no_install || !target_is_windows(target)) {
        if (!opts.no_install && !target_is_windows(target))
            printf("%s跨平台下载（%s）：已保存压缩包，请在目标平台上解压使用。%s\n", ui::kYellow,
                   target.c_str(), ui::kReset);
        printf("\n%s√ 下载完成：%s%s\n", ui::kGreen, ui::kReset,
               su::wide_to_utf8(dest.wstring()).c_str());
        return 0;
    }

    // 安装
    if (format == "msi") {
        printf("MSI 安装包已就绪（脚本模式不自动运行安装器）：\n  msiexec /i \"%s\"\n",
               su::wide_to_utf8(dest.wstring()).c_str());
        printf("\n%s√ 下载完成：%s%s\n", ui::kGreen, ui::kReset,
               su::wide_to_utf8(dest.wstring()).c_str());
        return 0;
    }

    printf("正在解压安装到 %s …\n", su::wide_to_utf8(dir.wstring()).c_str());
    // 解压可视化：\r 进度行（总进度 + 速度），完成打印汇总行
    ::archive::set_hook([](int phase, uint64_t done, uint64_t total, double bps) {
        if (phase == 0) {
            ui::progress(done, total, bps);
        } else if (phase == 2) {
            printf("\r  %s[解压完成]%s %s  平均 %s/s    \n", ui::kGreen, ui::kReset,
                   su::human_size(done).c_str(),
                   su::human_size((uint64_t)bps).c_str());
        }
    });
    bool rust_installed = install_and_verify(opts, dir, pkg, outcome, err);
    ::archive::clear_hook();
    if (!rust_installed) {
        printf("\n%s安装失败：%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }
    printf("%s√ 解压安装完成%s\n\n", ui::kGreen, ui::kReset);
    platform::managed_add_root("rust", dir); // 记录受管安装（供检测/卸载）
    if (!outcome.rustc_v.empty())
        printf("  rustc : %s%s%s\n", ui::kGreen, outcome.rustc_v.c_str(), ui::kReset);
    if (!outcome.cargo_v.empty())
        printf("  cargo : %s%s%s\n", ui::kGreen, outcome.cargo_v.c_str(), ui::kReset);

    // PATH（仅 -y 时自动）
    fs::path bin = dir / L"bin";
    bool path_added = false;
    if (!opts.no_path && opts.yes && fs::exists(bin)) {
        std::string perr;
        if (dist::add_to_user_path(bin, perr)) {
            path_added = true;
            printf("%s√ 已将 %s 加入用户 PATH（重新打开终端后生效）%s\n", ui::kGreen,
                   su::wide_to_utf8(bin.wstring()).c_str(), ui::kReset);
        } else {
            printf("%s%s%s\n", ui::kYellow, perr.c_str(), ui::kReset);
        }
    }

    printf("\n%s================ 安装汇总 ================%s\n", ui::kCyan, ui::kReset);
    printf("  版本    : %s (%s)\n", ver.c_str(), (!art.date.empty() ? art.date : rel_date).c_str());
    printf("  平台    : %s\n", target.c_str());
    printf("  组件    : %s\n", pkg.c_str());
    printf("  安装目录: %s\n", su::wide_to_utf8(dir.wstring()).c_str());
    if (!outcome.rustc_v.empty()) printf("  %s\n", outcome.rustc_v.c_str());
    if (!outcome.cargo_v.empty()) printf("  %s\n", outcome.cargo_v.c_str());
    if (!path_added && fs::exists(bin))
        printf("  提示    : 使用前请将 %s 加入 PATH\n", su::wide_to_utf8(bin.wstring()).c_str());
    printf("%s==========================================%s\n", ui::kCyan, ui::kReset);
    return 0;
}

// --------------------------------------------------------------- TUI 引擎


// --------------------------------------------------------------- Python 管理

static std::string py_mirror_mode_desc(int m) {
    if (m < 0) return "自动（华为云 → 官方 → npmmirror，失败自动切换）";
    return py::mirror_desc(m);
}

static int run_py_list() {
    printf("正在枚举 Python 版本（FTP 目录）…\n");
    std::vector<py::PyVersion> vers;
    std::string err;
    if (!py::fetch_versions_ftp(vers, err)) {
        printf("%sPython 版本枚举失败：%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }
    printf("  %-14s%s\n", "版本", "目录日期(≈发布日期)");
    for (size_t i = 0; i < vers.size(); ++i) {
        printf("%s%-14s%s%s%s\n", i == 0 ? ui::kGreen : "", vers[i].version.c_str(),
               ui::kReset, vers[i].date.c_str(), i == 0 ? "  ← 最新版本" : "");
    }
    printf("\n共 %d 个版本。\n", (int)vers.size());
    return 0;
}

static int run_py_batch(const Options& opts) {
    printf("正在枚举 Python 版本（FTP 目录）…\n");
    std::vector<py::PyVersion> vers;
    std::string err;
    if (!py::fetch_versions_ftp(vers, err)) {
        printf("%sPython 版本枚举失败：%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }

    // 版本：显式指定或取最新（列表已按日期降序）
    std::string ver, date;
    if (opts.py_version.empty() || su::lower(opts.py_version) == "latest") {
        ver = vers[0].version;
        date = vers[0].date;
    } else {
        ver = opts.py_version;
        bool found = false;
        for (const py::PyVersion& v : vers)
            if (v.version == ver) {
                date = v.date;
                found = true;
                break;
            }
        if (!found) {
            printf("%sFTP 目录中未找到版本 %s%s\n", ui::kRed, ver.c_str(), ui::kReset);
            return 1;
        }
    }
    logx::line("目标版本: " + ver);

    // 文件（FTP 枚举 + 可选 API 元数据增强）
    std::vector<py::PyFile> files;
    if (!py::fetch_files_ftp(ver, date, files, err)) {
        printf("%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }
    if (!opts.py_token.empty()) {
        std::map<std::string, py::PyApiMeta> api;
        std::string aerr;
        if (py::load_api_metadata(opts.py_token, api, aerr)) {
            for (py::PyFile& f : files) {
                auto it = api.find(f.version + "/" + f.filename);
                if (it != api.end()) {
                    f.sha256 = it->second.sha256;
                    f.md5 = it->second.md5;
                    f.filesize = it->second.filesize;
                    if (!it->second.date.empty()) f.release_date = it->second.date;
                    f.from_api = true;
                }
            }
            logx::line("API 元数据增强完成");
        } else {
            printf("%sAPI 元数据不可用（%s），继续使用 FTP 数据%s\n", ui::kYellow, aerr.c_str(),
                   ui::kReset);
        }
    }

    int mirror_hint = py::parse_mirror_name(opts.mirror_raw);
    std::string arch = !opts.py_arch.empty() ? opts.py_arch : py::detect_host_arch();
    py::PyKind kind = opts.py_kind == "embed"   ? py::PyKind::Embed
                      : opts.py_kind == "all" ? py::PyKind::All
                                              : py::PyKind::Installer;
    std::vector<const py::PyFile*> cand = py::filter_files(files, ver, arch, kind);
    if (cand.empty()) {
        std::map<std::string, int> archs;
        for (const py::PyFile& f : files)
            if (py::matches_kind(f, kind)) archs[f.arch]++;
        printf("%s版本 %s 没有（架构 %s / 类型 %s）的文件。可用架构：%s\n", ui::kRed, ver.c_str(),
               arch.c_str(), opts.py_kind.c_str(), ui::kReset);
        for (auto& a : archs) printf("  %s（%d 个）\n", a.first.c_str(), a.second);
        return 1;
    }
    const py::PyFile& f = *cand[0];

    // 目录
    wchar_t cwd[MAX_PATH * 2];
    GetCurrentDirectoryW(MAX_PATH * 2, cwd);
    fs::path dir = opts.path_given ? resolve_dir(opts.path) : fs::path(cwd) / L"Python";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        printf("%s无法创建目录 %s：%s%s\n", ui::kRed, su::wide_to_utf8(dir.wstring()).c_str(),
               ec.message().c_str(), ui::kReset);
        return 1;
    }
    logx::linef("Python 目标版本: %s (%s)", ver.c_str(), f.release_date.c_str());
    logx::linef("文件: %s（%s，%s）", f.filename.c_str(),
                su::human_size(f.filesize).c_str(), f.arch.c_str());
    logx::linef("目标目录: %s", su::wide_to_utf8(dir.wstring()).c_str());
    printf("目标版本: %s%s%s (%s)\n", ui::kGreen, ver.c_str(), ui::kReset,
           f.release_date.c_str());
    printf("文件    : %s\n", f.filename.c_str());
    printf("大小    : %s\n",
           f.filesize > 0 ? su::human_size(f.filesize).c_str() : "下载时确定");
    printf("校验    : %s\n",
           !f.sha256.empty() ? "SHA-256"
                             : (!f.md5.empty() ? "MD5" : "下载完整性（Content-Length）"));
    printf("镜像    : %s\n", py_mirror_mode_desc(mirror_hint).c_str());
    printf("目录    : %s\n", su::wide_to_utf8(dir.wstring()).c_str());

    // 下载
    fs::path dest = dir / su::utf8_to_wide(f.filename);
    double bps = 0;
    uint64_t last_bytes = 0;
    auto last_draw = std::chrono::steady_clock::now();
    auto progress_fn = [&](uint64_t done, uint64_t total) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_draw).count();
        if (dt >= 0.15 || (total > 0 && done >= total)) {
            double inst = dt > 0 ? (double)(done - last_bytes) / dt : 0;
            bps = bps == 0 ? inst : bps * 0.7 + inst * 0.3;
            last_bytes = done;
            last_draw = now;
            ui::progress(done, total, bps);
        }
    };
    int used = -1;
    if (!py::download(f, mirror_hint, dest, progress_fn, used, err)) {
        ui::progress_done(f.filesize, false);
        printf("%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }
    ui::progress_done(f.filesize, true);
    printf("下载源  : %s\n", py::mirror_desc(used).c_str());

    // 校验
    if (!py::verify(f, dest, err)) {
        printf("%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }
    printf("%s校验通过\n%s", ui::kGreen, ui::kReset);

    // 安装 / 解压 / 保留（统一版本化布局：<目录>\<版本> + <目录>\current junction）
    fs::path ver_dir = dir / su::utf8_to_wide(ver);
    fs::path py_current = dir / L"current";
    std::string l = su::lower(f.filename);
    if (opts.py_install && su::ends_with(l, ".exe")) {
        printf("正在静默安装（per-user，TargetDir=%s）…\n",
               su::wide_to_utf8(ver_dir.wstring()).c_str());
        if (!py::install_exe(dest, ver_dir, false, err)) {
            printf("%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
            return 1;
        }
        std::string vout;
        if (py::verify_install(ver_dir, vout, err))
            printf("%s%s%s\n", ui::kGreen, vout.c_str(), ui::kReset);
    } else if (opts.py_install && su::ends_with(l, ".zip")) {
        printf("正在解压到 %s …\n", su::wide_to_utf8(ver_dir.wstring()).c_str());
        // 解压可视化：\r 进度行（总进度 + 速度），完成打印汇总行
        ::archive::set_hook([](int phase, uint64_t done, uint64_t total, double bps) {
            if (phase == 0) {
                ui::progress(done, total, bps);
            } else if (phase == 2) {
                printf("\r  %s[解压完成]%s %s  平均 %s/s    \n", ui::kGreen, ui::kReset,
                       su::human_size(done).c_str(),
                       su::human_size((uint64_t)bps).c_str());
            }
        });
        bool py_extracted = py::extract_zip(dest, ver_dir, err);
        ::archive::clear_hook();
        if (!py_extracted) {
            printf("\n%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
            return 1;
        }
        std::string vout;
        if (py::verify_install(ver_dir, vout, err))
            printf("%s%s%s\n", ui::kGreen, vout.c_str(), ui::kReset);
    } else if (su::ends_with(l, ".msi")) {
        printf("MSI 包已就绪（脚本模式不自动安装）：\n  msiexec /i \"%s\" TARGETDIR=\"%s\"\n",
               su::wide_to_utf8(dest.wstring()).c_str(),
               su::wide_to_utf8(ver_dir.wstring()).c_str());
    } else {
        printf("已保留安装包（--py-install 可启用自动安装）\n");
    }

    // junction + PATH + 受管记录（检测/卸载的依据）
    bool layout_done = false;
    if (fs::exists(ver_dir / L"python.exe", ec)) {
        if (!platform::make_junction(py_current, ver_dir, err)) {
            printf("%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
            return 1;
        }
        if (!opts.no_path) {
            std::string perr;
            platform::add_path_dir(py_current, perr);
            platform::add_path_dir(py_current / L"Scripts", perr);
        }
        platform::managed_add_root("python", dir);
        layout_done = true;
    }

    printf("\n%s================ 完成汇总 ================%s\n", ui::kCyan, ui::kReset);
    printf("  版本    : Python %s (%s)\n", ver.c_str(), f.release_date.c_str());
    printf("  文件    : %s\n", f.filename.c_str());
    printf("  版本目录: %s\n", su::wide_to_utf8(ver_dir.wstring()).c_str());
    if (layout_done)
        printf("  current : %s（junction）\n", su::wide_to_utf8(py_current.wstring()).c_str());
    printf("%s==========================================%s\n", ui::kCyan, ui::kReset);
    return 0;
}


// --------------------------------------------------------------- SDK 管理

// Node.js 工具链配置（安装后统一入口，批处理与 TUI 共用）。
// 按优先级模型执行：
//   1. npm/npx：随装自带，不做任何动作，仅在末尾的检测报告中显示版本；
//   2. Corepack：Node 内置，按开关执行 enable/disable（选装 pnpm/yarn 时强制启用）；
//   3. pnpm/yarn：经 Corepack 安装激活（不单独下载二进制），并规范化存储位置
//      （pnpm global-dir/global-bin-dir/state-dir/cache-dir + npm prefix/cache）。
// 返回报告行（供完成汇总/完成页展示）；失败项写入报告并记日志，不中断安装结果。
static std::vector<std::string> setup_node_toolchain(const fs::path& node_dir,
                                                     bool corepack_on, bool want_pnpm,
                                                     bool want_yarn,
                                                     const fs::path& storage_base,
                                                     bool add_path,
                                                     std::string& tools_line) {
    std::vector<std::string> report;
    std::string err;
    // 2) Corepack 开关：选装 pnpm/yarn 时强制启用（二者经 Corepack 管理）
    bool force_on = corepack_on || want_pnpm || want_yarn;
    if (!nodetools::set_corepack(node_dir, force_on, err)) {
        report.push_back("  Corepack " + std::string(force_on ? "启用" : "禁用") +
                         "失败: " + err);
        return report;
    }
    // 3) pnpm / yarn：经 Corepack 管理版本，全局可用（不单独下载二进制）
    std::string pver, yver;
    if (want_pnpm) {
        if (!nodetools::install_via_corepack(node_dir, "pnpm", pver, err))
            report.push_back("  pnpm 安装失败: " + err);
    }
    if (want_yarn) {
        if (!nodetools::install_via_corepack(node_dir, "yarn", yver, err))
            report.push_back("  yarn 安装失败: " + err);
    }
    // 存储位置规范化：pnpm 四目录 + npm prefix/cache，设置后逐项回读校验
    if (want_pnpm || want_yarn) {
        std::vector<std::wstring> pdirs;
        if (nodetools::configure_storage(node_dir, storage_base, pdirs, err)) {
            if (add_path)
                for (const fs::path& d : pdirs) platform::add_path_dir(d, err);
            report.push_back("  存储位置: " + su::wide_to_utf8(storage_base.wstring()) +
                             "（pnpm global/global-bin/state/cache + npm prefix/cache）");
            report.push_back("  （以上配置项已逐项回读校验）");
        } else {
            report.push_back("  存储位置配置失败: " + err);
        }
    }
    // 1) 工具链检测：npm/npx 随装自带只检测显示；corepack/pnpm/yarn 显示状态
    report.push_back("  工具链检测:");
    for (const std::string& l :
         nodetools::format_tool_lines(nodetools::detect_tools(node_dir, want_yarn)))
        report.push_back("  " + l);
    if (!pver.empty()) tools_line = "pnpm " + pver + "（经 Corepack 全局）";
    if (!yver.empty())
        tools_line += (tools_line.empty() ? "" : " / ") + std::string("yarn ") + yver +
                      "（经 Corepack 全局）";
    return report;
}

static int run_sdk_list(const Options& opts) {
    auto provider = prov::Registry::instance().create(opts.sdk);
    if (!provider) return 1;
    printf("正在获取 %s 版本列表…\n", provider->display().c_str());
    std::vector<prov::VersionInfo> vers;
    std::string err;
    if (!provider->list_versions(vers, err)) {
        printf("%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }
    for (size_t i = 0; i < vers.size(); ++i) {
        printf("%s%-14s%s%s [%s]%s\n", i == 0 ? ui::kGreen : "", vers[i].id.c_str(),
               ui::kReset, vers[i].date.c_str(), vers[i].tag_label.c_str(),
               i == 0 ? "  ← 最新" : "");
    }
    printf("\n共 %d 个版本。\n", (int)vers.size());
    return 0;
}

static int run_sdk_batch(const Options& opts) {
    auto provider = prov::Registry::instance().create(opts.sdk);
    if (!provider) return 1;
    // Flutter：镜像站选择（--flutter-mirror 覆盖 JSON 记录记忆；不传则用上次选择/默认社区镜像）
    if (provider->id() == "flutter" && !opts.flutter_mirror.empty()) {
        int idx = parse_flutter_mirror(opts.flutter_mirror);
        if (idx < 0) {
            printf("%s--flutter-mirror 取值: official / tuna / ustc / cn（或 0-3）%s\n", ui::kRed,
                   ui::kReset);
            return 1;
        }
        provider->set_mirror_selected(idx);
    }
    printf("正在获取 %s 版本列表…\n", provider->display().c_str());
    std::vector<prov::VersionInfo> vers;
    std::string err;
    if (!provider->list_versions(vers, err)) {
        printf("%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }
    std::string vid = opts.sdk_version;
    if (vid.empty()) vid = vers[0].id;
    prov::VersionInfo vi;
    for (const prov::VersionInfo& v : vers)
        if (v.id == vid) {
            vi = v;
            break;
        }
    if (vi.id.empty()) {
        printf("%s未找到版本 %s%s\n", ui::kRed, vid.c_str(), ui::kReset);
        return 1;
    }
    prov::Artifact f;
    if (!provider->resolve(vid, f, err)) {
        printf("%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }

    wchar_t cwd[MAX_PATH * 2];
    GetCurrentDirectoryW(MAX_PATH * 2, cwd);
    fs::path root = opts.path_given ? resolve_dir(opts.path)
                                    : fs::path(cwd) / su::utf8_to_wide(provider->id());
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        printf("%s无法创建目录 %s：%s%s\n", ui::kRed, su::wide_to_utf8(root.wstring()).c_str(),
               ec.message().c_str(), ui::kReset);
        return 1;
    }
    logx::linef("SDK 目标: %s %s（%s）", provider->display().c_str(), f.version.c_str(),
                vi.tag_label.c_str());
    logx::linef("文件: %s", f.filename.c_str());
    logx::linef("根目录: %s", su::wide_to_utf8(root.wstring()).c_str());
    printf("目标    : %s %s [%s]\n", provider->display().c_str(), f.version.c_str(),
           vi.tag_label.c_str());
    printf("文件    : %s\n", f.filename.c_str());
    if (f.size > 0) printf("大小    : %s\n", su::human_size(f.size).c_str());
    printf("根目录  : %s\n", su::wide_to_utf8(root.wstring()).c_str());

    std::string verify_line;
    double bps = 0;
    uint64_t last_bytes = 0;
    auto last_draw = std::chrono::steady_clock::now();
    auto progress_fn = [&](uint64_t done, uint64_t total) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_draw).count();
        if (dt >= 0.15 || (total > 0 && done >= total)) {
            double inst = dt > 0 ? (double)(done - last_bytes) / dt : 0;
            bps = bps == 0 ? inst : bps * 0.7 + inst * 0.3;
            last_bytes = done;
            last_draw = now;
            ui::progress(done, total, bps);
        }
    };
    // 解压可视化：下载进度行之后接解压进度行（总进度 + 速度），完成打印汇总行；
    // 哈希验证阶段同步显示计算进度（SDK 压缩包可达 GB 级，SHA-256/512 计算需数秒）
    ::archive::set_hook([](int phase, uint64_t done, uint64_t total, double bps) {
        if (phase == 0) {
            ui::progress(done, total, bps);
        } else if (phase == 2) {
            printf("\r  %s[解压完成]%s %s  平均 %s/s    \n", ui::kGreen, ui::kReset,
                   su::human_size(done).c_str(),
                   su::human_size((uint64_t)bps).c_str());
        }
    });
    {
        double hbps = 0;
        uint64_t hlast = 0;
        auto hdraw = std::chrono::steady_clock::now();
        checksum::set_hash_progress([&](uint64_t done, uint64_t total) {
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - hdraw).count();
            if (dt < 0.15 && !(total > 0 && done >= total)) return;
            double inst = dt > 0 ? (double)(done - hlast) / dt : 0;
            hbps = hbps == 0 ? inst : hbps * 0.7 + inst * 0.3;
            hlast = done;
            hdraw = now;
            ui::progress(done, total, hbps);
        });
    }
    bool sdk_installed = prov::install_to_root(*provider, f, root, !opts.no_path, false, true,
                                               progress_fn, nullptr, verify_line, err);
    ::archive::clear_hook();
    checksum::clear_hash_progress();
    if (!sdk_installed) {
        printf("\n");
        ui::progress_done(f.size, false);
        printf("%s%s%s\n", ui::kRed, err.c_str(), ui::kReset);
        return 1;
    }
    ui::progress_done(f.size, true);

    bool multi = provider->multi_version();
    fs::path current = root / L"current";

    // Node.js 工具链（优先级：npm/npx 随装自带 → Corepack 开关 → pnpm/yarn 经 Corepack）
    std::vector<std::string> tool_report;
    if (provider->id() == "node") {
        fs::path nd = root / su::utf8_to_wide(f.version); // 版本目录（node.exe 所在）
        bool want_pnpm = opts.with_pnpm;
        bool want_yarn = opts.with_yarn;
        bool corepack_on = opts.corepack_mode != "disable"; // 默认启用
        if ((want_pnpm || want_yarn) && !corepack_on) {
            corepack_on = true; // pnpm/yarn 依赖 Corepack 管理，选装时强制启用
            logx::line("pnpm/yarn 依赖 Corepack 管理，已自动启用 Corepack");
        }
        fs::path base = opts.pnpm_home.empty()
                            ? root / L"pnpm-repository"
                            : resolve_dir(su::utf8_to_wide(opts.pnpm_home));
        std::string tools_line;
        printf("正在配置 Node.js 工具链（Corepack / pnpm / npm 存储位置）…\n");
        tool_report = setup_node_toolchain(nd, corepack_on, want_pnpm, want_yarn, base,
                                           !opts.no_path, tools_line);
        if (!tools_line.empty())
            logx::line("工具链: " + tools_line);
    }

    // Flutter：按所选镜像站写入 PUB_HOSTED_URL / FLUTTER_STORAGE_BASE_URL（官方源清除）
    if (provider->id() == "flutter") {
        for (const auto& ev : provider->mirror_env_vars()) {
            std::string e2;
            if (ev.second.empty()) {
                platform::remove_user_env(ev.first, e2);
                printf("  镜像环境变量: %s 已清除\n", ev.first.c_str());
            } else if (platform::set_user_env(ev.first, su::utf8_to_wide(ev.second), e2)) {
                printf("  镜像环境变量: %s=%s\n", ev.first.c_str(), ev.second.c_str());
            } else {
                printf("%s  镜像环境变量: %s 设置失败：%s%s\n", ui::kYellow, ev.first.c_str(),
                       e2.c_str(), ui::kReset);
            }
        }
    }

    printf("\n%s================ 完成汇总 ================%s\n", ui::kCyan, ui::kReset);
    printf("  目标    : %s %s [%s]\n", provider->display().c_str(), f.version.c_str(),
           vi.tag_label.c_str());
    if (multi) {
        printf("  版本目录: %s\n",
               su::wide_to_utf8((root / su::utf8_to_wide(f.version)).wstring())
                   .c_str());
        printf("  current : %s（junction）\n", su::wide_to_utf8(current.wstring()).c_str());
    } else {
        printf("  安装目录: %s（单版本平铺）\n", su::wide_to_utf8(root.wstring()).c_str());
    }
    if (!verify_line.empty()) printf("  %s\n", verify_line.c_str());
    if (!tool_report.empty()) {
        for (const std::string& l : tool_report) printf("  %s\n", l.c_str());
    }
    if (!opts.no_path)
        printf("  PATH/环境变量已写入（重新打开终端后生效）\n");
    printf("%s==========================================%s\n", ui::kCyan, ui::kReset);
    return 0;
}

// --------------------------------------------------------------- 卸载（批处理入口 + TUI 共用工具）

// Python 版本卸载：删除版本目录、维护 current junction、清理 PATH 与受管记录
static bool uninstall_python_version(const fs::path& root, const std::string& version,
                                     std::string& err) {
    std::error_code ec;
    fs::path ver_dir = root / su::utf8_to_wide(version);
    if (!fs::is_directory(ver_dir, ec)) {
        err = "未找到版本目录 " + su::wide_to_utf8(ver_dir.wstring());
        return false;
    }
    if (!fs::exists(ver_dir / L"python.exe", ec)) {
        err = "目录不是受本工具管理的 Python 版本目录：" +
              su::wide_to_utf8(ver_dir.wstring());
        return false;
    }
    fs::path junction = root / L"current";
    if (fs::exists(junction, ec)) {
        std::error_code ec2;
        if (fs::equivalent(junction, ver_dir, ec2)) {
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
    // 剩余版本 → junction 重新指向最近使用的版本；无剩余 → PATH/受管记录清理
    std::vector<fs::path> remain;
    for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory()) continue;
        std::string n = su::wide_to_utf8(e.path().filename().wstring());
        if (n == "current" || n == "archives" || (!n.empty() && n[0] == '_')) continue;
        if (fs::exists(e.path() / L"python.exe", ec)) remain.push_back(e.path());
    }
    if (!remain.empty()) {
        fs::path best;
        std::error_code tec;
        fs::file_time_type bt{};
        for (const fs::path& p : remain) {
            fs::file_time_type t = fs::last_write_time(p, tec);
            if (tec || best.empty() || bt < t) {
                best = p;
                bt = t;
            }
        }
        if (!platform::make_junction(junction, best, err)) return false;
        logx::linef("current 已切换到: %s",
                    su::wide_to_utf8(best.filename().wstring()).c_str());
        return true;
    }
    fs::remove(junction, ec);
    std::string perr;
    platform::remove_path_dir(junction, perr);
    platform::remove_path_dir(junction / L"Scripts", perr);
    platform::managed_remove_root("python", root);
    return true;
}

// Rust 安装卸载（平铺布局特例）：删除安装目录 + PATH 清理 + 受管记录移除
static bool uninstall_rust_root(const fs::path& root, std::string& err) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        err = "未找到安装目录 " + su::wide_to_utf8(root.wstring());
        return false;
    }
    logx::linef("正在删除 Rust 安装目录: %s", su::wide_to_utf8(root.wstring()).c_str());
    fs::remove_all(root, ec);
    if (ec) {
        err = "删除安装目录失败：" + ec.message() +
              "（可能有程序正在使用该目录，请关闭后重试）";
        return false;
    }
    std::string perr;
    platform::remove_path_dir(root / L"bin", perr);
    platform::managed_remove_root("rust", root);
    return true;
}

// 批处理卸载入口：--uninstall [版本|all]（配合 --sdk / --python；Rust 直接 --uninstall all）
static int run_uninstall_batch(const Options& opts) {
    print_banner();
    std::string target = opts.uninstall_target;
    bool all = su::lower(target) == "all" || target.empty();
    int failures = 0, done = 0;

    if (opts.python) {
        auto roots = platform::managed_get_roots("python");
        if (roots.empty()) {
            printf("%s未找到本工具管理的 Python 安装记录。%s\n", ui::kYellow, ui::kReset);
            return 1;
        }
        for (const std::wstring& r : roots) {
            fs::path root(r);
            std::vector<std::string> vers;
            std::error_code ec;
            for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
                if (!e.is_directory()) continue;
                std::string n = su::wide_to_utf8(e.path().filename().wstring());
                if (n == "current" || n == "archives" || (!n.empty() && n[0] == '_')) continue;
                if (fs::exists(e.path() / L"python.exe", ec)) vers.push_back(n);
            }
            for (const std::string& v : vers) {
                if (!all && v != target) continue;
                std::string err;
                printf("卸载 Python %s（%s）…\n", v.c_str(),
                       su::wide_to_utf8(root.wstring()).c_str());
                if (uninstall_python_version(root, v, err)) {
                    printf("%s√ 已卸载%s\n", ui::kGreen, ui::kReset);
                    done++;
                } else {
                    printf("%s卸载失败：%s%s\n", ui::kRed, err.c_str(), ui::kReset);
                    failures++;
                }
            }
        }
        if (done == 0 && failures == 0) {
            printf("%s未找到版本 %s（可用 --uninstall all 卸载全部）%s\n", ui::kRed,
                   target.c_str(), ui::kReset);
            return 1;
        }
    } else if (!opts.sdk.empty()) {
        auto provider = prov::Registry::instance().create(opts.sdk);
        if (!provider) return 1;
        auto installs = prov::managed_installs(*provider);
        if (installs.empty()) {
            printf("%s未找到本工具管理的 %s 安装。%s\n", ui::kYellow,
                   provider->display().c_str(), ui::kReset);
            return 1;
        }
        for (const auto& inst : installs) {
            const std::string& ver = inst.version;
            const fs::path& dir = inst.dir;
            if (!all && ver != target) continue;
            // 平铺布局：dir 即安装目录；多版本：dir 为版本子目录，root 为其父目录
            fs::path root = inst.flat_root ? dir : dir.parent_path();
            std::string err;
            printf("卸载 %s %s（%s）…\n", provider->display().c_str(), ver.c_str(),
                   su::wide_to_utf8(dir.wstring()).c_str());
            if (prov::uninstall_version(*provider, root, ver, err)) {
                printf("%s√ 已卸载%s\n", ui::kGreen, ui::kReset);
                done++;
            } else {
                printf("%s卸载失败：%s%s\n", ui::kRed, err.c_str(), ui::kReset);
                failures++;
            }
        }
        if (done == 0 && failures == 0) {
            printf("%s未找到版本 %s（可用 --uninstall all 卸载全部）%s\n", ui::kRed,
                   target.c_str(), ui::kReset);
            return 1;
        }
    } else {
        auto roots = platform::managed_get_roots("rust");
        if (roots.empty()) {
            printf("%s未找到本工具管理的 Rust 安装记录。%s\n", ui::kYellow, ui::kReset);
            return 1;
        }
        for (const std::wstring& r : roots) {
            fs::path root(r);
            std::string err;
            printf("卸载 Rust（%s）…\n", su::wide_to_utf8(root.wstring()).c_str());
            if (uninstall_rust_root(root, err)) {
                printf("%s√ 已卸载%s\n", ui::kGreen, ui::kReset);
                done++;
            } else {
                printf("%s卸载失败：%s%s\n", ui::kRed, err.c_str(), ui::kReset);
                failures++;
            }
        }
    }
    printf("\n卸载完成：%d 成功，%d 失败。\n", done, failures);
    return failures == 0 ? 0 : 1;
}

namespace tui {

using namespace ftxui;

// 屏幕索引（Tab 容器顺序，与 screens 一一对应；Tab 选择器即 Session::step）
enum StepIdx {
    S_UpdateCheck = 0,
    S_RepairLocal,
    S_Path,
    S_Version,
    S_Target,
    S_TargetCustom,
    S_Package,
    S_Format,
    S_Confirm,
    S_Work,
    S_Done,
    S_Choose,      // 选择管理目标（Rust / Python）
    S_PyVersion,   // Python 版本（搜索）
    S_PyFile,      // Python 文件选择
    S_PyConfirm,   // Python 确认
    S_SdkVersion,  // SDK 版本（搜索）
    S_SdkConfirm,  // SDK 确认
    S_Installed,   // 已安装检测（自动检测 + 显示位置 + 更新/卸载入口）
    S_Hub,         // 全部已安装语言管理中心（首页 ←/→ 进入）
    S_COUNT,
};

static const char* step_tag(int st) {
    switch (st) {
        case S_UpdateCheck: return "[检查更新]";
        case S_RepairLocal: return "[修复 · 本地安装包]";
        case S_Path: return "[步骤 1/6 · 安装路径]";
        case S_Version: return "[步骤 2/6 · 选择版本]";
        case S_Target: return "[步骤 3/6 · 目标平台]";
        case S_TargetCustom: return "[步骤 3/6 · 自定义三元组]";
        case S_Package: return "[步骤 4/6 · 组件包]";
        case S_Format: return "[步骤 5/6 · 包格式]";
        case S_Confirm: return "[步骤 6/6 · 确认]";
        case S_Work: return "[执行 · 下载安装]";
        case S_Done: return "[完成]";
        case S_Choose: return "[选择管理目标]";
        case S_PyVersion: return "[Python · 选择版本]";
        case S_PyFile: return "[Python · 选择文件]";
        case S_PyConfirm: return "[Python · 确认]";
        case S_SdkVersion: return "[SDK · 选择版本]";
        case S_SdkConfirm: return "[SDK · 确认]";
        case S_Installed: return "[已安装检测]";
        case S_Hub: return "[管理中心 · 全部已装语言]";
    }
    return "";
}

static const char* step_hints(int st) {
    switch (st) {
        case S_UpdateCheck: return "↑↓/W S 选择 · Enter 确认 · Esc 退出";
        case S_RepairLocal: return "正在从本地安装包修复，无需联网，请稍候";
        case S_Path: return "输入路径 · Enter 确认 · Esc 返回上一步";
        case S_Version: return "直接输入过滤 · ↑↓/W S 选择 · Enter 确认 · Esc 返回上一步";
        case S_Target: return "↑↓/W S 选择 · Enter 确认 · Esc 返回上一步";
        case S_TargetCustom: return "输入三元组 · Enter 确认 · Esc 返回上一步";
        case S_Package: return "↑↓/W S 选择 · Enter 确认 · Esc 返回上一步";
        case S_Format: return "↑↓/W S 选择 · Enter 确认 · Esc 返回上一步";
        case S_Confirm: return "Enter 开始 · M 镜像 · P 加PATH · K 留压缩包 · Esc 返回";
        case S_Work: return "下载中 Esc 取消（保留断点）";
        case S_Done: return "Enter 退出 · Esc 返回主界面";
        case S_Choose: return "↑↓/W S 选择 · ←/→ 已装语言管理 · Enter 确认 · Esc 退出";
        case S_PyVersion: return "直接输入过滤 · ↑↓/W S 选择 · Enter 确认 · Esc 返回上一步";
        case S_PyFile: return "↑↓/W S 选择 · Enter 确认 · Esc 返回上一步";
        case S_PyConfirm: return "Enter 开始 · I 自动安装 · P 加PATH · K 留包 · Esc 返回";
        case S_SdkVersion: return "直接输入过滤 · ↑↓/W S 选择 · Enter 确认 · Esc 返回上一步";
        case S_SdkConfirm: return "Enter 开始 · M 镜像(Flutter) · P 加PATH · C Corepack · N pnpm · Y yarn · Esc 返回";
        case S_Installed:
            return "↑↓/W S 选择 · Enter 确认 · O 打开目录 · U 重新检测 · Esc 返回";
        case S_Hub:
            return "↑↓/W S 选择 · Enter 管理 · P 体检 PATH · C 清理缓存 · U 重新检测 · ←/→ 返回";
    }
    return "";
}

// 一次 TUI 会话的全部状态
struct Session {
    const Options& opt;
    ReleasePages rp;
    std::string last_err;

    // 本机检测
    bool rust_ok = false;
    std::string installed_line;
    fs::path rustc_exe;

    // 选择状态
    bool dir_set = false;
    fs::path dir;
    std::string path_input;  // 路径输入框内容
    std::string path_input2; // 自定义三元组输入框内容
    bool ver_set = false;
    std::string ver, ver_date;
    bool target_set = false;
    std::string target;
    std::string pkg;
    bool format_set = false;
    std::string format;
    int mirror = -1;

    // 清单缓存（版本/平台变化后失效）
    bool manifest_tried = false;
    bool have_manifest = false;
    std::string toml;
    int manifest_mirror = -1;

    // 修复模式：优先使用已下载的安装包
    bool repair = false;
    fs::path local_archive;

    // 安装结果
    std::string rustc_v, cargo_v;
    bool path_added = false;

    bool pkg_locked() const { return opt.pkg != "rust"; }
    void invalidate_manifest() {
        manifest_tried = false;
        have_manifest = false;
        pkg = opt.pkg;
    }
    explicit Session(const Options& o)
        : opt(o), pkg(o.pkg), format(o.format), mirror(o.mirror) {}
};

inline std::string w(const fs::path& p) { return su::wide_to_utf8(p.wstring()); }

// 获取发行清单（幂等，版本/平台变化后由 invalidate 触发重取）
inline void ensure_manifest(Session& s) {
    if (s.manifest_tried) return;
    s.manifest_tried = true;
    s.have_manifest = dist::fetch_manifest(s.ver, s.mirror, s.toml, s.manifest_mirror, s.last_err);
}

// 从已安装目录推断目标三元组（lib/rustlib/<三元组>），失败退回当前架构默认值
inline std::string installed_triple(const fs::path& dir) {
    std::error_code ec;
    fs::path rl = dir / L"lib" / L"rustlib";
    if (fs::is_directory(rl, ec)) {
        for (const fs::directory_entry& e : fs::directory_iterator(rl)) {
            std::string n = su::wide_to_utf8(e.path().filename().wstring());
            if (e.is_directory() && n != "etc" && n.find('-') != std::string::npos) return n;
        }
    }
    return detect_triple();
}

// 在安装目录查找本地已下载的完整工具链压缩包 rust-<版本>-<三元组>.tar.gz
inline fs::path find_local_archive(const fs::path& dir, const std::string& ver) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    std::string want = "rust-" + ver + "-";
    std::string tri = installed_triple(dir);
    fs::path fallback;
    for (const fs::directory_entry& e : fs::directory_iterator(dir)) {
        std::string n = su::wide_to_utf8(e.path().filename().wstring());
        if (!su::starts_with(n, want) || !su::ends_with(n, ".tar.gz")) continue;
        std::string triple = n.substr(want.size(), n.size() - want.size() - 7);
        if (triple == tri) return e.path(); // 与已装平台精确匹配
        if (fallback.empty()) fallback = e.path();
    }
    return fallback;
}

// 版本名自然比较（数字段按数值比较）：支持 "3.12.6" / "v22.14.0" / "go1.22.0" / "21.0.12+7"
// 前导 v/V（后跟数字）仅为版本标记，比较时跳过，保证 "v1.12.13" 与 "3.47.6"、"v22.14.0"
// 与 "22.14.0"（--version 捕获值常无 v）两两比较结果一致
inline int ver_natural_cmp(const std::string& a, const std::string& b) {
    size_t i = a.size() > 1 && (a[0] == 'v' || a[0] == 'V') && isdigit((unsigned char)a[1]) ? 1 : 0;
    size_t j = b.size() > 1 && (b[0] == 'v' || b[0] == 'V') && isdigit((unsigned char)b[1]) ? 1 : 0;
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

// 从 --version 输出行提取版本 token（node: v22.14.0 / go: go1.22.0 / java: 引号内 / 其余取尾 token）
inline std::string extract_ver_token(const std::string& lang, const std::string& line) {
    if (lang == "jdk") {
        size_t q1 = line.find('"');
        size_t q2 = q1 == std::string::npos ? std::string::npos : line.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos)
            return line.substr(q1 + 1, q2 - q1 - 1);
        return line;
    }
    if (lang == "php" || lang == "ruby" || lang == "flutter") {
        // "PHP 8.3.14 (cli)…" / "ruby 3.3.5 (2024-…)" / "Flutter 3.24.3 • …"
        // → 语言名后的第一个 token
        size_t p = line.find(' ');
        while (p != std::string::npos && p + 1 < line.size()) {
            size_t b = p + 1, e2 = b;
            while (e2 < line.size() && !isspace((unsigned char)line[e2])) ++e2;
            if (e2 > b) return line.substr(b, e2 - b);
            p = e2;
        }
        return line;
    }
    if (lang == "go" || lang == "node") {
        const char* prefix = lang == "go" ? "go" : "v";
        for (size_t k = 0; k + 2 < line.size(); ++k) {
            bool hit = line[k] == prefix[0] && line[k + 1] == prefix[1] &&
                       isdigit((unsigned char)line[k + 2]);
            if (!hit && lang == "node") hit = line[k] == 'v' && isdigit((unsigned char)line[k + 1]);
            if (hit) {
                size_t end = k;
                while (end < line.size() && !isspace((unsigned char)line[end])) ++end;
                return line.substr(k, end - k);
            }
        }
        return line;
    }
    size_t end = line.find_last_not_of(" \t\r");
    if (end == std::string::npos) return line;
    size_t start = line.find_last_of(" \t\r", end);
    return line.substr(start == std::string::npos ? 0 : start + 1, end - start);
}

// p 是否位于 root 之下（大小写不敏感，忽略末尾反斜杠）
inline bool path_under(const fs::path& p, const fs::path& root) {
    std::wstring a = su::lower(p.wstring()), b = su::lower(root.wstring());
    while (!a.empty() && a.back() == L'\\') a.pop_back();
    while (!b.empty() && b.back() == L'\\') b.pop_back();
    if (b.empty() || a.size() <= b.size()) return false;
    return a.compare(0, b.size(), b) == 0 && a[b.size()] == L'\\';
}

// ------------------------------------------------------- 已装检测（TUI 共用）

// 一条已装记录：版本 + 位置 + 管理来源 + 是否为 current junction 指向
// flat_root：该路径本身即安装根目录（单版本平铺语言），无版本子目录/junction
struct DetectedInst {
    std::string version;
    std::string path; // 版本目录 / 安装根目录（utf8）
    bool managed = false;
    bool is_current = false;
    bool flat_root = false;
};

// 某语言已装检测（后台线程调用；provider 为空时按 Python 流程）：
// 受管记录（JSON 文件）+ 外部安装探测（环境变量 / PATH）
inline std::vector<DetectedInst> detect_lang_installs(const std::string& id,
                                                      prov::Provider* provider) {
    std::vector<DetectedInst> rows;
    std::error_code ec;
    auto mark_current = [&ec](DetectedInst& d, const fs::path& ver_dir) {
        fs::path junction = ver_dir.parent_path() / L"current";
        if (fs::exists(junction, ec)) {
            std::error_code ec2;
            d.is_current = fs::equivalent(junction, ver_dir, ec2);
        }
    };
    if (id == "python") {
        for (const std::wstring& r : platform::managed_get_roots("python")) {
            fs::path root(r);
            if (!fs::is_directory(root, ec)) continue;
            for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
                if (!e.is_directory()) continue;
                std::string n = su::wide_to_utf8(e.path().filename().wstring());
                if (n == "current" || n == "archives" || (!n.empty() && n[0] == '_')) continue;
                if (!fs::exists(e.path() / L"python.exe", ec)) continue;
                DetectedInst d{n, su::wide_to_utf8(e.path().wstring()), true, false};
                mark_current(d, e.path());
                rows.push_back(std::move(d));
            }
        }
        for (const py::InstalledPy& p : py::detect_installed()) {
            bool dup = false;
            for (const DetectedInst& r : rows)
                if (su::lower(su::utf8_to_wide(r.path)) == su::lower(p.path)) {
                    dup = true;
                    break;
                }
            if (!dup) rows.push_back({p.version, su::wide_to_utf8(p.path), false, false});
        }
    } else if (provider) {
        auto insts = prov::managed_installs(*provider);
        for (auto& inst : insts) {
            DetectedInst d{inst.version, su::wide_to_utf8(inst.dir.wstring()), true, false,
                           inst.flat_root};
            if (d.version.empty()) d.version = "未知版本"; // 平铺 --version 捕获失败兑底
            if (inst.flat_root)
                d.is_current = true; // 平铺安装：根目录即 PATH 指向
            else
                mark_current(d, inst.dir);
            rows.push_back(std::move(d));
        }
        // 受管行按版本自然序降序（新版本在前）
        std::stable_sort(rows.begin(), rows.end(),
                         [](const DetectedInst& a, const DetectedInst& b) {
                             return ver_natural_cmp(a.version, b.version) > 0;
                         });
        // 外部安装：环境变量优先，其次 PATH 探测
        fs::path exe;
        wchar_t envbuf[MAX_PATH * 2] = {};
        const wchar_t* envname = id == "jdk"     ? L"JAVA_HOME"
                                 : id == "go"    ? L"GOROOT"
                                 : id == "dotnet" ? L"DOTNET_ROOT"
                                 : id == "flutter" ? L"FLUTTER_ROOT"
                                                  : nullptr;
        if (envname && GetEnvironmentVariableW(envname, envbuf, MAX_PATH * 2)) {
            fs::path home(envbuf);
            exe = id == "jdk"  ? home / L"bin" / L"java.exe"
                  : id == "go" ? home / L"bin" / L"go.exe"
                  : id == "flutter" ? home / L"bin" / L"flutter.bat"
                               : home / L"dotnet.exe";
        }
        const wchar_t* exe_name = id == "jdk"     ? L"java.exe"
                                  : id == "go"    ? L"go.exe"
                                  : id == "dotnet" ? L"dotnet.exe"
                                  : id == "zig"   ? L"zig.exe"
                                  : id == "php"   ? L"php.exe"
                                  : id == "ruby"  ? L"ruby.exe"
                                  : id == "git"   ? L"git.exe"
                                  : id == "flutter" ? L"flutter.bat"
                                                  : L"node.exe";
        // exe 目录 → 安装根目录的上溯层数（bin/、cmd/ 内的可执行文件上溯 2 层）
        int depth = (id == "jdk" || id == "ruby" || id == "git" || id == "flutter") ? 2 : 1;
        if (!fs::is_regular_file(exe, ec)) exe = platform::find_in_path(exe_name);
        if (!exe.empty() && fs::is_regular_file(exe, ec)) {
            fs::path home = exe.parent_path();
            for (int i = 1; i < depth; ++i) home = home.parent_path();
            bool dup = false;
            for (const DetectedInst& r : rows)
                if (path_under(exe, su::utf8_to_wide(r.path)) ||
                    path_under(home, su::utf8_to_wide(r.path))) {
                    dup = true;
                    break;
                }
            if (!dup) {
                std::wstring flag = id == "jdk" ? L"-version" : L"--version";
                std::string line = platform::capture_first_line(exe, flag);
                std::string ver = extract_ver_token(id, line);
                if (!ver.empty())
                    rows.push_back({ver, su::wide_to_utf8(home.wstring()), false, false});
            }
        }
    }
    return rows;
}

// Rust 已装检测（平铺布局特例）：PATH / 常见位置查找 rustc.exe
inline bool detect_rust_install(DetectedInst& out, bool& rustup_managed) {
    rustup_managed = false;
    std::string line;
    fs::path exe = find_installed_rust(line);
    if (exe.empty()) return false;
    SemVer v = parse_semver(line);
    fs::path root = exe.parent_path().parent_path();
    out.version = v.ok ? v.text : line;
    out.path = su::wide_to_utf8(root.wstring());
    out.managed = false;
    out.is_current = false;
    for (const std::wstring& r : platform::managed_get_roots("rust")) {
        std::wstring a = su::lower(r), b = su::lower(root.wstring());
        while (!a.empty() && a.back() == L'\\') a.pop_back();
        while (!b.empty() && b.back() == L'\\') b.pop_back();
        if (a == b) {
            out.managed = true;
            break;
        }
    }
    wchar_t up[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH * 2)) {
        fs::path cargo_bin = fs::path(up) / L".cargo" / L"bin";
        rustup_managed = su::lower(exe.parent_path().wstring()) ==
                             su::lower(cargo_bin.wstring()) &&
                         fs::exists(fs::path(up) / L".rustup");
    }
    return true;
}

// 清理本工具缓存（<exe目录>\cache：SDK/版本索引缓存、7zr 工具缓存）
// 返回是否找到缓存；bytes/files 为清理量
inline bool clean_exe_cache(uint64_t& bytes, size_t& files) {
    bytes = 0;
    files = 0;
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    fs::path dir = fs::path(exe).parent_path() / L"cache";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) {
            bytes += (uint64_t)it->file_size(ec);
            ++files;
        }
    }
    fs::remove_all(dir, ec);
    return files > 0;
}

// 所有 UI 状态（shared_ptr 持有，闭包按值捕获，生命周期安全）
struct AppState : std::enable_shared_from_this<AppState> {
    std::shared_ptr<Session> sp_s; // 后台线程持有所属会话，防止悬垂
    Session& s;
    ScreenInteractive& screen;
    std::atomic<bool> exiting{false}; // 置位后工作线程不再 PostEvent
    int step = S_UpdateCheck;
    std::vector<int> hist;
    bool rustup_managed = false;

    // 检查更新屏
    std::string uc_title;
    std::vector<std::string> uc_items;
    int uc_sel = 0;
    bool uc_is_new = false;

    // 路径屏
    std::string path_err;

    // 版本屏（完整列表后台加载）
    std::atomic<bool> ver_loading{false};
    std::atomic<bool> ver_load_done{false};
    std::atomic<bool> ver_load_end{false};
    std::string ver_load_err;
    std::vector<github::Release> ver_load_result;
    std::thread ver_thread;

    // 版本屏
    std::vector<std::string> ver_items; // 过滤后的显示行
    std::vector<size_t> ver_map;        // 显示行 → s.rp.all 下标
    std::string ver_query;              // 即时搜索关键字
    int ver_sel = 0;
    bool ver_loaded = false;
    std::string ver_err;

    // 目标平台屏
    std::vector<std::string> tg_items;
    int tg_sel = 0;
    std::string custom_err;

    // 组件包屏
    std::vector<std::string> pk_names, pk_items;
    int pk_sel = 0;
    std::string pk_note, pk_err;
    bool pk_single = false;

    // 格式屏
    std::vector<std::string> fm_items{
        "tar.gz —— 推荐：下载后自动解压为完整可用的 Rust 环境",
        "msi   —— Windows 安装器：下载后调用 msiexec 安装",
    };
    int fm_sel = 0;
    std::string confirm_err;
    bool cb_path = true;  // 安装后加入用户 PATH
    bool cb_keep = false; // 安装后保留压缩包
    bool cb_install = true; // 下载后自动安装/解压（Python）

    // Node.js 工具链（仅 --sdk node 生效），优先级：npm/npx（随装自带，仅检测显示）
    // → Corepack（Node 内置，启用/禁用开关）→ pnpm/yarn（经 Corepack 管理，不下载二进制）
    bool cb_corepack = true;  // 启用 Corepack（管理 pnpm/yarn，默认开启）
    bool cb_pnpm = true;      // 安装后经 Corepack 全局安装 pnpm（默认开启）
    bool cb_yarn = false;     // 同时经 Corepack 全局安装 yarn
    std::string pnpm_base;    // pnpm/npm 存储根目录（空 = <安装目录>\pnpm-repository）
    std::vector<std::string> sdk_tool_lines;   // 完成页：安装后工具链配置/检测报告
    std::vector<std::string> sdk_tool_rows;    // 版本页：本机 PATH 工具链检测行
    std::vector<std::string> sdk_tools_result; // 后台检测结果暂存（UI 线程合并）
    std::atomic<bool> sdk_tools_ready{false};
    bool sdk_tools_probed = false;             // 本会话已探测过本机工具链（node）
    std::thread sdk_tools_thread;

    // Python 管理
    bool pymode = false;
    bool py_manifest_loading = false;
    bool py_manifest_ok = false;
    std::atomic<bool> py_manifest_done{false};
    std::thread py_manifest_thread;
    std::string py_manifest_err;
    std::vector<py::PyVersion> py_versions;
    std::vector<std::string> py_ver_items; // 过滤显示行
    std::vector<size_t> py_ver_map;        // → py_versions 下标
    std::string py_query;
    int py_ver_sel = 0;
    std::string py_ver;
    std::vector<py::PyFile> py_ver_files;  // 选定版本的文件
    std::vector<std::string> py_file_items;
    int py_file_sel = 0;
    py::PyFile py_file;                    // 选定文件
    std::string py_screen_err;
    std::string py_verify_line;
    std::vector<py::InstalledPy> py_installed;
    bool py_repair = false; // Python 修复模式（本地包优先）

    // ---- 已安装检测（统一屏：Python / SDK；Rust 走检查更新屏） ----
    using InstRow = DetectedInst;
    enum { IA_INSTALL = 0, IA_UPDATE, IA_UNINSTALL, IA_BACK, IA_SWITCH, IA_OPEN };
    std::vector<InstRow> inst_rows;
    std::atomic<bool> inst_done{false};
    std::thread inst_thread;
    std::vector<std::string> inst_items;              // 动作菜单（与 inst_actions 对齐）
    std::vector<std::pair<int, size_t>> inst_actions; // (动作, 行下标)
    int inst_sel = 0;
    std::string inst_detect_lang; // 检测结果所属语言（防止切换目标后显示旧结果）
    bool inst_confirm = false;    // 卸载二次确认状态
    std::string inst_confirm_ver; // 待确认卸载的版本
    fs::path inst_confirm_dir;    // 待确认卸载的版本目录
    bool inst_confirm_flat = false; // 待确认卸载是否为平铺安装（根目录即安装目录）
    std::vector<std::string> uninstall_lines; // 卸载完成页展示

    std::string inst_lang_disp() const {
        if (pymode) return "Python";
        return sdk_provider ? sdk_provider->display() : "SDK";
    }
    std::string inst_lang_id() const { return pymode ? "python" : sdk_id; }

    // 受管版本中的最高版本（自然序比较）
    std::string inst_managed_max() const {
        std::string best;
        for (const InstRow& r : inst_rows)
            if (r.managed && (best.empty() || ver_natural_cmp(r.version, best) > 0))
                best = r.version;
        return best;
    }
    fs::path inst_row_dir(const std::string& ver) const {
        for (const InstRow& r : inst_rows)
            if (r.version == ver) return su::utf8_to_wide(r.path);
        return {};
    }
    // 该语言是否多版本布局（JDK/Python/Node.js）；其余单版本平铺
    bool lang_multi_version() const {
        if (pymode) return true;
        return sdk_provider ? sdk_provider->multi_version() : true;
    }

    // 后台检测：受管根目录扫描（JSON 记录）+ 外部安装探测（注册表/环境变量/PATH）
    void begin_inst_detect() {
        if (inst_thread.joinable()) inst_thread.join();
        inst_done = false;
        inst_confirm = false;
        inst_items.clear();
        inst_actions.clear();
        inst_detect_lang = inst_lang_id();
        auto self = shared_from_this();
        std::string id = inst_lang_id();
        bool py = pymode;
        inst_thread = std::thread([self, id, py] {
            std::unique_ptr<prov::Provider> p;
            if (!py) p = prov::Registry::instance().create(id);
            std::vector<InstRow> rows = detect_lang_installs(id, p.get());
            {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->inst_rows = std::move(rows);
            }
            self->inst_done = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
    }
    void finish_inst_detect() {
        if (inst_thread.joinable()) inst_thread.join();
        rebuild_inst_items();
    }

    void rebuild_inst_items() {
        inst_sel = 0;
        inst_items.clear();
        inst_actions.clear();
        if (!inst_confirm) {
            inst_items.push_back("安装新版本…");
            inst_actions.push_back({IA_INSTALL, 0});
            std::string latest;
            if (pymode && !py_versions.empty()) latest = py_versions[0].version;
            if (!pymode && !sdk_versions.empty()) latest = sdk_versions[0].id;
            std::string max_ver = inst_managed_max();
            if (!latest.empty() && !max_ver.empty()) {
                bool show = ver_natural_cmp(max_ver, latest) < 0;
                // dotnet 版本目录为补丁版本号（如 8.0.425），latest 为通道号（如 8.0）：
                // 同通道内可能有新补丁，始终提供“更新”入口（确认页显示真实补丁版本）
                if (!show && !pymode && sdk_id == "dotnet" &&
                    (su::starts_with(max_ver, latest + ".") || max_ver == latest))
                    show = true;
                if (show) {
                    bool flat_upd = !pymode && sdk_provider && !sdk_provider->multi_version();
                    inst_items.push_back(flat_upd
                                             ? "更新到最新版 " + latest + "（覆盖当前 " +
                                                   max_ver + "）"
                                             : "更新到最新版 " + latest + "（当前 " + max_ver +
                                                   "，版本共存）");
                    inst_actions.push_back({IA_UPDATE, 0});
                }
            }
            // 受管版本：设为当前（仅多版本布局；非 current 指向时）+ 卸载
            for (size_t i = 0; i < inst_rows.size(); ++i) {
                if (!inst_rows[i].managed) continue;
                if (!inst_rows[i].is_current && !inst_rows[i].flat_root) {
                    inst_items.push_back("设为当前 ▸ " + inst_rows[i].version + "（" +
                                         inst_rows[i].path + "）");
                    inst_actions.push_back({IA_SWITCH, i});
                }
                inst_items.push_back("卸载 " + inst_rows[i].version + "（" +
                                     inst_rows[i].path + "）");
                inst_actions.push_back({IA_UNINSTALL, i});
            }
            // 打开目录（优先最新受管版本，否则第一行）
            size_t open_idx = inst_rows.size();
            for (size_t i = 0; i < inst_rows.size(); ++i) {
                if (!inst_rows[i].managed) continue;
                if (open_idx >= inst_rows.size() ||
                    ver_natural_cmp(inst_rows[i].version, inst_rows[open_idx].version) > 0)
                    open_idx = i;
            }
            if (open_idx >= inst_rows.size() && !inst_rows.empty()) open_idx = 0;
            if (open_idx < inst_rows.size()) {
                inst_items.push_back("打开安装目录（" + inst_rows[open_idx].path + "）");
                inst_actions.push_back({IA_OPEN, open_idx});
            }
            inst_items.push_back("返回");
            inst_actions.push_back({IA_BACK, 0});
        } else {
            inst_items.push_back("[确认] 卸载 " + inst_confirm_ver +
                                 "（删除 " + su::wide_to_utf8(inst_confirm_dir.wstring()) + "）");
            inst_actions.push_back({IA_UNINSTALL, (size_t)-1});
            inst_items.push_back("[取消] 返回");
            inst_actions.push_back({IA_BACK, 0});
        }
    }

    void inst_accept() {
        if (inst_actions.empty() || inst_sel < 0 ||
            inst_sel >= (int)inst_actions.size())
            return;
        int act = inst_actions[(size_t)inst_sel].first;
        size_t idx = inst_actions[(size_t)inst_sel].second;
        if (act == IA_INSTALL) {
            inst_confirm = false;
            logx::linef("用户选择: 安装新版本（%s）", inst_lang_disp().c_str());
            if (pymode) go(S_PyVersion);
            else go(S_SdkVersion);
            return;
        }
        if (act == IA_UPDATE) {
            std::string latest, max_ver = inst_managed_max();
            if (pymode) latest = py_versions.empty() ? std::string() : py_versions[0].version;
            else latest = sdk_versions.empty() ? std::string() : sdk_versions[0].id;
            fs::path ver_dir = inst_row_dir(max_ver);
            if (latest.empty() || ver_dir.empty()) return;
            // 平铺布局：版本目录即根目录，更新为覆盖安装；多版本：根目录为版本目录父级
            fs::path root = lang_multi_version() ? ver_dir.parent_path() : ver_dir;
            logx::linef("用户选择: 更新 %s → %s（根目录 %s）", max_ver.c_str(),
                        latest.c_str(), su::wide_to_utf8(root.wstring()).c_str());
            s.dir = root;
            s.dir_set = true;
            s.path_input = w(root);
            if (pymode) {
                py_ver = latest;
                py_repair = false;
                if (!pick_py_file(latest)) return;
                go(S_PyConfirm);
            } else {
                std::string err;
                prov::Artifact f;
                if (!sdk_provider->resolve(latest, f, err)) {
                    sdk_screen_err = err;
                    return;
                }
                sdk_file = f;
                go(S_SdkConfirm);
            }
            return;
        }
        if (act == IA_SWITCH) { // 设为当前版本（重指 current junction；仅多版本布局）
            if (idx >= inst_rows.size() || inst_rows[idx].flat_root) return;
            fs::path ver_dir = su::utf8_to_wide(inst_rows[idx].path);
            logx::linef("用户选择: 设为当前 %s %s", inst_lang_disp().c_str(),
                        inst_rows[idx].version.c_str());
            start_switch_work(inst_rows[idx].version, ver_dir);
            return;
        }
        if (act == IA_OPEN) { // 打开版本目录（资源管理器）
            if (idx >= inst_rows.size()) return;
            fs::path dir = su::utf8_to_wide(inst_rows[idx].path);
            logx::linef("用户选择: 打开目录 %s", inst_rows[idx].path.c_str());
            ShellExecuteW(nullptr, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (act == IA_UNINSTALL) {
            if (!inst_confirm) {
                if (idx >= inst_rows.size()) return;
                inst_confirm = true;
                inst_confirm_ver = inst_rows[idx].version;
                inst_confirm_dir = su::utf8_to_wide(inst_rows[idx].path);
                inst_confirm_flat = inst_rows[idx].flat_root;
                logx::linef("用户选择: 卸载 %s %s（待确认）", inst_lang_disp().c_str(),
                            inst_confirm_ver.c_str());
                rebuild_inst_items();
                return;
            }
            inst_confirm = false;
            std::string ver = inst_confirm_ver;
            fs::path ver_dir = inst_confirm_dir;
            bool flat = inst_confirm_flat;
            start_uninstall_work(ver, ver_dir, flat);
            return;
        }
        // IA_BACK
        if (inst_confirm) {
            inst_confirm = false;
            rebuild_inst_items();
            return;
        }
        back();
    }

    // 为指定 Python 版本挑选最佳 Windows 安装文件（exe 优先，其次 msi/embed zip）
    bool pick_py_file(const std::string& target) {
        std::vector<py::PyFile> files;
        std::string err;
        if (!py::fetch_files_ftp(target, "", files, err) || files.empty()) {
            py_screen_err = "未找到 " + target + " 的安装文件：" + err;
            return false;
        }
        std::string arch = py::detect_host_arch();
        const py::PyFile* best = nullptr;
        for (const py::PyFile& f : files) {
            if (f.arch != arch) continue;
            std::string l = su::lower(f.filename);
            bool installer = su::ends_with(l, ".exe") || su::ends_with(l, ".msi");
            bool embed = l.find("embed") != std::string::npos && su::ends_with(l, ".zip");
            if (installer) {
                best = &f;
                break;
            }
            if (embed && !best) best = &f;
        }
        if (!best) {
            py_screen_err = "版本 " + target + " 没有 Windows " + arch + " 安装包";
            return false;
        }
        py_file = *best;
        return true;
    }

    // 卸载（后台执行）：多版本删除版本目录 + junction 维护；平铺删除整个根目录
    // PATH/受管记录清理
    void start_uninstall_work(const std::string& ver, const fs::path& ver_dir, bool flat) {
        fs::path root = flat ? ver_dir : ver_dir.parent_path();
        reset_work("正在卸载 " + inst_lang_disp() + " " + ver);
        {
            std::lock_guard<std::mutex> lk(wmu);
            w_file = su::wide_to_utf8(ver_dir.wstring());
            w_note = flat ? "正在删除安装目录并清理 PATH / 受管记录 …"
                          : "正在删除版本目录并维护 junction / PATH …";
        }
        w_kind = 4;
        bool py = pymode;
        auto self = shared_from_this();
        w_thread = std::thread([self, py, ver, root, ver_dir, flat] {
            std::string err;
            bool ok = false;
            if (py) ok = uninstall_python_version(root, ver, err);
            else if (self->sdk_provider)
                ok = prov::uninstall_version(*self->sdk_provider, root, ver, err);
            else err = "内部错误：未知卸载目标";
            std::lock_guard<std::mutex> lk(self->wmu);
            if (ok) {
                self->uninstall_lines.clear();
                self->uninstall_lines.push_back("√ 已卸载 " + self->inst_lang_disp() + " " + ver);
                self->uninstall_lines.push_back("  原位置: " + su::wide_to_utf8(ver_dir.wstring()));
                self->uninstall_lines.push_back(
                    flat ? "  安装目录 / PATH / 环境变量 / 受管记录已清理"
                         : "  current junction / PATH / 受管记录已同步维护；剩余版本自动接管 current");
                self->w_phase = 1;
            } else {
                self->w_phase = 2;
                self->w_err = err;
            }
            self->w_finished = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
        go(S_Work);
    }

    // 设为当前版本（后台执行）：重指 current junction，PATH 立即生效
    void start_switch_work(const std::string& ver, const fs::path& ver_dir) {
        fs::path root = ver_dir.parent_path();
        reset_work("正在切换 " + inst_lang_disp() + " 当前版本");
        {
            std::lock_guard<std::mutex> lk(wmu);
            w_file = su::wide_to_utf8(ver_dir.wstring());
            w_note = "正在重指 current junction …";
        }
        w_kind = 6;
        auto self = shared_from_this();
        w_thread = std::thread([self, ver, root, ver_dir] {
            std::string err;
            bool ok = platform::make_junction(root / L"current", ver_dir, err);
            std::lock_guard<std::mutex> lk(self->wmu);
            if (ok) {
                self->uninstall_lines.clear();
                self->uninstall_lines.push_back("√ 当前版本已切换为 " +
                                                self->inst_lang_disp() + " " + ver);
                self->uninstall_lines.push_back("  current → " +
                                                su::wide_to_utf8(ver_dir.wstring()));
                self->uninstall_lines.push_back("  PATH 中 current 指向的目录立即生效");
                self->w_phase = 1;
            } else {
                self->w_phase = 2;
                self->w_err = err;
            }
            self->w_finished = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
        go(S_Work);
    }
    std::map<std::string, py::PyApiMeta> py_api_meta;
    bool py_api_loaded = false;
    std::vector<std::string> ch_items{"Rust 环境",    "Python 环境",
                                      "Node.js",      "JDK (Temurin)",
                                      "Go",           ".NET",
                                      "Zig",          "PHP",
                                      "Ruby",         "Git For Windows",
                                      "Flutter"};
    int ch_sel = 0;

    // SDK 管理（Node/JDK/Go/.NET）
    std::string sdk_id; // node/jdk/go/dotnet（非空 = SDK 模式）
    std::unique_ptr<prov::Provider> sdk_provider;
    std::vector<prov::VersionInfo> sdk_versions;
    std::vector<std::string> sdk_items; // 过滤显示行
    std::vector<size_t> sdk_map;        // → sdk_versions 下标
    std::string sdk_query;
    int sdk_sel = 0;
    std::string sdk_versions_for; // 当前列表所属的 eco（切换目标时强制重载）
    bool sdk_loading = false;
    std::atomic<bool> sdk_load_done{false};
    std::thread sdk_thread;
    std::string sdk_err;
    prov::Artifact sdk_file; // 解析后的具体文件
    std::string sdk_verify_line;
    std::string sdk_screen_err;

    // ---- 管理中心（全部已装语言，首页 ←/→ 进入） ----
    struct HubLang {
        std::string id;
        std::string display;
        std::vector<DetectedInst> rows;
    };
    std::vector<HubLang> hub_result;          // 后台检测结果暂存
    std::vector<HubLang> hub_langs;           // UI 侧列表（仅含已安装的语言）
    std::vector<std::string> hub_items;       // 菜单行（与 hub_map 对齐）
    std::vector<size_t> hub_map;              // 菜单行 → hub_langs 下标
    int hub_sel = 0;
    std::atomic<bool> hub_loading{false};
    std::atomic<bool> hub_done{false};
    std::thread hub_thread;
    std::vector<std::string> hub_notes;       // PATH 体检 / 缓存清理结果

    void begin_hub_detect() {
        if (hub_thread.joinable()) hub_thread.join();
        hub_loading = true;
        hub_done = false;
        hub_items.clear();
        hub_map.clear();
        auto self = shared_from_this();
        hub_thread = std::thread([self] {
            static const struct {
                const char* id;
                const char* disp;
            } kLangs[] = {{"rust", "Rust"},       {"python", "Python"},
                          {"node", "Node.js"},    {"jdk", "JDK (Temurin)"},
                          {"go", "Go"},           {"dotnet", ".NET"},
                          {"zig", "Zig"},         {"php", "PHP"},
                          {"ruby", "Ruby"},       {"git", "Git For Windows"},
                          {"flutter", "Flutter"}};
            std::vector<HubLang> out;
            for (const auto& L : kLangs) {
                HubLang hl;
                hl.id = L.id;
                hl.display = L.disp;
                if (std::string(L.id) == "rust") {
                    DetectedInst d;
                    bool rm = false;
                    if (detect_rust_install(d, rm)) hl.rows.push_back(std::move(d));
                    (void)rm;
                } else if (std::string(L.id) == "python") {
                    hl.rows = detect_lang_installs("python", nullptr);
                } else {
                    auto p = prov::Registry::instance().create(L.id);
                    if (p) hl.rows = detect_lang_installs(L.id, p.get());
                }
                if (!hl.rows.empty()) out.push_back(std::move(hl));
            }
            {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->hub_result = std::move(out);
            }
            self->hub_loading = false;
            self->hub_done = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
    }
    void finish_hub_detect() {
        if (hub_thread.joinable()) hub_thread.join();
        hub_loading = false;
        {
            std::lock_guard<std::mutex> lk(wmu);
            hub_langs = std::move(hub_result);
            hub_result.clear();
        }
        rebuild_hub_items();
    }
    void rebuild_hub_items() {
        hub_sel = 0;
        hub_items.clear();
        hub_map.clear();
        for (size_t i = 0; i < hub_langs.size(); ++i) {
            const HubLang& L = hub_langs[i];
            std::string vs;
            for (size_t k = 0; k < L.rows.size() && k < 3; ++k) {
                const DetectedInst& r = L.rows[k];
                vs += (k ? " · " : "") + r.version;
                if (r.is_current) vs += "（当前）";
                else if (!r.managed) vs += "（外部）";
            }
            if (L.rows.size() > 3) vs += " 等 " + std::to_string(L.rows.size()) + " 个版本";
            hub_items.push_back(L.display + "    " + vs);
            hub_map.push_back(i);
        }
    }
    void hub_accept() {
        if (hub_map.empty() || hub_sel < 0 || hub_sel >= (int)hub_map.size()) return;
        const HubLang& L = hub_langs[hub_map[(size_t)hub_sel]];
        logx::linef("管理中心进入: %s", L.display.c_str());
        if (L.id == "rust") {
            pymode = false;
            sdk_id.clear();
            sdk_provider.reset();
            go(S_UpdateCheck);
            return;
        }
        if (L.id == "python") {
            pymode = true;
            sdk_id.clear();
            sdk_provider.reset();
            go(S_Installed);
            return;
        }
        pymode = false;
        sdk_provider = prov::Registry::instance().create(L.id);
        if (!sdk_provider) return;
        sdk_id = sdk_provider->id();
        go(S_Installed);
    }

    // 工作线程共享（下载 / 本地修复）
    std::mutex wmu;
    std::string w_title, w_file, w_bar, w_note, w_err;
    std::atomic<int> w_phase{0}; // 0=运行中 1=成功 2=失败 3=已取消
    std::atomic<bool> w_cancel{false};
    std::atomic<bool> w_finished{false};
    std::atomic<uint64_t> w_done{0}, w_total{0};
    std::thread w_thread;
    InstallOutcome w_outcome;
    int w_kind = 0; // 0=下载 1=本地修复

    // 步骤条（安装管线可视化）：w_steps 名称序列；w_stage_step 阶段→步骤下标映射
    // （阶段 0=下载 1=校验 2=解压 3=合并 4=配置 5=完成；-1 表示该阶段不映射）
    std::vector<std::string> w_steps;
    std::vector<int> w_stage_step;
    std::atomic<int> w_step{-1};

    // 速度统计（仅工作线程访问）
    double w_bps = 0;
    uint64_t w_last_bytes = 0;
    std::chrono::steady_clock::time_point w_last_draw = std::chrono::steady_clock::now();

    AppState(std::shared_ptr<Session> sp, ScreenInteractive& scr)
        : sp_s(std::move(sp)), s(*sp_s), screen(scr) {}
    ~AppState() {
        exiting = true;
        if (w_thread.joinable()) w_thread.detach();
        if (ver_thread.joinable()) ver_thread.detach();
        if (sdk_tools_thread.joinable()) sdk_tools_thread.detach();
        if (hub_thread.joinable()) hub_thread.detach();
        if (inst_thread.joinable()) inst_thread.detach();
    }

    // ---------------- 导航 ----------------
    void go(int next) {
        // 进入路径屏时预填该语言上次使用的根目录（未手动指定过路径时）
        if (next == S_Path && !s.dir_set && s.path_input.empty()) {
            std::wstring last = platform::managed_get_last_root(
                pymode ? "python" : (sdk_id.empty() ? "rust" : sdk_id));
            if (!last.empty()) s.path_input = su::wide_to_utf8(last);
        }
        hist.push_back(step);
        enter(next);
    }
    void back() {
        if (hist.empty()) {
            screen.ExitLoopClosure()();
            return;
        }
        int prev = hist.back();
        hist.pop_back();
        enter(prev);
    }
    void enter(int st) {
        step = st;
        switch (st) {
            case S_UpdateCheck: rebuild_updatecheck(); break;
            case S_Version: rebuild_versions(); break;
            case S_Target: rebuild_targets(); break;
            case S_Package: rebuild_packages(); break;
            case S_Installed:
                begin_inst_detect();
                begin_lists_for_installed(); // 后台拉取版本列表（快速更新项需要最新版本号）
                break;
            case S_Hub:
                if (!hub_loading && !hub_done.load()) begin_hub_detect();
                break;
            case S_SdkVersion:
                sdk_screen_err.clear();
                if (sdk_id == "node" && !sdk_tools_probed)
                    begin_node_tools_probe(); // 后台检测本机 node/npm/pnpm/yarn 版本
                if (sdk_versions_for != sdk_id) { // 切换了目标 → 丢弃旧列表并重载
                    sdk_versions.clear();
                    sdk_query.clear();
                    sdk_sel = 0;
                    begin_sdk_load();
                } else if (!sdk_versions.empty()) {
                    refilter_sdk_versions();
                } else if (!sdk_loading && !sdk_load_done.load()) {
                    begin_sdk_load();
                }
                break;
            case S_PyVersion:
                if (!py_manifest_ok && !py_manifest_loading && py_manifest_done.load())
                    py_screen_err.clear(); // 保留错误直到重新开始
                if (py_manifest_ok) refilter_py_versions();
                else if (!py_manifest_loading && !py_manifest_done.load()) begin_py_manifest();
                break;
            default: break;
        }
    }
    // 第一个尚未确定的步骤（手动流程）；路径预填该语言上次使用的根目录
    int first_unset() {
        if (!s.dir_set) return S_Path;
        if (!s.ver_set) return S_Version;
        if (!s.target_set) return S_Target;
        if (!s.pkg_locked()) return S_Package;
        if (!s.format_set) return S_Format;
        return S_Confirm;
    }

    // 后台启动版本列表加载（已装检测屏的快速更新项需要最新版本号）
    void begin_lists_for_installed() {
        if (pymode) {
            if (!py_manifest_ok && !py_manifest_loading && !py_manifest_done.load())
                begin_py_manifest();
        } else if (sdk_provider) {
            if (sdk_versions_for != sdk_id) {
                sdk_versions.clear();
                sdk_query.clear();
                sdk_sel = 0;
                begin_sdk_load();
            } else if (sdk_versions.empty() && !sdk_loading && !sdk_load_done.load()) {
                begin_sdk_load();
            }
        }
    }

    // ---------------- 检查更新（Rust） ----------------
    enum { UA_UPDATE = 0, UA_REINSTALL, UA_REPAIR, UA_MANUAL, UA_UNINSTALL };
    std::vector<int> uc_actions; // 与 uc_items 一一对应
    void rebuild_updatecheck() {
        uc_sel = 0;
        uc_actions.clear();
        uc_items.clear();
        fs::path root = s.rustc_exe.parent_path().parent_path();
        SemVer cur = s.rust_ok ? parse_semver(s.installed_line) : SemVer{};
        SemVer lat = s.rp.all.empty() ? SemVer{} : parse_semver(s.rp.all[0].tag);
        if (!s.rp.all.empty() && s.rust_ok && cur.ok && lat.ok && semver_cmp(cur, lat) < 0) {
            uc_is_new = true;
            uc_title = "检测到新版本：" + lat.text + "（当前 " + cur.text + "，安装于 " + w(root) +
                       "）";
            uc_items.push_back("是，更新到最新版本 " + s.rp.all[0].tag + "（自动配置，确认后开始）");
            uc_actions.push_back(UA_UPDATE);
            uc_items.push_back("否，手动选择其他版本");
            uc_actions.push_back(UA_MANUAL);
        } else if (s.rust_ok && cur.ok) {
            uc_is_new = false;
            uc_title = "已安装 Rust：" + cur.text + "（安装于 " + w(root) + "）";
            if (!s.rp.all.empty()) {
                uc_items.push_back("修复 " + cur.text + "（一键本地秒修；无本地安装包时自动下载）");
                uc_actions.push_back(UA_REPAIR);
                uc_items.push_back("重装 " + cur.text + "（删除旧压缩包，重新下载并覆盖安装）");
                uc_actions.push_back(UA_REINSTALL);
            }
            uc_items.push_back("手动选择其他版本");
            uc_actions.push_back(UA_MANUAL);
            if (rustup_managed) {
                uc_items.push_back("（该 Rust 由 rustup 管理，卸载请使用 rustup，本工具不接管）");
                uc_actions.push_back(-1);
            } else {
                uc_items.push_back("卸载当前 Rust 安装（删除 " + w(root) + " 并清理 PATH）");
                uc_actions.push_back(UA_UNINSTALL);
            }
        } else {
            uc_is_new = false;
            uc_title = s.rp.all.empty()
                           ? "无法获取最新版本信息（离线或网络受限）"
                           : "未检测到本机 Rust 安装";
            uc_items.push_back("手动选择版本安装");
            uc_actions.push_back(UA_MANUAL);
        }
    }

    void apply_update_preset(const fs::path& root, const std::string& tag,
                             const std::string& date) {
        logx::linef("目标目录: %s", w(root));
        s.dir = root;
        s.dir_set = true;
        s.path_input = w(root);
        s.ver = tag;
        s.ver_date = date;
        s.ver_set = true;
        s.invalidate_manifest();
        s.target = installed_triple(root);
        s.target_set = true;
        s.pkg = "rust";
        s.format = "tar.gz";
        s.format_set = true;
        logx::linef("自动配置: 版本=%s 平台=%s 组件=rust 格式=tar.gz 修复=%d", tag.c_str(),
                    s.target.c_str(), (int)s.repair);
    }
    void auto_config(bool repair) {
        apply_update_preset(s.rustc_exe.parent_path().parent_path(), s.rp.all[0].tag,
                            s.rp.all[0].date);
        s.repair = repair;
    }

    void uc_accept() {
        if (uc_actions.empty() || uc_sel < 0 || uc_sel >= (int)uc_actions.size()) return;
        int act = uc_actions[(size_t)uc_sel];
        if (act < 0) return; // 不可用项（如 rustup 托管）
        if (act == UA_UPDATE) {
            logx::linef("用户选择: 更新到最新版本 %s", s.rp.all[0].tag.c_str());
            auto_config(false);
            go(S_Confirm);
            return;
        }
        if (act == UA_MANUAL) {
            logx::line("用户选择: 手动安装其他版本（先选版本，再设路径）");
            go(S_Version);
            return;
        }
        if (act == UA_UNINSTALL) { // Rust 卸载（平铺布局特例）
            fs::path root = s.rustc_exe.parent_path().parent_path();
            logx::linef("用户选择: 卸载 Rust（%s）", w(root).c_str());
            reset_work("正在卸载 Rust");
            {
                std::lock_guard<std::mutex> lk(wmu);
                w_file = w(root);
                w_note = "正在删除安装目录并清理 PATH / 受管记录 …";
            }
            w_kind = 5;
            auto self = shared_from_this();
            w_thread = std::thread([self, root] {
                std::string err;
                bool ok = uninstall_rust_root(root, err);
                std::lock_guard<std::mutex> lk(self->wmu);
                if (ok) {
                    self->uninstall_lines.clear();
                    self->uninstall_lines.push_back("√ 已卸载 Rust");
                    self->uninstall_lines.push_back("  原位置: " + w(root));
                    self->uninstall_lines.push_back("  PATH / 受管记录已清理");
                    self->w_phase = 1;
                } else {
                    self->w_phase = 2;
                    self->w_err = err;
                }
                self->w_finished = true;
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
            });
            go(S_Work);
            return;
        }
        // 修复 / 重装（均需版本列表可用）
        if (act == UA_REPAIR) {
            logx::line("用户选择: 修复");
            fs::path root = s.rustc_exe.parent_path().parent_path();
            fs::path local = find_local_archive(root, s.rp.all[0].tag);
            if (!local.empty())
                logx::linef("发现本地安装包: %s", w(local).c_str());
            if (!local.empty()) {
                apply_update_preset(root, s.rp.all[0].tag, s.rp.all[0].date);
                s.repair = true;
                s.local_archive = local;
                go(S_RepairLocal);
            } else {
                auto_config(true);
                go(S_Confirm);
            }
            return;
        }
        // UA_REINSTALL
        logx::line("用户选择: 重装");
        auto_config(false);
        go(S_Confirm);
    }

    // ---------------- 路径 ----------------
    void path_accept() {
        fs::path p = resolve_dir(su::utf8_to_wide(s.path_input));
        std::error_code ec;
        fs::create_directories(p, ec);
        if (ec) {
            path_err = "无法创建目录：" + ec.message();
            return;
        }
        path_err.clear();
        s.dir = p;
        s.dir_set = true;
        logx::linef("目标目录: %s", w(p));
        logx::line("用户确认安装路径");
        platform::managed_set_last_root(
            pymode ? "python" : (sdk_id.empty() ? "rust" : sdk_id), p); // 记住该语言根目录
        if (pymode) go(S_PyConfirm);
        else if (!sdk_id.empty()) go(S_SdkConfirm);
        else go(s.ver_set ? S_Target : S_Version);
    }

    // ---------------- 版本 ----------------
    void rebuild_versions() {
        ver_err.clear();
        if (ver_loaded) {
            refilter_versions();
        } else if (!ver_loading) {
            begin_version_load();
        }
    }
    // 后台分页拉取剩余版本列表，避免进入版本屏时卡顿
    void begin_version_load() {
        ver_loading = true;
        ver_load_done = false;
        ver_load_err.clear();
        ver_load_result.clear();
        ver_query.clear();
        ver_items.clear();
        ver_map.clear();
        auto self = shared_from_this();
        size_t have = s.rp.all.size();
        bool end_list = s.rp.end_of_list;
        int first_page = (int)(have / kFetchPerPage) + 1;
        ver_thread = std::thread([self, have, end_list, first_page] {
            std::vector<github::Release> out;
            std::string err;
            int page = first_page;
            bool end_now = end_list;
            while (!end_now) {
                std::vector<github::Release> v;
                if (!github::fetch_releases(page, (int)kFetchPerPage, v, err)) break;
                if ((int)v.size() < (int)kFetchPerPage) end_now = true;
                for (auto& r : v) out.push_back(std::move(r));
                page++;
                if (out.size() > 5000) break; // 防失控
            }
            self->ver_load_result = std::move(out);
            self->ver_load_err = err;
            self->ver_load_end = end_now;
            self->ver_load_done = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
    }
    // UI 线程：合并加载结果
    void finish_version_load() {
        if (ver_thread.joinable()) ver_thread.join();
        if (!ver_load_err.empty()) ver_err = "获取版本列表失败：" + ver_load_err;
        for (auto& r : ver_load_result) s.rp.all.push_back(std::move(r));
        if (ver_load_end) s.rp.end_of_list = true;
        ver_load_result.clear();
        ver_loading = false;
        ver_loaded = true;
        refilter_versions();
        logx::linef("版本列表加载完成（共 %s 个版本）",
                    std::to_string(s.rp.all.size()).c_str());
    }
    // 按关键字（版本号/日期，忽略大小写）实时过滤；空关键字时置顶★最新版本
    void refilter_versions() {
        ver_items.clear();
        ver_map.clear();
        std::string q = su::lower(ver_query);
        size_t start = 0;
        if (q.empty() && !s.rp.all.empty()) {
            ver_map.push_back(0);
            ver_items.push_back("★ 最新版本  " + s.rp.all[0].tag + "  (" + s.rp.all[0].date +
                                ")");
            start = 1;
        }
        for (size_t i = start; i < s.rp.all.size(); ++i) {
            const github::Release& r = s.rp.all[i];
            std::string hay = su::lower(r.tag + " " + r.date);
            if (!q.empty() && hay.find(q) == std::string::npos) continue;
            ver_map.push_back(i);
            ver_items.push_back(r.tag + "     " + r.date);
        }
        if (ver_sel >= (int)ver_items.size()) ver_sel = (int)ver_items.size() - 1;
        if (ver_sel < 0) ver_sel = 0;
    }
    void ver_accept() {
        if (ver_map.empty() || s.rp.all.empty()) return;
        int pos = ver_sel;
        if (pos < 0) pos = 0;
        if (pos >= (int)ver_map.size()) pos = (int)ver_map.size() - 1;
        const github::Release& r = s.rp.all[ver_map[pos]];
        std::string tag = r.tag, date = r.date;
        if (tag != s.ver) s.invalidate_manifest();
        s.ver = tag;
        s.ver_date = date;
        s.ver_set = true;
        logx::linef("用户选择版本: %s (%s)", tag.c_str(), date.c_str());
        go(s.dir_set ? S_Target : S_Path);
    }

    // ---------------- 目标平台 ----------------
    void rebuild_targets() {
        tg_sel = 0;
        custom_err.clear();
        tg_items = {detect_triple() + "  （自动检测，推荐）",
                    "x86_64-pc-windows-msvc",
                    "x86_64-pc-windows-gnu",
                    "i686-pc-windows-msvc",
                    "i686-pc-windows-gnu",
                    "aarch64-pc-windows-msvc",
                    "✎ 输入自定义三元组…"};
    }
    void after_target() {
        if (!target_is_windows(s.target)) { // 跨平台包无 msi
            s.format = "tar.gz";
            s.format_set = true;
            go(S_Confirm);
        } else {
            go(S_Package);
        }
    }
    void target_accept() {
        if (tg_sel == 6) {
            go(S_TargetCustom);
            return;
        }
        if (tg_sel < 0 || tg_sel >= 6) return;
        std::string t = tg_items[tg_sel];
        size_t sp = t.find("  （");
        if (sp != std::string::npos) t = t.substr(0, sp);
        if (t != s.target) s.invalidate_manifest();
        s.target = t;
        s.target_set = true;
        logx::line("目标平台: " + t);
        after_target();
    }
    void custom_accept() {
        if (!valid_triple(s.path_input2)) {
            custom_err = "三元组无效（仅允许字母、数字、- _ .）";
            return;
        }
        custom_err.clear();
        if (s.path_input2 != s.target) s.invalidate_manifest();
        s.target = s.path_input2;
        s.target_set = true;
        logx::line("目标平台(自定义): " + s.path_input2);
        after_target();
    }

    // ---------------- 组件包 ----------------
    void rebuild_packages() {
        pk_sel = 0;
        pk_err.clear();
        pk_note.clear();
        pk_items.clear();
        pk_names.clear();
        ensure_manifest(s);
        if (s.pkg_locked()) {
            if (s.have_manifest) {
                toml::PkgTarget t;
                if (!toml::find_pkg_target(s.toml, s.opt.pkg, s.target, t) || !t.available) {
                    pk_err = "包 " + s.opt.pkg + " 在平台 " + s.target + " 上不可用";
                    for (const std::string& t2 : toml::list_targets(s.toml, s.opt.pkg))
                        pk_note += "可用平台: " + t2 + "   ";
                    return;
                }
            }
            s.pkg = s.opt.pkg;
            pk_single = true;
            pk_note = "已按 --package 指定组件包：" + s.opt.pkg;
            pk_items = {"继续 →"};
            return;
        }
        if (!s.have_manifest) {
            pk_single = true;
            s.pkg = "rust";
            pk_note = "旧版本无发行清单，仅提供 rust 完整工具链";
            pk_items = {"rust（完整工具链）—— Enter 继续"};
            return;
        }
        pk_single = false;
        pk_names = toml::list_pkgs(s.toml, s.target);
        for (const std::string& n : pk_names) {
            std::string v = toml::pkg_version(s.toml, n);
            std::string label = n;
            if (n == "rust") label += "（完整工具链：rustc + cargo + 标准库 + 文档，推荐）";
            if (!v.empty()) label += "   [" + v.substr(0, 48) + "]";
            pk_items.push_back(label);
        }
    }
    void package_accept() {
        if (!pk_err.empty() || pk_items.empty()) {
            back();
            return;
        }
        if (!pk_single && pk_sel >= 0 && pk_sel < (int)pk_names.size()) s.pkg = pk_names[pk_sel];
        logx::line("组件包: " + s.pkg);
        go(S_Format);
    }

    // ---------------- 格式 ----------------
    void format_accept() {
        s.format = fm_sel == 1 ? "msi" : "tar.gz";
        s.format_set = true;
        logx::line("包格式: " + s.format);
        go(S_Confirm);
    }

    // ---------------- Python 管理 ----------------
    void begin_py_manifest() {
        if (py_manifest_loading) return;
        py_manifest_loading = true;
        py_manifest_done = false;
        py_screen_err.clear();
        auto self = shared_from_this();
        py_manifest_thread = std::thread([self] {
            std::vector<py::PyVersion> vers;
            std::string err;
            if (!py::fetch_versions_ftp(vers, err)) {
                self->py_manifest_err = err;
                self->py_manifest_ok = false;
            } else {
                self->py_versions = std::move(vers);
                self->py_manifest_ok = true;
            }
            self->py_manifest_done = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
    }
    void finish_py_manifest() {
        if (py_manifest_thread.joinable()) py_manifest_thread.join();
        py_manifest_loading = false;
        if (!py_manifest_err.empty()) {
            py_screen_err = py_manifest_err;
            return;
        }
        refilter_py_versions();
        if (step == S_Installed) rebuild_inst_items(); // 快速更新项依赖最新版本号
    }
    void refilter_py_versions() {
        py_ver_items.clear();
        py_ver_map.clear();
        std::string q = su::lower(py_query);
        for (size_t i = 0; i < py_versions.size(); ++i) {
            const py::PyVersion& v = py_versions[i];
            std::string hay = su::lower(v.version + " " + v.date);
            if (!q.empty() && hay.find(q) == std::string::npos) continue;
            py_ver_map.push_back(i);
            py_ver_items.push_back(v.version + "     " + v.date);
        }
        if (py_ver_sel >= (int)py_ver_items.size()) py_ver_sel = (int)py_ver_items.size() - 1;
        if (py_ver_sel < 0) py_ver_sel = 0;
    }
    void py_ver_accept() {
        if (py_ver_map.empty()) return;
        int pos = py_ver_sel;
        if (pos < 0) pos = 0;
        if (pos >= (int)py_ver_map.size()) pos = (int)py_ver_map.size() - 1;
        const py::PyVersion& v = py_versions[py_ver_map[pos]];
        py_ver = v.version;
        logx::linef("用户选择 Python 版本: %s (%s)", v.version.c_str(), v.date.c_str());
        rebuild_py_files();
        go(S_PyFile);
    }
    void rebuild_py_files() {
        py_file_sel = 0;
        py_file_items.clear();
        py_ver_files.clear();
        std::string err;
        if (!py::fetch_files_ftp(py_ver, "", py_ver_files, err)) {
            py_screen_err = err;
            return;
        }
        if (!s.opt.py_token.empty()) {
            if (!py_api_loaded) {
                std::string aerr;
                py_api_loaded = py::load_api_metadata(s.opt.py_token, py_api_meta, aerr);
                if (py_api_loaded) logx::line("API 元数据已加载");
                else logx::line("API 元数据不可用（继续使用 FTP 数据）: " + aerr);
            }
            if (py_api_loaded)
                for (py::PyFile& f : py_ver_files) {
                    auto it = py_api_meta.find(f.version + "/" + f.filename);
                    if (it != py_api_meta.end()) {
                        f.sha256 = it->second.sha256;
                        f.md5 = it->second.md5;
                        f.filesize = it->second.filesize;
                        if (!it->second.date.empty()) f.release_date = it->second.date;
                        f.from_api = true;
                    }
                }
        }
        std::stable_sort(py_ver_files.begin(), py_ver_files.end(),
                         [](const py::PyFile& a, const py::PyFile& b) {
                             auto rank = [](const py::PyFile& f) {
                                 std::string l = su::lower(f.filename);
                                 if (su::ends_with(l, ".exe")) return 0;
                                 if (su::ends_with(l, ".msi")) return 1;
                                 return 2;
                             };
                             return rank(a) < rank(b);
                         });
        for (const py::PyFile& f : py_ver_files)
            py_file_items.push_back(
                f.filename + "   " +
                (f.filesize > 0 ? su::human_size(f.filesize) : std::string("大小未知")) + "   " +
                f.arch);
    }
    void py_file_accept() {
        if (py_file_sel < 0 || py_file_sel >= (int)py_ver_files.size()) return;
        py_file = py_ver_files[py_file_sel];
        logx::linef("用户选择 Python 文件: %s", py_file.filename.c_str());
        go(S_Path);
    }
    void start_py_work() {
        reset_work("正在下载 Python " + py_file.version);
        {
            std::lock_guard<std::mutex> lk(wmu);
            w_file = py_file.filename;
            if (py_repair) w_note = "修复模式：优先使用已下载的安装包";
            else if (cb_install) w_note = "下载后将自动安装/解压到目标目录";
        }
        w_kind = 2;
        set_work_steps({"下载", "哈希验证", "安装", "配置", "完成"}, {0, 1, 2, 2, 3, 4});
        int mh = py::parse_mirror_name(s.opt.mirror_raw);
        auto self = shared_from_this();
        py::PyFile f = py_file;
        fs::path dir = s.dir;
        fs::path ver_dir = dir / su::utf8_to_wide(py_ver); // 统一布局：<目录>\<版本>
        fs::path py_current = dir / L"current";
        fs::path archive = dir / su::utf8_to_wide(f.filename);
        bool repair = py_repair;
        bool do_install = cb_install;
        auto prog = [self](uint64_t done, uint64_t total) {
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - self->w_last_draw).count();
            if (dt >= 0.15 || (total > 0 && done >= total)) {
                double inst = dt > 0 ? (double)(done - self->w_last_bytes) / dt : 0;
                self->w_bps = self->w_bps == 0 ? inst : self->w_bps * 0.7 + inst * 0.3;
                self->w_last_bytes = done;
                self->w_last_draw = now;
                self->w_done = done;
                self->w_total = total;
                {
                    std::lock_guard<std::mutex> lk(self->wmu);
                    self->w_bar = ui::progress_line(done, total, self->w_bps);
                }
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
            }
        };
        auto cancelled = [self] { return self->w_cancel.load(); };
        w_thread = std::thread([self, f, dir, ver_dir, py_current, archive, repair, do_install,
                                mh, prog, cancelled] {
            WorkHookGuard hook_guard;
            (void)hook_guard;
            ::archive::set_hook(make_extract_hook(self));
            checksum::set_hash_progress(make_hash_progress(self)); // SHA-256 计算进度
            std::error_code ec;
            InstallOutcome out;
            fs::path dest = archive;
            out.dest = dest;
            std::string err;
            int used = -1;
            if (repair && fs::exists(dest, ec))
                logx::line("修复模式：使用本地已有安装包（跳过下载）");
            else if (!py::download(f, mh, dest, prog, used, err, cancelled)) {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_phase = err == "已取消" ? 3 : 2;
                self->w_err = err;
                self->w_finished = true;
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                return;
            }
            {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_step = 1;
                self->w_note = "正在计算哈希校验值…";
            }
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
            if (!py::verify(f, dest, err)) {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_phase = 2;
                self->w_err = err;
                self->w_finished = true;
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                return;
            }
            std::string l = su::lower(f.filename);
            if (do_install && su::ends_with(l, ".exe")) {
                {
                    std::lock_guard<std::mutex> lk(self->wmu);
                    self->w_step = 2;
                    self->w_note = "正在静默安装（per-user，TargetDir=" +
                                   su::wide_to_utf8(ver_dir.wstring()) + "）…";
                }
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                if (!py::install_exe(dest, ver_dir, false, err)) {
                    std::lock_guard<std::mutex> lk(self->wmu);
                    self->w_phase = 2;
                    self->w_err = err;
                    self->w_finished = true;
                    if (!self->exiting) self->screen.PostEvent(Event::Custom);
                    return;
                }
                std::string vout;
                if (py::verify_install(ver_dir, vout, err)) self->py_verify_line = vout;
            } else if (do_install && su::ends_with(l, ".zip")) {
                {
                    std::lock_guard<std::mutex> lk(self->wmu);
                    self->w_step = 2;
                    self->w_note = "正在解压…";
                }
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                if (!py::extract_zip(dest, ver_dir, err)) {
                    std::lock_guard<std::mutex> lk(self->wmu);
                    self->w_phase = 2;
                    self->w_err = err;
                    self->w_finished = true;
                    if (!self->exiting) self->screen.PostEvent(Event::Custom);
                    return;
                }
                std::string vout;
                if (py::verify_install(ver_dir, vout, err)) self->py_verify_line = vout;
            } else {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_note = "该文件类型不支持自动安装，已保留安装包";
            }
            // 多版本布局：python/current junction + PATH（current 与 Scripts）
            if (do_install || fs::exists(ver_dir / L"python.exe", ec)) {
                {
                    std::lock_guard<std::mutex> lk(self->wmu);
                    self->w_step = 3;
                    self->w_note = "正在配置（current junction / PATH）…";
                }
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                if (!platform::make_junction(py_current, ver_dir, err)) {
                    std::lock_guard<std::mutex> lk(self->wmu);
                    self->w_phase = 2;
                    self->w_err = err;
                    self->w_finished = true;
                    if (!self->exiting) self->screen.PostEvent(Event::Custom);
                    return;
                }
            }
            if (self->cb_path && !self->s.opt.no_path) {
                std::string perr;
                if (platform::add_path_dir(py_current, perr) &&
                    platform::add_path_dir(py_current / L"Scripts", perr))
                    self->s.path_added = true;
            }
            platform::managed_add_root("python", dir); // 记录受管安装（供检测/卸载）
            {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_outcome = out;
                self->w_phase = 1;
            }
            self->w_finished = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
        go(S_Work);
    }

    // ---------------- SDK 管理（Node/JDK/Go/.NET，经 Provider 接口） ----------------
    void begin_sdk_load() {
        if (!sdk_provider || sdk_loading) return;
        sdk_loading = true;
        sdk_load_done = false;
        sdk_versions_for = sdk_id;
        sdk_err.clear();
        auto self = shared_from_this();
        sdk_thread = std::thread([self] {
            std::vector<prov::VersionInfo> v;
            std::string err;
            bool ok = self->sdk_provider->list_versions(v, err);
            self->sdk_versions = std::move(v);
            self->sdk_err = ok ? std::string() : err;
            self->sdk_load_done = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
    }
    void finish_sdk_load() {
        if (sdk_thread.joinable()) sdk_thread.join();
        sdk_loading = false;
        if (!sdk_versions.empty()) refilter_sdk_versions();
        else sdk_screen_err = sdk_err;
        if (step == S_Installed) rebuild_inst_items(); // 快速更新项依赖最新版本号
    }
    void refilter_sdk_versions() {
        sdk_items.clear();
        sdk_map.clear();
        std::string q = su::lower(sdk_query);
        for (size_t i = 0; i < sdk_versions.size(); ++i) {
            std::string hay = su::lower(sdk_versions[i].id + " " + sdk_versions[i].tag_label +
                                        " " + sdk_versions[i].display);
            if (!q.empty() && hay.find(q) == std::string::npos) continue;
            sdk_map.push_back(i);
            sdk_items.push_back(sdk_versions[i].display + "     [" +
                                sdk_versions[i].tag_label + "]" +
                                (sdk_versions[i].date.empty()
                                     ? ""
                                     : "  " + sdk_versions[i].date));
        }
        if (sdk_sel >= (int)sdk_items.size()) sdk_sel = (int)sdk_items.size() - 1;
        if (sdk_sel < 0) sdk_sel = 0;
    }
    void sdk_ver_accept() {
        if (sdk_map.empty()) return;
        int pos = sdk_sel;
        if (pos < 0) pos = 0;
        if (pos >= (int)sdk_map.size()) pos = (int)sdk_map.size() - 1;
        const prov::VersionInfo& v = sdk_versions[sdk_map[pos]];
        logx::linef("用户选择 %s 版本: %s [%s]", sdk_id.c_str(), v.id.c_str(),
                    v.tag_label.c_str());
        std::string err;
        prov::Artifact f;
        if (!sdk_provider->resolve(v.id, f, err)) {
            sdk_screen_err = err;
            return;
        }
        sdk_file = f;
        go(S_Path);
    }

    // 本机 PATH 工具链检测（仅 node，后台执行，不阻塞 UI）。
    // npm/npx 随装自带只检测显示；corepack/pnpm/yarn 显示启用状态。
    void begin_node_tools_probe() {
        if (sdk_tools_probed) return;
        sdk_tools_probed = true;
        auto self = shared_from_this();
        sdk_tools_thread = std::thread([self] {
            std::error_code cec;
            fs::path wd = fs::current_path(cec); // 借 PATH 解析 .cmd，无需知道安装目录
            if (cec) wd = L".";                  // cwd 失效时退回当前目录占位
            std::vector<std::string> rows;
            std::string nv = nodetools::tool_version(wd, "node");
            rows.push_back(nv.empty() ? "  node 未检测到（本机 PATH）"
                                      : "  node " + nv + "（本机 PATH）");
            for (const std::string& l :
                 nodetools::format_tool_lines(nodetools::detect_tools(wd, false)))
                rows.push_back(l);
            self->sdk_tools_result = std::move(rows);
            self->sdk_tools_ready = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
    }
    void finish_node_tools_probe() {
        if (sdk_tools_thread.joinable()) sdk_tools_thread.join();
        sdk_tool_rows = std::move(sdk_tools_result);
        sdk_tools_result.clear();
    }
    void start_sdk_work() {
        if (!sdk_provider) return;
        reset_work("正在下载 " + sdk_provider->display() + " " + sdk_file.version);
        w_kind = 3;
        set_work_steps({"下载", "哈希验证", "解压", "配置", "完成"}, {0, 1, 2, 2, 3, 4});
        auto self = shared_from_this();
        prov::Artifact f = sdk_file;
        fs::path root = s.dir;
        bool add_path = cb_path;
        auto prog = [self](uint64_t done, uint64_t total) {
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - self->w_last_draw).count();
            if (dt >= 0.15 || (total > 0 && done >= total)) {
                double inst = dt > 0 ? (double)(done - self->w_last_bytes) / dt : 0;
                self->w_bps = self->w_bps == 0 ? inst : self->w_bps * 0.7 + inst * 0.3;
                self->w_last_bytes = done;
                self->w_last_draw = now;
                self->w_done = done;
                self->w_total = total;
                {
                    std::lock_guard<std::mutex> lk(self->wmu);
                    self->w_bar = ui::progress_line(done, total, self->w_bps);
                }
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
            }
        };
        auto cancelled = [self] { return self->w_cancel.load(); };
        bool is_node = sdk_id == "node";
        bool is_flutter = sdk_id == "flutter";
        bool want_pnpm = cb_pnpm && is_node;
        bool want_yarn = cb_yarn && is_node;
        bool corepack_on = cb_corepack && is_node;
        // 存储根目录在 UI 线程解析（相对路径/环境变量展开），默认 <root>\pnpm-repository
        fs::path storage =
            pnpm_base.empty()
                ? root / L"pnpm-repository"
                : resolve_dir(su::utf8_to_wide(pnpm_base));
        w_thread = std::thread([self, f, root, add_path, prog, cancelled, is_node, is_flutter,
                                want_pnpm, want_yarn, corepack_on, storage] {
            WorkHookGuard hook_guard;
            (void)hook_guard;
            ::archive::set_hook(make_extract_hook(self));
            prov::set_stage_hook(make_stage_hook(self));
            checksum::set_hash_progress(make_hash_progress(self)); // SHA-256/SHA-512 计算进度
            std::string err, vline;
            if (!prov::install_to_root(*self->sdk_provider, f, root, add_path, false, true,
                                       prog, cancelled, vline, err)) {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_phase = self->w_cancel.load() ? 3 : 2;
                self->w_err = err;
                self->w_finished = true;
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                return;
            }
            // Flutter：按所选镜像站写入 PUB_HOSTED_URL / FLUTTER_STORAGE_BASE_URL（官方源清除）
            if (is_flutter) {
                std::string env_note;
                for (const auto& ev : self->sdk_provider->mirror_env_vars()) {
                    std::string e2;
                    if (ev.second.empty()) {
                        platform::remove_user_env(ev.first, e2);
                        env_note += (env_note.empty() ? "" : "、") + ev.first + "已清除";
                    } else if (platform::set_user_env(ev.first, su::utf8_to_wide(ev.second), e2)) {
                        env_note += (env_note.empty() ? "" : "、") + ev.first + "=" + ev.second;
                    } else {
                        env_note += (env_note.empty() ? "" : "、") + ev.first + "设置失败";
                    }
                }
                if (!env_note.empty()) {
                    logx::line("镜像环境变量: " + env_note);
                    vline += (vline.empty() ? "" : "  /  ") + env_note;
                }
            }
            // Node.js 工具链：npm/npx 随装自带（仅检测显示）→ Corepack 开关 →
            // pnpm/yarn（经 Corepack，全局）→ 存储位置规范化 → 检测报告
            std::vector<std::string> tool_lines;
            if (is_node) {
                fs::path nd = root / su::utf8_to_wide(f.version); // 版本目录（node.exe 所在）
                {
                    std::lock_guard<std::mutex> lk(self->wmu);
                    self->w_note = "正在配置工具链（Corepack / pnpm / 存储位置）…";
                }
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                std::string tools_line;
                tool_lines = setup_node_toolchain(nd, corepack_on, want_pnpm, want_yarn,
                                                  storage, add_path, tools_line);
                if (!tools_line.empty()) vline += (vline.empty() ? "" : "  /  ") + tools_line;
            }
            {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->sdk_verify_line = vline;
                self->sdk_tool_lines = std::move(tool_lines);
                self->w_phase = 1;
            }
            self->w_finished = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
        go(S_Work);
    }

    // ---------------- 工件解析 ----------------
    bool resolve_artifact(dist::Artifact& art, std::string& filename, std::string& err) {
        if (s.have_manifest && s.format == "tar.gz") {
            if (!dist::resolve_from_manifest(s.toml, s.pkg, s.target, art, err)) {
                std::string ts;
                for (const std::string& t : toml::list_targets(s.toml, s.pkg))
                    ts += std::string("\n可用平台: ") + t;
                if (!ts.empty()) err += ts;
                return false;
            }
        } else {
            std::string date;
            if (s.have_manifest) {
                dist::Artifact base;
                std::string e2;
                if (dist::resolve_from_manifest(s.toml, "rust", s.target, base, e2))
                    date = base.date;
            }
            if (date.empty() && !s.ver_date.empty()) date = s.ver_date;
            std::string fname =
                "rust-" + s.ver + "-" + s.target + (s.format == "msi" ? ".msi" : ".tar.gz");
            if (date.empty() || !dist::resolve_legacy(s.ver, s.target, date, fname, art, err)) {
                if (err.empty()) err = "无法确定发布日期，无法定位下载文件";
                return false;
            }
        }
        dist::probe_size(art, s.manifest_mirror >= 0 ? s.manifest_mirror : 0);
        filename = art.rel_path;
        size_t slash = filename.rfind('/');
        if (slash != std::string::npos) filename = filename.substr(slash + 1);
        return true;
    }

    // ---------------- 工作线程 ----------------
    void reset_work(const std::string& title) {
        std::lock_guard<std::mutex> lk(wmu);
        w_title = title;
        w_file.clear();
        w_bar.clear();
        w_note.clear();
        w_err.clear();
        w_phase = 0;
        w_cancel = false;
        w_finished = false;
        w_done = 0;
        w_total = 0;
        w_bps = 0;
        w_last_bytes = 0;
        w_last_draw = std::chrono::steady_clock::now();
        w_steps.clear();
        w_stage_step.clear();
        w_step = -1;
    }

    // 设置步骤条与阶段映射；流程自第一步开始
    void set_work_steps(std::vector<std::string> steps, std::vector<int> stage_step) {
        std::lock_guard<std::mutex> lk(wmu);
        w_steps = std::move(steps);
        w_stage_step = std::move(stage_step);
        w_step = w_steps.empty() ? -1 : 0;
    }

    void start_download() {
        ensure_manifest(s);
        dist::Artifact art;
        std::string filename, err;
        if (!resolve_artifact(art, filename, err)) {
            confirm_err = err;
            return; // 留在确认页显示错误
        }
        confirm_err.clear();
        logx::linef("开始下载流程（镜像: %s，模式: %s）", mirror_mode_desc(s.mirror).c_str(),
                    s.repair ? "修复" : "全新下载");
        reset_work("正在下载 " + filename);
        {
            std::lock_guard<std::mutex> lk(wmu);
            w_file = filename;
            if (s.repair) w_note = "修复模式：优先使用已下载的安装包";
        }
        w_kind = 0;
        set_work_steps({"下载", "哈希验证", "解压", "完成"}, {0, 1, 2, 2, 2, 3});

        auto self = shared_from_this();
        auto prog = [self](uint64_t done, uint64_t total) {
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - self->w_last_draw).count();
            if (dt >= 0.15 || (total > 0 && done >= total)) {
                double inst = dt > 0 ? (double)(done - self->w_last_bytes) / dt : 0;
                self->w_bps = self->w_bps == 0 ? inst : self->w_bps * 0.7 + inst * 0.3;
                self->w_last_bytes = done;
                self->w_last_draw = now;
                self->w_done = done;
                self->w_total = total;
                {
                    std::lock_guard<std::mutex> lk(self->wmu);
                    self->w_bar = ui::progress_line(done, total, self->w_bps);
                }
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
            }
        };
        auto cancelled = [self] { return self->w_cancel.load(); };

        dist::Artifact artc = art;
        fs::path dir = s.dir;
        w_thread = std::thread([self, artc, dir, prog, cancelled] {
            WorkHookGuard hook_guard;
            (void)hook_guard;
            ::archive::set_hook(make_extract_hook(self));
            prov::set_stage_hook(make_stage_hook(self));
            checksum::set_hash_progress(make_hash_progress(self)); // SHA-256 计算进度
            InstallOutcome out;
            std::string err;
            int used = -1;
            if (!download_and_install(self->s.opt, artc, dir, used, prog, cancelled, out, err)) {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_phase = out.cancelled ? 3 : 2;
                self->w_err = err;
                self->w_finished = true;
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                return;
            }
            if (self->s.opt.no_install || !target_is_windows(self->s.target) ||
                self->s.format == "msi") {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_outcome = out;
                self->w_phase = 1;
                self->w_finished = true;
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                return;
            }
            {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_note = "正在解压安装到 " + w(dir) + " …";
            }
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
            std::string ierr;
            if (!install_and_verify(self->s.opt, dir, self->s.pkg, out, ierr)) {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_phase = 2;
                self->w_err = ierr;
                self->w_finished = true;
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                return;
            }
            {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_outcome = out;
                self->w_phase = 1;
            }
            self->w_finished = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
        go(S_Work);
    }

    void start_repair_local() {
        reset_work("正在修复（本地安装包，无需联网）");
        {
            std::lock_guard<std::mutex> lk(wmu);
            w_file = w(s.local_archive);
            w_note = "正在解压覆盖…";
        }
        w_kind = 1;
        set_work_steps({"解压", "完成"}, {0, 0, 0, 0, 0, 1});
        fs::path archive = s.local_archive;
        fs::path dir = s.dir;
        auto self = shared_from_this();
        w_thread = std::thread([self, archive, dir] {
            WorkHookGuard hook_guard;
            (void)hook_guard;
            ::archive::set_hook(make_extract_hook(self));
            prov::set_stage_hook(make_stage_hook(self));
            InstallOutcome out;
            out.dest = archive;
            std::string err;
            if (!install_and_verify(self->s.opt, dir, "rust", out, err)) {
                std::error_code ec;
                fs::remove(archive, ec); // 删除损坏的本地包，转下载修复
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_phase = 2;
                self->w_err = err + "\n已删除损坏的本地安装包，回车将转为下载修复";
                self->w_finished = true;
                if (!self->exiting) self->screen.PostEvent(Event::Custom);
                return;
            }
            {
                std::lock_guard<std::mutex> lk(self->wmu);
                self->w_outcome = out;
                self->w_phase = 1;
            }
            self->w_finished = true;
            if (!self->exiting) self->screen.PostEvent(Event::Custom);
        });
        go(S_Work);
    }

    // 工作结束后的状态转移（UI 线程调用，线程已 join）
    void work_finish_transition() {
        int ph = w_phase.load();
        logx::linef("工作流结束（结果: %s）", ph == 1 ? "成功" : ph == 3 ? "已取消" : "失败");
        if (w_kind == 4 || w_kind == 5 || w_kind == 6) { // 卸载（Python/SDK / Rust） / 切换当前版本
            if (ph == 1) {
                go(S_Done);
            } else {
                inst_confirm = false;
                back(); // 返回检测屏/检查更新屏
            }
            return;
        }
        if (w_kind == 3) { // SDK
            if (ph == 1) {
                s.path_added = cb_path || s.path_added;
                go(S_Done);
            } else {
                sdk_screen_err = w_err;
                back();
            }
            return;
        }
        if (w_kind == 2) { // Python
            if (ph == 1) {
                s.path_added = cb_path || s.path_added;
                go(S_Done);
            } else {
                py_screen_err = w_err;
                back(); // 返回确认页（下载断点已保留）
            }
            return;
        }
        if (ph == 1) {
            s.rustc_v = w_outcome.rustc_v;
            s.cargo_v = w_outcome.cargo_v;
            fs::path bin = s.dir / L"bin";
            std::error_code ec;
            if (cb_path && !s.opt.no_path && fs::exists(bin)) {
                wchar_t pathv[32768] = {};
                GetEnvironmentVariableW(L"PATH", pathv, 32768);
                if (su::path_list_contains(pathv, bin.wstring())) s.path_added = true;
                else {
                    std::string perr;
                    if (dist::add_to_user_path(bin, perr)) s.path_added = true;
                    else s.last_err = perr;
                }
            } else {
                s.path_added = false;
            }
            if (cb_keep) fs::remove(w_outcome.dest, ec);
            go(S_Done);
            return;
        }
        // 失败 / 取消：带回错误信息
        if (w_kind == 1) { // 本地修复失败 → 自动配置后转下载修复
            auto_config(true);
            confirm_err = w_err;
            go(S_Confirm);
            return;
        }
        confirm_err = w_err;
        back();
    }

    // 完成页文案
    std::vector<std::string> done_lines() {
        std::vector<std::string> v;
        if (w_kind == 4 || w_kind == 5 || w_kind == 6) return uninstall_lines; // 卸载/切换结果
        if (!sdk_id.empty() && sdk_provider) {
            if (sdk_provider->multi_version()) { // 多版本布局：<root>\<版本> + current
                v.push_back("√ " + sdk_provider->display() + " " + sdk_file.version +
                            " 就绪: " + w(s.dir / L"current") + "（→ " +
                            w(s.dir / su::utf8_to_wide(sdk_file.version)) + "）");
            } else { // 单版本平铺：程序文件直接位于根目录
                v.push_back("√ " + sdk_provider->display() + " " + sdk_file.version +
                            " 就绪: " + w(s.dir) + "（单版本平铺布局）");
            }
            if (!sdk_verify_line.empty()) v.push_back(sdk_verify_line);
            for (const std::string& l : sdk_tool_lines) v.push_back(l);
            if (s.path_added) v.push_back("√ 已加入 PATH/环境变量（重新打开终端后生效）");
            return v;
        }
        if (pymode) {
            v.push_back("√ Python 就绪: " + w(s.dir / L"current") + "（→ " +
                        w(s.dir / su::utf8_to_wide(py_ver)) + "）");
            if (!py_verify_line.empty()) v.push_back(py_verify_line);
            if (s.path_added) v.push_back("√ 已加入用户 PATH（重新打开终端后生效）");
            v.push_back("python.exe 位于 " + w(s.dir / su::utf8_to_wide(py_ver)));
            return v;
        }
        v.push_back("√ " + std::string(s.format == "msi" ? "下载完成" : "安装完成") + ": " +
                    w(s.dir));
        if (!s.rustc_v.empty()) v.push_back(s.rustc_v);
        if (!s.cargo_v.empty()) v.push_back(s.cargo_v);
        if (s.path_added) v.push_back("√ 已将 bin 加入用户 PATH（重新打开终端后生效）");
        else v.push_back("提示: 使用前请将 " + w(s.dir / L"bin") + " 加入 PATH");
        if (s.format == "msi")
            v.push_back("MSI 安装包已就绪，运行: msiexec /i \"" + w(w_outcome.dest) + "\"");
        return v;
    }

    // 完成页 Esc：重置选择，回到检测屏/路径屏
    void done_reset() {
        logx::line("用户返回主界面，重置选择");
        // 安装/卸载/切换后本机状态已变化 → 后台重新检测全部已装语言（刷新首页摘要）
        begin_hub_detect();
        if (w_kind == 4 || w_kind == 5 || w_kind == 6) { // 卸载/切换完成 → 回到选择管理目标
            hist.clear();
            sdk_id.clear();
            sdk_provider.reset();
            pymode = false;
            enter(S_Choose);
            return;
        }
        if (!sdk_id.empty()) {
            sdk_query.clear();
            sdk_verify_line.clear();
            sdk_tool_lines.clear();
            sdk_file = prov::Artifact{};
            refilter_sdk_versions();
            enter(S_Installed); // 回到已装检测（含最新状态）
            return;
        }
        if (pymode) {
            py_ver.clear();
            py_file = py::PyFile{};
            py_query.clear();
            py_verify_line.clear();
            enter(S_Installed);
            return;
        }
        if (s.opt.version.empty()) {
            s.ver_set = false;
            s.ver.clear();
        }
        s.dir_set = s.opt.path_given;
        s.target_set = !s.opt.target.empty();
        s.repair = false;
        s.local_archive.clear();
        s.rustc_v.clear();
        s.cargo_v.clear();
        s.path_added = false;
        s.invalidate_manifest();
        ver_loaded = false;
        enter(S_Path);
    }
};

// W/S 键映射为菜单上下移动（方向键由 FTXUI 原生支持）
static Component with_wasd(Component c) {
    Component inner = c;
    return CatchEvent(std::move(c), [inner](Event e) {
        if (!e.is_character()) return false;
        const std::string& ch = e.character();
        if (ch == "w" || ch == "W") return inner->OnEvent(Event::ArrowUp);
        if (ch == "s" || ch == "S") return inner->OnEvent(Event::ArrowDown);
        return false;
    });
}

static Component make_menu(std::vector<std::string>* entries, int* sel,
                           std::function<void()> on_enter) {
    MenuOption o = MenuOption::Vertical();
    o.on_enter = std::move(on_enter);
    return with_wasd(Menu(entries, sel, o));
}

// 自绘可视窗口起始下标（保证选中项始终可见，长列表可滚动）
static int vis_window_start(int n, int sel, int vis) {
    int start = 0;
    if (n > vis) {
        start = std::min(sel - vis / 2, n - vis);
        if (start < 0) start = 0;
    }
    return start;
}

// 列表通用导航（↑↓/W S/Home/End/PageUp/PageDown）；n 为条目数，命中返回 true。
// 不依赖 FTXUI Menu 焦点语义，由路由层直接驱动（修复部分页面无法上下移动的问题）
static bool list_nav_event(const Event& e, int n, int& sel) {
    if (e == Event::ArrowUp) {
        if (sel > 0) sel--;
        return true;
    }
    if (e == Event::ArrowDown) {
        if (sel < n - 1) sel++;
        return true;
    }
    if (e == Event::Home) {
        sel = 0;
        return true;
    }
    if (e == Event::End) {
        if (n > 0) sel = n - 1;
        return true;
    }
    if (e == Event::PageUp) {
        sel = std::max(0, sel - 10);
        return true;
    }
    if (e == Event::PageDown) {
        if (n > 0) sel = std::min(n - 1, sel + 10);
        return true;
    }
    if (e.is_character()) {
        const std::string& ch = e.character();
        if (ch == "w" || ch == "W") {
            if (sel > 0) sel--;
            return true;
        }
        if (ch == "s" || ch == "S") {
            if (sel < n - 1) sel++;
            return true;
        }
    }
    return false;
}

static Component build_app(std::shared_ptr<Session> sp_s, ScreenInteractive& screen) {
    auto st = std::make_shared<AppState>(sp_s, screen);
    Session& s = *sp_s;

    st->py_installed = py::detect_installed();

    // CLI 预设（TUI 模式下同样生效）：Node.js 工具链选项与存储位置
    if (s.opt.sdk == "node") {
        if (s.opt.corepack_mode == "disable") st->cb_corepack = false;
        if (s.opt.with_yarn) st->cb_yarn = true;
        if (s.opt.with_pnpm || s.opt.with_yarn) st->cb_corepack = true; // 依赖 Corepack
        if (!s.opt.pnpm_home.empty()) st->pnpm_base = s.opt.pnpm_home;
    }

    // 检测 rustup 托管
    if (s.rust_ok) {
        wchar_t up[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH * 2)) {
            fs::path cargo_bin = fs::path(up) / L".cargo" / L"bin";
            st->rustup_managed = su::lower(s.rustc_exe.parent_path().wstring()) ==
                                     su::lower(cargo_bin.wstring()) &&
                                 fs::exists(fs::path(up) / L".rustup");
        }
    }

    // ---- 检查更新 ----
    auto uc_menu = make_menu(&st->uc_items, &st->uc_sel, [st] { st->uc_accept(); });
    Component uc_screen = Renderer(uc_menu, [st, uc_menu]() -> Element {
        Element v = vbox({text(st->uc_title) | bold, text(""), uc_menu->Render()});
        if (st->rustup_managed)
            v = vbox({text("⚠ 该 Rust 由 rustup 管理，继续将覆盖 rustup 的 shim（建议改用 rustup update）") |
                          color(Color::Yellow),
                      text(""),
                      v});
        return window(text("检查更新"), v);
    });

    // ---- 路径 ----
    InputOption pio;
    pio.content = &s.path_input;
    pio.on_enter = [st] { st->path_accept(); };
    Component path_inp = Input(&s.path_input, pio);
    Component path_screen = Renderer(path_inp, [st, path_inp]() -> Element {
        Element v = vbox({
            text("下载/安装目录（直接回车使用预填值）："),
            text(""),
            hbox({text("  > "), path_inp->Render() | flex}),
            text(""),
            st->path_err.empty() ? text("") : text(st->path_err) | color(Color::Red),
        });
        return window(text("安装路径"), v);
    });

    // ---- 版本（即时搜索 + 自绘列表窗口） ----
    Component ver_screen = Renderer([st]() -> Element {
        Elements v;
        if (st->ver_loading && !st->ver_loaded) {
            v.push_back(text("正在加载完整版本列表（GitHub 分页拉取中）…") | bold);
            v.push_back(text(""));
            v.push_back(text("已显示最新版本，可按 Esc 返回，加载完成后自动刷新") | dim);
            return window(text("选择版本"), vbox(std::move(v)));
        }
        if (!st->ver_err.empty()) {
            v.push_back(text(st->ver_err) | color(Color::Red));
            v.push_back(text("（按 Esc 返回）") | dim);
            return window(text("选择版本"), vbox(std::move(v)));
        }
        // 搜索框（直接打字过滤，无需先聚焦）
        v.push_back(hbox({text(" 搜索: ") | bold,
                          text(st->ver_query + "█") | color(Color::Cyan)}));
        v.push_back(text(""));
        // 自绘可视窗口，保证选中项始终可见
        int n = (int)st->ver_items.size();
        int vis = std::min(n, 14);
        int start = 0;
        if (n > vis) {
            start = std::min(st->ver_sel - vis / 2, n - vis);
            if (start < 0) start = 0;
        }
        for (int i = start; i < std::min(n, start + vis); ++i) {
            bool selq = i == st->ver_sel;
            bool pinned = i == 0 && st->ver_query.empty();
            std::string label = (selq ? "▶ " : "   ") + st->ver_items[i];
            Element row = text(label);
            if (selq) row = row | bold | color(Color::Green);
            else if (pinned) row = row | color(Color::Yellow);
            v.push_back(row);
        }
        v.push_back(text(""));
        std::string cnt = "匹配 " + std::to_string(n) + " / 共 " +
                          std::to_string(st->s.rp.all.size()) + " 个版本";
        if (n > vis)
            cnt += "  （显示 " + std::to_string(start + 1) + "-" +
                   std::to_string(std::min(n, start + vis)) + "）";
        v.push_back(text(cnt) | dim);
        return window(text("选择版本"), vbox(std::move(v)));
    });

    // ---- 目标平台 ----
    auto tg_menu = make_menu(&st->tg_items, &st->tg_sel, [st] { st->target_accept(); });
    Component tg_screen = Renderer(tg_menu, [st, tg_menu]() -> Element {
        return window(text("选择目标平台（Rust 编译目标）"), tg_menu->Render());
    });

    // ---- 自定义三元组 ----
    InputOption cio;
    cio.content = &s.path_input2;
    cio.on_enter = [st] { st->custom_accept(); };
    Component custom_inp = Input(&s.path_input2, cio);
    Component custom_screen = Renderer(custom_inp, [st, custom_inp]() -> Element {
        Element v = vbox({
            text("目标三元组（如 aarch64-pc-windows-gnu）："),
            text(""),
            hbox({text("  > "), custom_inp->Render() | flex}),
            text(""),
            st->custom_err.empty() ? text("") : text(st->custom_err) | color(Color::Red),
        });
        return window(text("自定义三元组"), v);
    });

    // ---- 组件包 ----
    auto pk_menu = make_menu(&st->pk_items, &st->pk_sel, [st] { st->package_accept(); });
    Component pk_screen = Renderer(pk_menu, [st, pk_menu]() -> Element {
        Elements v;
        v.push_back(st->pk_err.empty() ? text("选择要安装的组件包：")
                                       : text(st->pk_err) | color(Color::Red));
        if (!st->pk_note.empty()) v.push_back(text(st->pk_note) | dim);
        v.push_back(text(""));
        if (!st->pk_err.empty()) v.push_back(text("（Enter/Esc 返回上一步）") | dim);
        else v.push_back(pk_menu->Render());
        return window(text("选择组件包"), vbox(std::move(v)));
    });

    // ---- 包格式 ----
    auto fm_menu = make_menu(&st->fm_items, &st->fm_sel, [st] { st->format_accept(); });
    Component fm_screen = Renderer(fm_menu, [st, fm_menu]() -> Element {
        return window(text("选择安装包格式"), fm_menu->Render());
    });

    // ---- 确认 ----
    Component confirm_screen = Renderer([st]() -> Element {
        Element p_line = st->cb_path
                             ? text("[√] 安装后加入用户 PATH   （P 切换）") | color(Color::Green)
                             : text("[ ] 安装后加入用户 PATH   （P 切换）");
        Element k_line = st->cb_keep
                             ? text("[√] 安装后保留压缩包   （K 切换）") | color(Color::Green)
                             : text("[ ] 安装后保留压缩包   （K 切换）");
        Element v = vbox({
            text("版本     : " + st->s.ver +
                 (st->s.ver_date.empty() ? "" : "  (" + st->s.ver_date + ")")),
            text("平台     : " + st->s.target),
            text("组件包   : " + st->s.pkg + (st->s.pkg == "rust" ? "  （完整工具链）" : "")),
            text("格式     : " + st->s.format),
            text("镜像     : " + mirror_mode_desc(st->s.mirror)),
            text(std::string("模式     : ") +
                 (st->s.repair ? "修复（优先使用已下载的安装包，缺失时自动下载）"
                               : "全新下载安装")),
            text("目标目录 : " + w(st->s.dir)),
            text(""),
            p_line,
            k_line,
            text(""),
            st->confirm_err.empty() ? text("") : text(st->confirm_err) | color(Color::Red),
        });
        return window(text("确认安装信息"), v);
    });

    // ---- 工作屏（下载） ----
    Component work_screen = Renderer([st]() -> Element {
        std::lock_guard<std::mutex> lk(st->wmu);
        Elements v;
        v.push_back(text(st->w_file.empty() ? "…" : st->w_file) | bold);
        v.push_back(text(""));
        // 步骤条：√ 已完成（绿）· ▶ 当前（青·加粗）· 待办（灰）；成功时全部置 √
        if (!st->w_steps.empty()) {
            int cur = st->w_step.load();
            if (st->w_phase == 1) cur = (int)st->w_steps.size();
            Elements strip;
            for (size_t i = 0; i < st->w_steps.size(); ++i) {
                if ((int)i < cur)
                    strip.push_back(text("√ " + st->w_steps[i]) | color(Color::Green));
                else if ((int)i == cur)
                    strip.push_back(text("▶ " + st->w_steps[i]) | bold | color(Color::Cyan));
                else
                    strip.push_back(text(st->w_steps[i]) | dim);
                if (i + 1 < st->w_steps.size()) strip.push_back(text(" · ") | dim);
            }
            v.push_back(hbox(std::move(strip)));
            v.push_back(text(""));
        }
        if (st->w_total > 0) {
            v.push_back(gauge((float)((double)st->w_done.load() / (double)st->w_total.load())) |
                        color(Color::Cyan));
            v.push_back(text(st->w_bar));
        } else if (!st->w_bar.empty()) {
            v.push_back(text(st->w_bar));
        }
        if (!st->w_note.empty()) v.push_back(text(st->w_note) | dim);
        v.push_back(text(""));
        if (st->w_phase == 0) {
            v.push_back(text("Esc 取消下载（保留断点，支持续传；解压阶段需等待完成）") | dim);
        } else if (st->w_phase == 1) {
            v.push_back(text("√ 完成！回车继续") | color(Color::Green) | bold);
        } else {
            v.push_back(text(st->w_err) | color(Color::Red));
            v.push_back(text(""));
            v.push_back(text("回车返回 · Esc 返回主界面") | dim);
        }
        return window(text(st->w_title), vbox(std::move(v)));
    });

    // ---- 修复屏（本地包直装） ----
    Component repair_screen = Renderer([st]() -> Element {
        std::lock_guard<std::mutex> lk(st->wmu);
        Elements v;
        v.push_back(text(st->w_file) | bold);
        v.push_back(text(""));
        v.push_back(text(st->w_note) | dim);
        v.push_back(text(""));
        if (st->w_phase == 0) v.push_back(text("无需联网，请稍候…") | dim);
        else if (st->w_phase == 1)
            v.push_back(text("√ 完成！回车继续") | color(Color::Green) | bold);
        else {
            v.push_back(text(st->w_err) | color(Color::Red));
            v.push_back(text(""));
            v.push_back(text("回车转为下载修复 · Esc 返回主界面") | dim);
        }
        return window(text(st->w_title), vbox(std::move(v)));
    });

    // ---- 完成页 ----
    Component done_screen = Renderer([st]() -> Element {
        Elements v;
        for (const std::string& line : st->done_lines())
            v.push_back(text(line));
        return window(text("安装结果"), vbox(std::move(v)));
    });

    // ---- 管理目标（首页） ----
    Component ch_menu_c = make_menu(&st->ch_items, &st->ch_sel, [st] {
        static const char* kSdkIds[] = {"", "", "node", "jdk", "go", "dotnet",
                                        "zig", "php", "ruby", "git", "flutter"};
        if (st->ch_sel == 0) {
            logx::line("用户选择: 管理 Rust");
            st->go((st->s.rust_ok && !st->s.ver_set) ? S_UpdateCheck : st->first_unset());
        } else if (st->ch_sel == 1) {
            st->pymode = true;
            logx::line("用户选择: 管理 Python");
            st->go(S_Installed); // 已安装检测（自动检测 + 显示位置 + 更新/卸载）
        } else {
            st->pymode = false;
            st->sdk_provider = prov::Registry::instance().create(kSdkIds[st->ch_sel]);
            if (!st->sdk_provider) return;
            st->sdk_id = st->sdk_provider->id();
            logx::linef("用户选择: 管理 %s", st->sdk_provider->display().c_str());
            st->go(S_Installed);
        }
    });
    Component choose_screen = Renderer(ch_menu_c, [st, ch_menu_c]() -> Element {
        Elements v;
        v.push_back(text("选择要管理的目标环境："));
        v.push_back(text(""));
        v.push_back(ch_menu_c->Render());
        // 已装语言摘要（检测完成后显示；无安装时不显示）
        if (st->hub_done.load() && !st->hub_langs.empty()) {
            v.push_back(text(""));
            v.push_back(text("已安装语言：") | bold);
            std::string line;
            size_t shown = 0;
            for (const auto& L : st->hub_langs) {
                if (shown >= 4) break;
                line += (shown ? " · " : "") + L.display + " " + L.rows[0].version;
                ++shown;
            }
            if (st->hub_langs.size() > 4)
                line += " · 共 " + std::to_string(st->hub_langs.size()) + " 种";
            v.push_back(text("  " + line) | color(Color::Green));
        }
        v.push_back(text(""));
        v.push_back(text(
            "←/→ 全部已装语言管理中心 · JDK/Python/Node.js 多版本 <安装目录>\\<版本> · 其余单版本平铺") |
            dim);
        return window(text("选择管理目标"), vbox(std::move(v)));
    });

    // ---- Python 版本（即时搜索 + 自绘列表窗口） ----
    Component pyver_screen = Renderer([st]() -> Element {
        Elements v;
        if (st->py_manifest_loading) {
            v.push_back(text("正在枚举 Python 版本（FTP 目录，结果缓存 6 小时）…") | bold);
            v.push_back(text(""));
            v.push_back(text("可按 Esc 返回") | dim);
            return window(text("Python 版本"), vbox(std::move(v)));
        }
        if (!st->py_screen_err.empty()) {
            v.push_back(text(st->py_screen_err) | color(Color::Red));
            v.push_back(text(""));
            v.push_back(text("（Enter 重试 · Esc 返回）") | dim);
            return window(text("Python 版本"), vbox(std::move(v)));
        }
        v.push_back(hbox({text(" 搜索: ") | bold,
                          text(st->py_query + "█") | color(Color::Cyan)}));
        v.push_back(text(""));
        int n = (int)st->py_ver_items.size();
        int vis = std::min(n, 14);
        int start = 0;
        if (n > vis) {
            start = std::min(st->py_ver_sel - vis / 2, n - vis);
            if (start < 0) start = 0;
        }
        for (int i = start; i < std::min(n, start + vis); ++i) {
            bool selq = i == st->py_ver_sel;
            std::string label = (selq ? "▶ " : "   ") + st->py_ver_items[i];
            Element row = text(label);
            if (selq) row = row | bold | color(Color::Green);
            v.push_back(row);
        }
        v.push_back(text(""));
        v.push_back(text("共 " + std::to_string(n) + " 个版本（FTP 主源）") | dim);
        return window(text("Python 版本"), vbox(std::move(v)));
    });

    // ---- Python 文件选择 ----
    auto pyfile_menu = make_menu(&st->py_file_items, &st->py_file_sel,
                                 [st] { st->py_file_accept(); });
    Component pyfile_screen = Renderer(pyfile_menu, [st, pyfile_menu]() -> Element {
        Elements v;
        v.push_back(text("Python " + st->py_ver + " —— 选择要下载的文件："));
        v.push_back(text(""));
        v.push_back(pyfile_menu->Render());
        return window(text("选择文件"), vbox(std::move(v)));
    });

    // ---- Python 确认 ----
    Component pyconfirm_screen = Renderer([st]() -> Element {
        Element i_line =
            st->cb_install
                ? text("[√] 下载后自动安装（exe 静默）/ 解压（zip）   （I 切换）") |
                      color(Color::Green)
                : text("[ ] 下载后自动安装（exe 静默）/ 解压（zip）   （I 切换）");
        Element p_line =
            st->cb_path ? text("[√] 安装后加入 PATH（exe: PrependPath）   （P 切换）") |
                              color(Color::Green)
                        : text("[ ] 安装后加入 PATH（exe: PrependPath）   （P 切换）");
        Element k_line = st->cb_keep ? text("[√] 安装后保留安装包   （K 切换）") |
                                           color(Color::Green)
                                     : text("[ ] 安装后保留安装包   （K 切换）");
        Element v = vbox({
            text("版本     : Python " + st->py_file.version +
                 (st->py_file.release_date.empty()
                      ? ""
                      : "  (" + st->py_file.release_date + ")")),
            text("文件     : " + st->py_file.filename),
            text("大小     : " + (st->py_file.filesize > 0
                                      ? su::human_size(st->py_file.filesize)
                                      : std::string("下载时确定"))),
            text("哈希验证 : " + std::string(!st->py_file.sha256.empty()
                                                 ? "SHA-256"
                                                 : (!st->py_file.md5.empty() ? "MD5"
                                                                             : "下载完整性"))),
            text("镜像     : " + py_mirror_mode_desc(
                                    py::parse_mirror_name(st->s.opt.mirror_raw))),
            text("模式     : " + std::string(st->cb_install ? "下载并安装/解压" : "仅下载")),
            text("目标目录 : " + w(st->s.dir)),
            text(""),
            i_line,
            p_line,
            k_line,
            text(""),
            st->py_screen_err.empty() ? text("") : text(st->py_screen_err) | color(Color::Red),
        });
        return window(text("确认安装信息"), v);
    });

    // ---- SDK 版本（即时搜索 + 自绘列表窗口 + 支持级别配色） ----
    auto level_color = [](prov::SupportLevel l) -> Color {
        switch (l) {
            case prov::SupportLevel::Lts: return Color::Green;
            case prov::SupportLevel::Sts: return Color::Blue;
            case prov::SupportLevel::Supported: return Color::Green;
            case prov::SupportLevel::Stable: return Color::Blue;
            case prov::SupportLevel::Current: return Color::Cyan;
            case prov::SupportLevel::Eol: return Color::GrayDark;
            case prov::SupportLevel::Preview: return Color::Yellow;
            default: return Color::White;
        }
    };
    Component sdkver_screen = Renderer([st, level_color]() -> Element {
        Elements v;
        std::string disp =
            st->sdk_provider ? st->sdk_provider->display() : st->sdk_id;
        if (st->sdk_loading) {
            v.push_back(text("正在获取 " + disp + " 版本列表…") | bold);
            v.push_back(text(""));
            v.push_back(text("可按 Esc 返回") | dim);
            return window(text(disp + " 版本"), vbox(std::move(v)));
        }
        if (!st->sdk_screen_err.empty()) {
            v.push_back(text(st->sdk_screen_err) | color(Color::Red));
            v.push_back(text(""));
            v.push_back(text("（Enter 重试 · Esc 返回）") | dim);
            return window(text(disp + " 版本"), vbox(std::move(v)));
        }
        v.push_back(hbox({text(" 搜索: ") | bold,
                          text(st->sdk_query + "█") | color(Color::Cyan)}));
        v.push_back(text(""));
        int n = (int)st->sdk_items.size();
        int vis = std::min(n, 14);
        int start = 0;
        if (n > vis) {
            start = std::min(st->sdk_sel - vis / 2, n - vis);
            if (start < 0) start = 0;
        }
        for (int i = start; i < std::min(n, start + vis); ++i) {
            bool selq = i == st->sdk_sel;
            std::string label = (selq ? "▶ " : "   ") + st->sdk_items[i];
            Element row = text(label);
            if (selq) row = row | bold | color(Color::Green);
            else {
                size_t idx = st->sdk_map[(size_t)i];
                row = row | color(level_color(st->sdk_versions[idx].level));
            }
            v.push_back(row);
        }
        v.push_back(text(""));
        v.push_back(text("共 " + std::to_string(n) + " 个版本") | dim);
        // 本机工具链检测（node）：npm/npx 随装自带只检测显示；pnpm/yarn 显示启用状态
        if (st->sdk_id == "node" && !st->sdk_tool_rows.empty()) {
            v.push_back(text(""));
            v.push_back(separator());
            v.push_back(text(" 本机工具链检测：") | bold);
            for (const std::string& l : st->sdk_tool_rows) v.push_back(text(l) | dim);
        }
        return window(text(disp + " 版本"), vbox(std::move(v)));
    });

    // ---- SDK 确认 ----
    // 存储位置输入（仅 node 显示）：空 = 默认 <安装目录>\pnpm-repository
    InputOption nio;
    nio.content = &st->pnpm_base;
    nio.placeholder = "默认: <安装目录>\\pnpm-repository";
    Component pnpm_inp = Input(&st->pnpm_base, nio);
    Component sdkconfirm_screen = Renderer(pnpm_inp, [st, pnpm_inp]() -> Element {
        Element p_line =
            st->cb_path ? text("[√] 写入 PATH 与环境变量   （P 切换）") | color(Color::Green)
                        : text("[ ] 写入 PATH 与环境变量   （P 切换）");
        bool multi = st->sdk_provider && st->sdk_provider->multi_version();
        std::wstring layout =
            multi ? st->s.dir.wstring() + L"\\<版本>，current junction 指向当前版本"
                  : st->s.dir.wstring() + L"（程序文件直接位于根目录，无 junction）";
        Elements rows = {
            text("目标     : " + (st->sdk_provider ? st->sdk_provider->display() : st->sdk_id)),
            text("版本     : " + st->sdk_file.version),
            text("文件     : " + st->sdk_file.filename),
            text("哈希验证 : " +
                 std::string(!st->sdk_file.sha256.empty()
                                 ? "SHA-256"
                                 : (!st->sdk_file.sha512.empty() ? "SHA-512"
                                                                 : "下载完整性"))),
            text(multi ? "模式     : 多版本目录 <安装目录>\\<版本> + current junction"
                       : "模式     : 单版本平铺目录（更新覆盖，卸载删除整个目录）"),
            text("根目录   : " + w(st->s.dir)),
            text("布局     : " + su::wide_to_utf8(layout)),
            text(""),
            p_line,
        };
        if (st->sdk_id == "node") {
            // Node.js 工具链优先级：npm/npx 随装自带 → Corepack 开关 → pnpm/yarn 经 Corepack
            rows.push_back(separator());
            rows.push_back(text(" Node.js 工具链: npm/npx 随装自带（装后自动检测显示版本）") | dim);
            rows.push_back(st->cb_corepack
                               ? text("[√] 启用 Corepack（Node 内置，管理 pnpm/yarn）（C 切换）") |
                                     color(Color::Green)
                               : text("[ ] 启用 Corepack（Node 内置，管理 pnpm/yarn）（C 切换）"));
            rows.push_back(st->cb_pnpm
                               ? text("[√] 安装 pnpm（经 Corepack，全局）           （N 切换）") |
                                     color(Color::Green)
                               : text("[ ] 安装 pnpm（经 Corepack，全局）           （N 切换）"));
            rows.push_back(st->cb_yarn
                               ? text("[√] 安装 yarn（经 Corepack，全局）           （Y 切换）") |
                                     color(Color::Green)
                               : text("[ ] 安装 yarn（经 Corepack，全局）           （Y 切换）"));
            rows.push_back(hbox({text(" 存储位置: "), pnpm_inp->Render() | flex}));
            rows.push_back(text("   pnpm global-dir/global-bin-dir/state-dir/cache-dir 与"
                                " npm prefix/cache 规范至此目录") |
                            dim);
        }
        if (st->sdk_id == "flutter" && st->sdk_provider) {
            // Flutter 下载镜像站：官方 / 清华 TUNA / 中科大 USTC / 中国社区旧镜像（M 键切换）
            std::vector<std::string> opts = st->sdk_provider->mirror_options();
            int sel = st->sdk_provider->mirror_selected();
            if (!opts.empty() && sel >= 0 && sel < (int)opts.size()) {
                rows.push_back(separator());
                rows.push_back(text(" 下载镜像 : " + opts[(size_t)sel] + "   （M 切换）") |
                               color(Color::Cyan));
                rows.push_back(text("   安装后设置 PUB_HOSTED_URL / FLUTTER_STORAGE_BASE_URL 镜像环境变量"
                                    "（官方源自动清除）") |
                                dim);
                rows.push_back(text("   所选镜像下载失败时自动回退其余镜像站") | dim);
            }
        }
        rows.push_back(text(""));
        rows.push_back(st->sdk_screen_err.empty() ? text("")
                                                  : text(st->sdk_screen_err) | color(Color::Red));
        return window(text("确认安装信息"), vbox(std::move(rows)));
    });

    // ---- 已安装检测（统一：Python / SDK，自动检测 + 显示位置 + 更新/卸载/切换） ----
    // 键位由路由层直接处理（↑↓/W S 滚动 + Enter + U/O 热键），此处仅自绘可视窗口
    Component inst_screen = Renderer([st]() -> Element {
        Elements v;
        std::string disp = st->inst_lang_disp();
        if (st->inst_detect_lang != st->inst_lang_id() || st->inst_items.empty()) {
            v.push_back(text("正在检测本机已安装的 " + disp + " …") | bold);
            v.push_back(text(""));
            v.push_back(text("（受管记录 · 注册表 · 环境变量 · PATH）") | dim);
            return window(text("已安装检测 · " + disp), vbox(std::move(v)));
        }
        std::vector<AppState::InstRow> rows;
        {
            std::lock_guard<std::mutex> lk(st->wmu);
            rows = st->inst_rows;
        }
        // 已安装列表（无安装时不显示该区块；超过 8 行折叠）
        if (!rows.empty()) {
            v.push_back(text("已安装列表（自动检测）：") | bold);
            int shown = 0;
            for (const auto& r : rows) {
                if (shown >= 8) break;
                std::string tag = r.managed ? "本工具管理" : "外部安装";
                std::string cur = r.is_current ? "  ← 当前版本" : "";
                v.push_back(text("  " + r.version + "    " + r.path + "  [" + tag + "]" + cur) |
                            (r.managed ? color(Color::Green) : color(Color::GrayLight)));
                ++shown;
            }
            if ((int)rows.size() > 8)
                v.push_back(text("  … 等 " + std::to_string(rows.size()) + " 个版本") | dim);
            v.push_back(text(""));
        }
        v.push_back(text("可执行操作："));
        int n = (int)st->inst_items.size();
        int vis = std::min(n, 12);
        int start = vis_window_start(n, st->inst_sel, vis);
        for (int i = start; i < std::min(n, start + vis); ++i) {
            bool selq = i == st->inst_sel;
            std::string label = (selq ? "▶ " : "   ") + st->inst_items[i];
            Element row = text(label);
            if (selq) row = row | bold | color(Color::Green);
            v.push_back(row);
        }
        if (n > vis)
            v.push_back(text(sfmt("  （%d-%d / 共 %d 项，↑↓ 滚动）", start + 1,
                                  std::min(n, start + vis), n)) |
                        dim);
        if (st->inst_confirm)
            v.push_back(text("⚠ 卸载将删除安装目录且不可恢复，再次 Enter 确认") |
                        color(Color::Yellow));
        v.push_back(text(""));
        v.push_back(text("O 打开目录 · U 重新检测 · Esc 返回") | dim);
        return window(text("已安装检测 · " + disp), vbox(std::move(v)));
    });

    // ---- 管理中心（全部已装语言；首页 ←/→ 进入） ----
    // 键位由路由层直接处理（↑↓/W S 滚动 + ←/→/Esc 返回 + Enter + P/C/U 热键）
    Component hub_screen = Renderer([st]() -> Element {
        Elements v;
        v.push_back(text("本机已安装语言（自动检测，Enter 进入该语言管理）：") | bold);
        v.push_back(text(""));
        if (st->hub_loading.load() && st->hub_items.empty()) {
            v.push_back(text("正在检测已安装语言（受管记录 · 注册表 · 环境变量 · PATH）…") | dim);
        } else if (st->hub_items.empty()) {
            // 无任何已安装语言：不显示空列表
            v.push_back(text("未检测到任何已安装语言。") | color(Color::Yellow));
            v.push_back(text("可按 Esc/←/→ 返回首页选择目标进行安装。") | dim);
        } else {
            int n = (int)st->hub_items.size();
            int vis = std::min(n, 10);
            int start = vis_window_start(n, st->hub_sel, vis);
            for (int i = start; i < std::min(n, start + vis); ++i) {
                bool selq = i == st->hub_sel;
                std::string label = (selq ? "▶ " : "   ") + st->hub_items[i];
                Element row = text(label);
                if (selq) row = row | bold | color(Color::Green);
                v.push_back(row);
            }
            if (n > vis)
                v.push_back(text(sfmt("  （%d-%d / 共 %d 项，↑↓ 滚动）", start + 1,
                                      std::min(n, start + vis), n)) |
                            dim);
            // 选中语言明细（版本 + 完整位置 + 管理来源；超过 6 行折叠）
            if (st->hub_sel >= 0 && st->hub_sel < (int)st->hub_map.size()) {
                const auto& L = st->hub_langs[st->hub_map[(size_t)st->hub_sel]];
                v.push_back(text(""));
                v.push_back(separator());
                v.push_back(text(" " + L.display + " 明细：") | bold);
                int shown = 0;
                for (const auto& r : L.rows) {
                    if (shown >= 6) break;
                    std::string tag = r.managed ? "本工具管理" : "外部安装";
                    std::string cur = r.is_current ? "  ← 当前版本" : "";
                    v.push_back(text("  " + r.version + "    " + r.path + "  [" + tag + "]" + cur) |
                                (r.managed ? color(Color::Green) : color(Color::GrayLight)));
                    ++shown;
                }
                if ((int)L.rows.size() > 6)
                    v.push_back(text("  … 等 " + std::to_string(L.rows.size()) + " 个版本") | dim);
            }
        }
        if (!st->hub_notes.empty()) {
            v.push_back(text(""));
            for (const std::string& n : st->hub_notes) v.push_back(text(n) | dim);
        }
        v.push_back(text(""));
        v.push_back(
            text("↑↓/W S 选择 · Enter 管理 · P 体检 PATH（清理失效项） · C 清理缓存 · U 重新检测 · ←/→ 返回") |
            dim);
        return window(text("全部已安装语言"), vbox(std::move(v)));
    });

    // ---- 屏幕容器（顺序与 StepIdx 一致，选择器即 Session::step）----
    std::vector<Component> screens;
    screens.push_back(uc_screen);        // S_UpdateCheck
    screens.push_back(repair_screen);    // S_RepairLocal
    screens.push_back(path_screen);      // S_Path
    screens.push_back(ver_screen);       // S_Version
    screens.push_back(tg_screen);        // S_Target
    screens.push_back(custom_screen);    // S_TargetCustom
    screens.push_back(pk_screen);        // S_Package
    screens.push_back(fm_screen);        // S_Format
    screens.push_back(confirm_screen);   // S_Confirm
    screens.push_back(work_screen);      // S_Work
    screens.push_back(done_screen);      // S_Done
    screens.push_back(choose_screen);    // S_Choose
    screens.push_back(pyver_screen);     // S_PyVersion
    screens.push_back(pyfile_screen);    // S_PyFile
    screens.push_back(pyconfirm_screen); // S_PyConfirm
    screens.push_back(sdkver_screen);    // S_SdkVersion
    screens.push_back(sdkconfirm_screen); // S_SdkConfirm
    screens.push_back(inst_screen);       // S_Installed
    screens.push_back(hub_screen);        // S_Hub
    Component tab = Container::Tab(screens, &st->step);

    // ---- 全局事件 ----
    Component router = CatchEvent(tab, [st](Event e) {
        if (e == Event::CtrlC) return true; // Ctrl+C：不退出（选中文字时由终端复制）
        // 本机工具链检测完成 → 任意事件时合并（防止循环启动前的 Custom 事件丢失）
        if (st->sdk_tools_ready.exchange(false)) st->finish_node_tools_probe();
        if (e == Event::Custom) {
            if (st->w_finished.exchange(false)) {
                if (st->w_thread.joinable()) st->w_thread.join();
            }
            if (st->ver_load_done.exchange(false)) st->finish_version_load();
            if (st->py_manifest_done.exchange(false)) st->finish_py_manifest();
            if (st->sdk_load_done.exchange(false)) st->finish_sdk_load();
            if (st->inst_done.exchange(false)) st->finish_inst_detect();
            if (st->hub_done.exchange(false)) st->finish_hub_detect();
            return true;
        }
        // 首页 ←/→：进入全部已装语言管理中心
        if (st->step == S_Choose && (e == Event::ArrowLeft || e == Event::ArrowRight)) {
            st->go(S_Hub);
            return true;
        }
        // 管理中心：↑↓/W S/Home/End/PgUp/PgDn 选择（路由层驱动，不再依赖 Menu 焦点）
        // ←/→/Esc 返回；Enter 管理；热键 P/C/U
        if (st->step == S_Hub) {
            if (list_nav_event(e, (int)st->hub_map.size(), st->hub_sel)) return true;
            if (e == Event::ArrowLeft || e == Event::ArrowRight || e == Event::Escape) {
                st->back();
                return true;
            }
            if (e == Event::Return) {
                st->hub_accept();
                return true;
            }
            if (e.is_character()) {
                const std::string& ch = e.character();
                if (ch == "p" || ch == "P") {
                    int total = 0;
                    std::string err;
                    int removed = platform::fix_user_path(total, err);
                    if (removed < 0)
                        st->hub_notes.push_back("PATH 体检失败：" + err);
                    else if (removed == 0)
                        st->hub_notes.push_back("PATH 体检：共 " + std::to_string(total) +
                                                " 项，无失效项");
                    else
                        st->hub_notes.push_back("PATH 体检：共 " + std::to_string(total) +
                                                " 项，已清理失效 " + std::to_string(removed) + " 项");
                    return true;
                }
                if (ch == "c" || ch == "C") {
                    uint64_t bytes = 0;
                    size_t files = 0;
                    if (clean_exe_cache(bytes, files))
                        st->hub_notes.push_back("缓存清理：删除 " + std::to_string(files) +
                                                " 个文件，释放 " + su::human_size(bytes));
                    else
                        st->hub_notes.push_back("缓存清理：无缓存可清理");
                    return true;
                }
                if (ch == "u" || ch == "U") {
                    st->begin_hub_detect(); // 重新检测
                    return true;
                }
            }
            return true; // 其余按键吞掉，避免误触下层组件
        }
        if (st->step == S_Installed) {
            // ↑↓/W S/Home/End/PgUp/PgDn 选择（路由层驱动）；Enter 确认；U/O 热键
            if (list_nav_event(e, (int)st->inst_items.size(), st->inst_sel)) return true;
            if (e == Event::Return) {
                st->inst_accept();
                return true;
            }
            if (e.is_character()) {
                const std::string& ch = e.character();
                if (ch == "u" || ch == "U") {
                    st->begin_inst_detect(); // 手动重新检测
                    return true;
                }
                if (ch == "o" || ch == "O") { // 打开目录热键：定位到“打开版本目录”项并执行
                    for (size_t i = 0; i < st->inst_actions.size(); ++i)
                        if (st->inst_actions[i].first == AppState::IA_OPEN) {
                            st->inst_sel = (int)i;
                            st->inst_accept();
                            break;
                        }
                    return true;
                }
            }
        }
        if (st->step == S_Version) {
            // 即时搜索：直接打字过滤；W/S 或 ↑↓ 选择；Enter 确认
            if (e.is_character()) {
                const std::string& ch = e.character();
                if (ch == "w" || ch == "W") {
                    if (st->ver_sel > 0) st->ver_sel--;
                    return true;
                }
                if (ch == "s" || ch == "S") {
                    if (st->ver_sel < (int)st->ver_items.size() - 1) st->ver_sel++;
                    return true;
                }
                st->ver_query += ch;
                st->refilter_versions();
                return true;
            }
            if (e == Event::Backspace) {
                std::string& q = st->ver_query;
                while (!q.empty() && ((unsigned char)q.back() & 0xC0) == 0x80) q.pop_back();
                if (!q.empty()) q.pop_back();
                st->refilter_versions();
                return true;
            }
            if (e == Event::ArrowUp) {
                if (st->ver_sel > 0) st->ver_sel--;
                return true;
            }
            if (e == Event::ArrowDown) {
                if (st->ver_sel < (int)st->ver_items.size() - 1) st->ver_sel++;
                return true;
            }
            if (e == Event::Home) {
                st->ver_sel = 0;
                return true;
            }
            if (e == Event::End) {
                st->ver_sel = (int)st->ver_items.size() - 1;
                return true;
            }
            if (e == Event::Return) {
                st->ver_accept();
                return true;
            }
            if (e == Event::Tab) return true;
            if (e == Event::Escape) {
                st->back();
                return true;
            }
            return false;
        }
        if (st->step == S_PyVersion) {
            if (e.is_character()) {
                const std::string& ch = e.character();
                if (ch == "w" || ch == "W") {
                    if (st->py_ver_sel > 0) st->py_ver_sel--;
                    return true;
                }
                if (ch == "s" || ch == "S") {
                    if (st->py_ver_sel < (int)st->py_ver_items.size() - 1) st->py_ver_sel++;
                    return true;
                }
                st->py_query += ch;
                st->refilter_py_versions();
                return true;
            }
            if (e == Event::Backspace) {
                std::string& q = st->py_query;
                while (!q.empty() && ((unsigned char)q.back() & 0xC0) == 0x80) q.pop_back();
                if (!q.empty()) q.pop_back();
                st->refilter_py_versions();
                return true;
            }
            if (e == Event::ArrowUp) {
                if (st->py_ver_sel > 0) st->py_ver_sel--;
                return true;
            }
            if (e == Event::ArrowDown) {
                if (st->py_ver_sel < (int)st->py_ver_items.size() - 1) st->py_ver_sel++;
                return true;
            }
            if (e == Event::Home) {
                st->py_ver_sel = 0;
                return true;
            }
            if (e == Event::End) {
                st->py_ver_sel = (int)st->py_ver_items.size() - 1;
                return true;
            }
            if (e == Event::Return) {
                if (st->py_ver_map.empty() && !st->py_manifest_ok && !st->py_manifest_loading)
                    st->begin_py_manifest(); // 重试
                else
                    st->py_ver_accept();
                return true;
            }
            if (e == Event::Tab) return true;
            if (e == Event::Escape) {
                st->back();
                return true;
            }
            return false;
        }
        if (st->step == S_SdkVersion) {
            if (e.is_character()) {
                const std::string& ch = e.character();
                if (ch == "w" || ch == "W") {
                    if (st->sdk_sel > 0) st->sdk_sel--;
                    return true;
                }
                if (ch == "s" || ch == "S") {
                    if (st->sdk_sel < (int)st->sdk_items.size() - 1) st->sdk_sel++;
                    return true;
                }
                st->sdk_query += ch;
                st->refilter_sdk_versions();
                return true;
            }
            if (e == Event::Backspace) {
                std::string& q = st->sdk_query;
                while (!q.empty() && ((unsigned char)q.back() & 0xC0) == 0x80) q.pop_back();
                if (!q.empty()) q.pop_back();
                st->refilter_sdk_versions();
                return true;
            }
            if (e == Event::ArrowUp) {
                if (st->sdk_sel > 0) st->sdk_sel--;
                return true;
            }
            if (e == Event::ArrowDown) {
                if (st->sdk_sel < (int)st->sdk_items.size() - 1) st->sdk_sel++;
                return true;
            }
            if (e == Event::Home) {
                st->sdk_sel = 0;
                return true;
            }
            if (e == Event::End) {
                st->sdk_sel = (int)st->sdk_items.size() - 1;
                return true;
            }
            if (e == Event::Return) {
                if (st->sdk_map.empty() && !st->sdk_loading) st->begin_sdk_load(); // 重试
                else st->sdk_ver_accept();
                return true;
            }
            if (e == Event::Tab) return true;
            if (e == Event::Escape) {
                st->back();
                return true;
            }
            return false;
        }
        if (e == Event::Escape) {
            if (st->step == S_Work) {
                if (st->w_phase == 0) st->w_cancel = true;
                else st->work_finish_transition();
                return true;
            }
            if (st->step == S_Done) {
                st->done_reset();
                return true;
            }
            st->back();
            return true;
        }
        if (e == Event::Return) {
            if (st->step == S_Work) {
                if (st->w_phase != 0) st->work_finish_transition();
                return true;
            }
            if (st->step == S_Done) {
                st->screen.ExitLoopClosure()();
                return true;
            }
            if (st->step == S_Confirm) {
                st->start_download();
                return true;
            }
            if (st->step == S_Package && !st->pk_err.empty()) {
                st->back();
                return true;
            }
            if (st->step == S_PyConfirm) {
                st->start_py_work();
                return true;
            }
            if (st->step == S_PyVersion && !st->py_manifest_ok && !st->py_manifest_loading) {
                st->begin_py_manifest(); // 重试
                return true;
            }
            if (st->step == S_SdkConfirm) {
                st->start_sdk_work();
                return true;
            }
            if (st->step == S_SdkVersion && !st->sdk_loading && st->sdk_versions.empty() &&
                st->sdk_provider) {
                st->begin_sdk_load(); // 重试
                return true;
            }
            return false;
        }
        if (e.is_character()) {
            const std::string& ch = e.character();
            if (st->step == S_PyConfirm && (ch == "i" || ch == "I")) {
                st->cb_install = !st->cb_install;
                return true;
            }
            if (st->step == S_PyConfirm && (ch == "p" || ch == "P")) {
                st->cb_path = !st->cb_path;
                return true;
            }
            if (st->step == S_PyConfirm && (ch == "k" || ch == "K")) {
                st->cb_keep = !st->cb_keep;
                return true;
            }
            if (st->step == S_SdkConfirm && (ch == "p" || ch == "P")) {
                st->cb_path = !st->cb_path;
                return true;
            }
            if (st->step == S_SdkConfirm && st->sdk_id == "node") {
                // 工具链开关联动：Corepack 关闭 → pnpm/yarn 一并取消；
                // 选装 pnpm/yarn → 自动启用 Corepack（二者经其管理）
                if (ch == "c" || ch == "C") {
                    st->cb_corepack = !st->cb_corepack;
                    if (!st->cb_corepack) {
                        st->cb_pnpm = false;
                        st->cb_yarn = false;
                    }
                    return true;
                }
                if (ch == "n" || ch == "N") {
                    st->cb_pnpm = !st->cb_pnpm;
                    if (st->cb_pnpm) st->cb_corepack = true;
                    return true;
                }
                if (ch == "y" || ch == "Y") {
                    st->cb_yarn = !st->cb_yarn;
                    if (st->cb_yarn) st->cb_corepack = true;
                    return true;
                }
            }
            if (st->step == S_SdkConfirm && (ch == "m" || ch == "M") &&
                st->sdk_id == "flutter" && st->sdk_provider) {
                // 循环切换下载镜像站（官方 → TUNA → USTC → 社区旧镜像），选择持久化
                std::vector<std::string> opts = st->sdk_provider->mirror_options();
                if (!opts.empty())
                    st->sdk_provider->set_mirror_selected(
                        (st->sdk_provider->mirror_selected() + 1) % (int)opts.size());
                return true;
            }
            if (st->step == S_Confirm && (ch == "m" || ch == "M")) {
                st->s.mirror = st->s.mirror >= 2 ? -1 : st->s.mirror + 1;
                return true;
            }
            if (st->step == S_Confirm && (ch == "p" || ch == "P")) {
                st->cb_path = !st->cb_path;
                return true;
            }
            if (st->step == S_Confirm && (ch == "k" || ch == "K")) {
                st->cb_keep = !st->cb_keep;
                return true;
            }
        }
        return false;
    });

    // ---- 布局 ----
    Component layout = Renderer(router, [st, router]() -> Element {
        Element head = window(
            text(" Environ Manage（环境管理器）   By:Ming-QWQ520(明) "),
            hbox({text(" 多版本: JDK/Python/Node.js · 其余单版本平铺 · Ctrl+C 复制选中文字 ") | dim,
                  filler(),
                  text(std::string(step_tag(st->step)) + " ") | dim}));
        Element foot =
            hbox({text(std::string(" ") + step_tag(st->step) + "  " + step_hints(st->step)) | dim,
                  filler()});
        return vbox({std::move(head), separator(), router->Render() | yframe | flex,
                     separator(), std::move(foot)});
    });

    // 初始屏幕：--sdk / --python → 已装检测；否则进入管理目标选择
    if (!s.opt.sdk.empty()) {
        st->sdk_provider = prov::Registry::instance().create(s.opt.sdk);
        st->sdk_id = s.opt.sdk;
        st->pymode = false;
        st->go(S_Installed);
    } else if (s.opt.python) {
        st->pymode = true;
        st->go(S_Installed);
    } else {
        st->enter(S_Choose);
    }

    // 启动即后台检测全部已装语言（首页摘要与管理中心共用结果）
    if (st->hub_loading.load() == false && st->hub_done.load() == false)
        st->begin_hub_detect();

    return layout;
}

inline int run_tui(const Options& opts) {
    Session s(opts);

    // 版本列表仅 Rust 流程需要：拉取失败不再阻断 TUI（进入界面后相关功能可重试）
    std::string err;
    if (!s.rp.ensure(1, err)) {
        logx::line("启动时获取 Rust 版本列表失败（相关功能进入界面后可重试）: " + err);
        s.rp.all.clear();
        s.rp.end_of_list = false;
    }

    s.rustc_exe = find_installed_rust(s.installed_line);
    s.rust_ok = !s.rustc_exe.empty();

    // 命令行预设
    if (!s.opt.version.empty()) {
        std::string want = s.opt.version;
        if (su::lower(want) == "latest" || want == "最新") {
            s.ver = s.rp.all[0].tag;
            s.ver_date = s.rp.all[0].date;
            s.ver_set = true;
        } else {
            s.ver = want;
            for (const github::Release& r : s.rp.all)
                if (r.tag == want) {
                    s.ver_date = r.date;
                    s.ver_set = true;
                    break;
                }
            if (!s.ver_set && s.rp.ensure((size_t)-1, err))
                for (const github::Release& r : s.rp.all)
                    if (r.tag == want) {
                        s.ver_date = r.date;
                        s.ver_set = true;
                        break;
                    }
            if (!s.ver_set) s.ver_set = true; // 按输入值继续尝试
        }
    }
    if (s.opt.path_given) {
        s.dir = resolve_dir(s.opt.path);
        s.dir_set = true;
        s.path_input = w(s.dir);
        logx::linef("目标目录: %s", w(s.dir));
    }
    if (!s.opt.target.empty()) {
        s.target = s.opt.target;
        s.target_set = true;
    }
    s.format_set = s.opt.format != "tar.gz";

    auto screen = ScreenInteractive::Fullscreen();
    auto sp_s = std::make_shared<Session>(std::move(s));
    Component app = build_app(sp_s, screen);
    // 忽略 Ctrl+C 控制台事件：未选中文字时不再直接退出，选中文字时由终端完成复制
    SetConsoleCtrlHandler(nullptr, TRUE);
    screen.Loop(app);
    SetConsoleCtrlHandler(nullptr, FALSE);
    return 0;
}

} // namespace tui


// --------------------------------------------------------------- 入口

int main() {
    ui::init();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) argc = 0;
    int rc;
    try {
        bool args_ok = true;
        Options opts = parse_args(argc, argv, args_ok);
        if (opts.log_given) logx::set_file(resolve_dir(opts.log_path));
        else logx::set_file(exe_log_file());
        std::string target = opts.python ? "Python"
                                         : (!opts.sdk.empty() ? "SDK:" + opts.sdk
                                                              : "Rust");
        std::string listmode = (opts.list || opts.py_list || opts.sdk_list) ? "版本列表"
                                                                            : "";
        logx::linef("RustInstall 会话开始（目标: %s，模式: %s）", target.c_str(),
                    listmode.empty() ? (interactive() ? "TUI" : "批处理") : listmode.c_str());
        logx::line("命令行: " + su::wide_to_utf8(GetCommandLineW()));
        if (!args_ok) {
            printf("\n");
            print_usage();
            rc = opts.list ? 0 : 2;
        } else if (opts.list) {
            rc = run_list();
        } else if (opts.py_list) {
            rc = run_py_list();
        } else if (opts.sdk_list) {
            rc = run_sdk_list(opts);
        } else if (opts.uninstall && !interactive()) {
            rc = run_uninstall_batch(opts);
        } else if (opts.python && !interactive()) {
            rc = run_py_batch(opts);
        } else if (!opts.sdk.empty() && !interactive()) {
            rc = run_sdk_batch(opts);
        } else if (interactive()) {
            rc = tui::run_tui(opts);
        } else {
            rc = run_batch(opts);
        }
    } catch (const std::exception& e) {
        printf("%s发生异常：%s%s\n", ui::kRed, e.what(), ui::kReset);
        rc = 1;
    } catch (...) {
        printf("%s发生未知异常。%s\n", ui::kRed, ui::kReset);
        rc = 1;
    }
    if (argv) LocalFree(argv);
    logx::linef("会话结束（退出码 %d）", rc);
    logx::close();
    return rc;
}

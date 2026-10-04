# Environ Manage（环境管理器）

> By: Ming-QWQ520(明) · 开源协议: [AGPL-3.0](LICENSE)

一个纯命令行的 **Windows 开发环境下载管理工具**（C++20，TUI 基于 [FTXUI](https://github.com/ArthurSonzogni/FTXUI)（已内置于 `RustInstall/third_party/ftxui`，MIT），网络层为 WinHTTP，**无其他第三方依赖**）。
支持 **Rust / Python / Node.js / JDK (Temurin) / Go / .NET / Zig / PHP / Ruby (RubyInstaller) / Git For Windows** 十种环境的完整下载、校验、安装与全生命周期管理。

- **双模式**：无参数启动即为全屏 TUI 交互界面；带参数则为非交互批处理（脚本/CI 可用）。
- **分语言布局 + 全生命周期管理**：**JDK / Python / Node.js 多版本共存**，安装到
  `<安装目录>\<版本>`（如 `D:\Python\3.12.6\python.exe`），`<安装目录>\current` junction
  指向当前版本；**Go / .NET / Zig / PHP / Ruby / Git For Windows 单版本平铺**，直接安装于
  `<安装目录>`（如 `D:\Git\cmd\git.exe`），更新即覆盖、卸载删整个目录。
  所有语言均自动检测已装版本与位置，支持安装/更新/卸载。
- **国内网络友好**：GitHub API 经 gh-proxy 加速，安装包走中科大 / 上交 / 华为云 / npmmirror / 阿里云等镜像，失败自动切换，全程支持断点续传。
- **完整性校验**：SHA-256 / MD5 校验和验证 + 下载字节数比对，校验和缺失时降级为大小校验。

---

## 支持的环境

| 环境 | 版本源 | 镜像 / 加速 | 校验 |
| --- | --- | --- | --- |
| **Rust** | GitHub Releases API（`rust-lang/rust`）+ `static.rust-lang.org` channel 清单 | 中科大 / 官方 / 上交；API 走 gh-proxy | 清单 SHA-256（含 `.sha256` 伴随文件） |
| **Python** | python.org FTP 目录索引（官方 API v2 为可选元数据增强） | 华为云 / 官方 / npmmirror | SHA-256 → MD5 → 大小比对 |
| **Node.js** | `nodejs.org/dist/index.json` | npmmirror（同格式） | `SHASUMS256.txt` |
| **JDK (Temurin)** | Adoptium v3 API（`available_releases` + `feature_releases/<major>/ga`） | GitHub 直链走 gh-proxy | API 自带 SHA-256 |
| **Go** | `go.dev/dl/?mode=json&include=all` | golang.google.cn / 阿里云 | `files.sha256` |
| **.NET** | `builds.dotnet.microsoft.com` `release-metadata/releases-index.json` | 官方 CDN | 无官方校验和，按下载字节数与 `Content-Length` 比对 |
| **Zig** | `ziglang.org/download/index.json`（JSON 索引，自带 SHA-256） | 中科大 / npmmirror / 官方 | 索引自带 SHA-256 |
| **PHP** | `windows.php.net` 官方目录解析（releases + archives） | 官方 releases → archives 回退 | `sha256.sum` → 大小比对 |
| **Ruby** | `oneclick/rubyinstaller2` GitHub Releases（.7z） | gh-proxy 加速 → 直连 | 大小比对（官方无独立校验和） |
| **Git** | `git-for-windows/git` GitHub Releases（Git For Windows 便携版 MinGit 包） | gh-proxy 加速 → 直连 | 官方 `.sha256` → 大小比对 |

---

## 构建要求

- **Visual Studio 2022 (17.13+) 或 2026**，MSVC v143+，Windows 10 1803+（需自带 `tar.exe`）
- 无需额外安装 FTXUI 或其他库：`third_party/ftxui` 已随项目内置并直接参与编译

用 Visual Studio 打开解决方案 `RustInstall.slnx` 直接生成，或命令行：

```bat
msbuild RustInstall\RustInstall.vcxproj /p:Configuration=Release /p:Platform=x64
```

---

## 下载编译版（GitHub Releases 自动构建）

仓库内置 GitHub Actions 自动编译流水线（`.github/workflows/release.yml`），无需本地安装 Visual Studio 即可获取 Release 版可执行文件：

- **推送 tag（`v*`）**：构建 x64 / x86 双平台 Release，并创建带版本号的正式版本 Release；
- **推送 `main` / `WIP` 分支**：自动构建并滚动更新 **[最新编译版本](https://github.com/Ming-QWQ520/Environ-Manage/releases/latest)**（Release 标题中标注 build 编号与提交哈希）；
- **手动触发**：在 Actions 页面运行 "Release 编译发布"，可选择是否发布为最新编译版本；
- 附件统一附带 `SHA256SUMS.txt` 校验和；每个 zip 内含 `RustInstall.exe`、`LICENSE`、`README.md`。

最新编译版的固定下载链接（始终指向最近一次自动构建）：

```text
https://github.com/Ming-QWQ520/Environ-Manage/releases/latest/download/Environ-Manage-latest-x64.zip
https://github.com/Ming-QWQ520/Environ-Manage/releases/latest/download/Environ-Manage-latest-x86.zip
```

> 流水线首选 VS 2026 (v145) 工具集编译，若 runner 未安装 VS 2026 会自动回退 v143 (VS 2022)，两种环境产出的均为独立单文件 exe，无第三方运行时依赖。

---

## 使用

### TUI 交互模式（无参数启动）

```
RustInstall.exe
```

全屏 TUI：顶部为页眉（标题 / 作者 / 布局与当前步骤），中部为当前步骤面板，
底部为键位提示栏。启动后先选择管理目标（Rust / Python / Node.js / JDK / Go / .NET /
Zig / PHP / Ruby / Git），首页自动后台检测全部已装语言并显示摘要（**无安装时不显示**），
随后自动检测本机已装版本并显示安装位置（Python / Node.js / JDK / Go / .NET /
Zig / PHP / Ruby / Git 走"已安装检测"屏，可执行安装/更新/卸载/切换当前版本/打开目录；Rust 走"检查更新"屏），再按
"安装路径 → 选择版本 → 确认 → 下载安装 → 完成结果"逐步进行。

> **Ctrl+C 复制**：TUI 中选中文字后按 Ctrl+C 由终端完成复制（QuickEdit），未选中时
> 不会退出程序；退出请用 Esc（返回上一步）或完成页 Enter。

> **管理中心（←/→）**：在首页按 `←` / `→` 进入"全部已安装语言"管理中心 ——
> 列出本机检测到的所有语言（含外部安装，显示版本、完整位置、管理来源与当前版本标记），
> `Enter` 直接跳入该语言的管理界面；无任何已装语言时不显示空列表；
> 另提供 **PATH 体检**（`P`，清理用户 PATH 中指向不存在目录的失效项）、
> **缓存清理**（`C`，清空 `<exe目录>\cache`）与重新检测（`U`）。

- `↑↓` 或 `W/S`：移动选择
- `←→` 或 `A/D`：版本列表翻页（按需加载，可一直翻到最老版本）；**首页按 `←`/`→` 进入全部已装语言管理中心**
- `Enter`：确认
- **`Esc` 或退格：返回上一步**（历史栈导航，第一步再返回即退出）
- 版本列表支持输入关键字搜索（Python / SDK）
- 确认页 `M`：循环切换镜像（自动 → 官方 → 中科大 → 上交）
- 下载中 `Esc`：取消下载（保留已下载部分，下次自动断点续传）
- 完成页 `Esc`：返回主界面重新选择

所有面板仅整屏绘制一次，之后原位刷新，选择过程无闪屏。

### 命令行参数（脚本模式，非交互）

```
用法: RustInstall [选项]

  -p, --path <目录>        下载/安装目录（默认: 当前目录\Rust）
  -v, --version <版本>     latest 或具体版本号（如 1.99.0）
  -t, --target <三元组>    目标平台（默认: 当前架构，如 x86_64-pc-windows-msvc）
      --package <包名>     组件包，默认 rust=完整工具链（可选 cargo/rustc/rust-std 等）
  -f, --format <格式>      tar.gz（解压即用，默认）或 msi（Windows 安装器）
  -m, --mirror <源>        auto / official / ustc / sjtu（默认 auto 自动切换）
      --list               仅列出全部可用版本后退出
      --no-install         仅下载，不解压安装
      --keep-archive       安装后保留压缩包
      --no-path-setup      不询问、不修改用户 PATH
      --log <文件>         日志输出位置（默认: <exe 目录>\log\RustInstall.log）
  -y, --yes                所有询问采用默认值

Python 管理:
      --python             管理 Python 而不是 Rust
      --py-list            仅列出 Python 版本后退出
      --py-version <版本>  指定 Python 版本（默认 latest）
      --py-kind <类型>     installer（默认）/ embed / all
      --py-arch <架构>     amd64（默认自动）/ arm64 / x86
      --py-install         下载后静默安装（exe）/ 解压（zip）到 -p 目录
      --py-token <Token>   可选：python.org API Token（元数据增强，匿名 API 已限流）

SDK 管理:
      --sdk <id>           node / jdk / go / dotnet / zig / php / ruby / git
      --sdk-list           仅列出该 SDK 的版本后退出
      --sdk-version <版本> 指定版本（默认最新；jdk 为大版本，dotnet 为通道）
      Node.js 工具链（优先级: npm/npx 随装自带 → Corepack 开关 → pnpm/yarn）:
      --with-pnpm          node 安装后经 Corepack 全局安装 pnpm（默认全局启用）
      --with-yarn          node 安装后经 Corepack 全局安装 yarn
      --corepack <开关>    enable（默认，启用 Corepack 管理 pnpm/yarn）/ disable
      --pnpm-home <目录>   pnpm/npm 存储根目录（默认 <目录>\pnpm-repository）
      布局: node/jdk 多版本 <目录>\<版本> + <目录>\current junction；
            go/dotnet/zig/php/ruby/git 单版本平铺（直接安装于 <目录>）

卸载（批处理）:
      --uninstall [版本|all]
                           卸载受管安装（配合 --sdk <id> / --python；Rust 直接
                           --uninstall all）
```

示例：

```bat
:: TUI 交互模式
RustInstall.exe

:: 最新版 Rust 完整安装到 D:\Rust（相当于 rustup 的独立安装，无需联网安装器）
RustInstall.exe -p D:\Rust -v latest

:: 指定版本 + GNU 工具链 + 强制走中科大镜像，仅下载不安装
RustInstall.exe -p D:\Rust -v 1.85.0 -t x86_64-pc-windows-gnu -m ustc --no-install

:: 查看全部可安装版本
RustInstall.exe --list

:: 最新版 embed 嵌入式包解压到 D:\Python（免安装）
RustInstall.exe --python -p D:\Python --py-kind embed --py-install

:: 3.13.0 安装器静默安装到 D:\Python313
RustInstall.exe --python -p D:\Python313 --py-version 3.13.0 --py-install

:: 安装 Node.js 最新版（含 current/PATH，并启用 pnpm）
RustInstall.exe --sdk node -p D:\Sdk --with-pnpm

:: 彻底规范 pnpm/npm 存储（四个 pnpm 目录 + npm prefix/cache 一次配置并回读校验）
RustInstall.exe --sdk node -p D:\Sdk --with-pnpm --pnpm-home D:\pnpm-repository

:: Corepack 启用/禁用开关（管理 pnpm/yarn；禁用即移除 shim）
RustInstall.exe --sdk node -p D:\Sdk --corepack disable

:: Temurin JDK 21（自动选 21 线最新 ga）
RustInstall.exe --sdk jdk -p D:\Sdk --sdk-version 21

:: 列出各 SDK 版本（含支持标签）
RustInstall.exe --sdk go --sdk-list
RustInstall.exe --sdk dotnet --sdk-list

:: 安装 Zig 最新稳定版（中科大 / npmmirror 镜像自动切换）
RustInstall.exe --sdk zig -p D:\Sdk

:: 安装 PHP 最新稳定版（默认 TS 线程安全版；NTS 版本号带 -nts 后缀，如 8.3.14-nts）
RustInstall.exe --sdk php -p D:\Sdk

:: 安装 Ruby 最新版（RubyInstaller 便携解压，无需运行安装器）
RustInstall.exe --sdk ruby -p D:\Sdk

:: 安装 Git For Windows 便携版（MinGit 官方精简包解压即用）
RustInstall.exe --sdk git -p D:\Sdk
```

---

## Rust 环境管理

- **安装检测**：启动时自动检测本机是否已安装 Rust（PATH 与 `%USERPROFILE%\.cargo\bin`），
  已安装则显示版本与路径，未安装则不显示；进而与 GitHub 最新版本对比——
  **有新版本**时询问是否更新，**已是最新**时询问是否重装/修复。确认后自动以原安装目录为
  目标、预选最新版本，走常规下载安装流程（`Esc` 可跳过进入手动模式；检测到 rustup 托管的
  shim 会给出覆盖警告）。
- **版本检索**：调用 GitHub Releases API（`rust-lang/rust`）获取全部版本；直连失败时自动经
  [gh-proxy 镜像](https://gh-proxy.com/docs/github-accelerator)（`gh-proxy.com`，备用 `gh-proxy.org`）加速访问。
- **文件下载**：GitHub Releases 只提供版本信息，安装包实际位于 `static.rust-lang.org/dist/`，
  由 `channel-rust-<版本>.toml` 清单索引（含 SHA-256）。工具支持官方源与国内镜像
  （中科大 / 上交），自动模式按 **中科大 → 官方 → 上交** 依次尝试、失败自动切换，并支持**断点续传**。
- **完整安装**：下载 → SHA-256 校验 → 调用 Windows 自带 `tar.exe` 解压合并到指定目录 →
  运行 `rustc --version` / `cargo --version` 验证 → 可选将 `bin` 加入用户 PATH。
  也可选择 `.msi` 安装包格式（下载后调用 msiexec）。

检测到已安装的 Rust 时，启动后先进入"检查更新"页，所有接受路径均自动配置
（平台取自已安装的三元组、完整工具链、tar.gz），无需逐步引导：

- **有新版本** → 「更新到最新版本（自动配置，确认后开始）/ 手动选择其他版本」
- **已是最新** →
  「**修复**：一键本地秒修 —— 自动查找安装目录里已下载的 `rust-<版本>-<三元组>.tar.gz`
  直接解压覆盖，无需联网、零提问；本地包缺失/损坏时自动转下载修复（确认一次）」/
  「重装：删除旧压缩包重新下载并覆盖安装（确认一次）」/ 手动选择其他版本

安装完成后 `D:\Rust\bin` 下即为可用的 `rustc.exe` / `cargo.exe`；
选择加入 PATH 后重新打开终端即可使用 `rustc --version` 验证。

---

## Python 环境管理

数据源采用 **官方 FTP 目录为绝对主源**（python.org API v2 已对匿名请求严格限流，
仅作为可选的元数据增强）：

- **版本枚举**：解析 `https://www.python.org/ftp/python/` 目录索引（1 个请求），仅收录
  `X.Y[.Z[.W]]` 形式的正式版目录，日期取自索引的目录修改时间
- **文件枚举**：`/ftp/python/{版本}/` 目录索引（选定版本时 1 个请求），直链 =
  `https://www.python.org/ftp/python/{版本}/{文件名}`
- **元数据增强**（可选）：`--py-token <Token>` 提供 python.org API Token 后，一次性拉取
  API 元数据（SHA-256 / MD5 / 精确发布日期 / 大小）合并进 FTP 数据；匿名限流或失败时自动
  继续使用 FTP 数据
- **校验**：优先 SHA-256 → MD5（旧版本）→ 下载完整性（Content-Length 比对）
- **下载镜像**：官方 / 华为云 / npmmirror（国内镜像挂载点无 `ftp/` 前缀，已自动映射），
  支持断点续传与自动切换
- **管理**：exe 静默安装（`/quiet InstallAllUsers=0 TargetDir=<安装目录>\<版本>`），zip 解压
  （embed 嵌入式包），安装后 `python.exe --version` 验证；统一多版本布局
  `<安装目录>\<版本>` + `<安装目录>\current` junction，PATH 加入 `current` 与 `current\Scripts`
- **检测 / 更新 / 卸载**：自动扫描受管根目录（含版本与位置）+ 注册表 `PythonCore`
  （HKLM/HKCU）探测外部安装并标注来源；支持一键更新到最新（旧版本保留共存）与按版本卸载
  （current 自动切换到剩余的最近版本，全部卸完后清理 PATH 与受管记录）

---

## Node.js / JDK / Go / .NET / Zig / PHP / Ruby / Git For Windows 管理

TUI 选择目标或 `--sdk <id>` 进入，八个 SDK 走统一的 Provider 接口
（`providers/provider.hpp`，UI 只依赖 `provider.hpp` + `registry.hpp`，各 Provider 通过
静态注册器自注册到工厂，按名字创建）：

| SDK | 版本源 | 支持标签 | 布局 | 安装后 |
| --- | --- | --- | --- | --- |
| Node.js | `nodejs.org/dist/index.json`（npmmirror 镜像同格式） | `lts` 字段：字符串 → **LTS · 代号**（如 Krypton）；false → Current | 多版本 | zip 解压到 `\<版本>`，exe 在版本目录根 |
| JDK (Temurin) | Adoptium v3 `available_releases` + `feature_releases/<major>/ga` | API 的 LTS 大版本表 → **LTS**，其余功能版 | 多版本 | `\<release_name>`，`JAVA_HOME` 指向 current |
| Go | `go.dev/dl/?mode=json&include=all`（golang.google.cn / 阿里云镜像同路径） | 无 LTS：最新两个 minor → **Supported**，其余 **EOL** | 单版本平铺 | 直接安装于根目录（顶层 `go/` 自动合并），`GOROOT` 指向根目录 |
| .NET | `builds.dotnet.microsoft.com` 的 `release-metadata/releases-index.json`（带 BOM，自动剥离） | `support-phase`/`release-type` → **LTS / STS / EOL / Preview** | 单版本平铺 | 直接安装于根目录（扁平结构），`DOTNET_ROOT` 指向根目录 |
| Zig | `ziglang.org/download/index.json`（对象键为版本号，跳过 master） | 稳定版 **Stable** | 单版本平铺 | 直接安装于根目录（顶层 `zig-windows-*/` 自动合并） |
| PHP | `windows.php.net` 目录解析（`php-*-[nts-]Win32-*-<arch>.zip`） | **TS**（线程安全）/ **NTS** | 单版本平铺 | 直接安装于根目录（扁平结构） |
| Ruby | `oneclick/rubyinstaller2` Releases 的 `rubyinstaller-*-{x64,x86}.7z` | **RubyInstaller** | 单版本平铺 | 直接安装于根目录（7z 解压，经自动下载的 7zr.exe） |
| Git For Windows | `git-for-windows/git` Releases 的 `MinGit-*-*.zip`（排除 busybox） | **MinGit** | 单版本平铺 | 直接安装于根目录（扁平结构），`cmd\git.exe` |

- **布局分派**：`node` / `jdk` 多版本（`<安装目录>\<版本>` + `<安装目录>\current` junction，
  与 Python 一致）；其余语言单版本平铺（直接安装于 `<安装目录>`，无版本子目录与 junction，
  更新为覆盖安装，卸载删除整个安装目录）。历史版本安装的旧多版本目录仍可正常检测与卸载
- **PATH**：多版本加入 `<安装目录>\current[\<bin>]`；平铺加入 `<安装目录>[\<bin>]`；
  JDK 另设 `JAVA_HOME`、Go 设 `GOROOT`、.NET 设 `DOTNET_ROOT` 与
  `DOTNET_CLI_TELEMETRY_OPTOUT=1`；Zig / PHP / Ruby / Git 仅写 PATH
- **检测 / 更新 / 卸载 / 切换 / 打开目录**：进入目标环境后自动扫描已装版本与位置（本工具受管记录 +
  注册表 / 环境变量 / PATH 探测外部安装），可一键更新到最新（多版本新版本共存并切换 current，
  平铺覆盖安装）、卸载（多版本按版本删除 + 自动重指 current，平铺删除整个安装目录，
  全部卸完后清理 PATH/环境变量/受管记录）；**设为当前**（仅多版本；重指 current junction，
  PATH 立即生效）与**打开安装目录**（`O` 键）；无已装版本时不显示已安装列表区块；
  批处理用 `--uninstall [版本|all]`
- **校验**：Node 用 `SHASUMS256.txt`；JDK 用 API 自带的 SHA-256；Go 用 `files.sha256`；
  .NET 无校验和，由下载字节数与 `Content-Length` 比对保证
- **镜像**：Node 走 npmmirror、Go 走阿里云/国内官方、JDK 的 GitHub 直链走 gh-proxy 加速，
  失败自动回退官方源
- **Node 工具链（`--sdk node`）**：按优先级模型统一管理，安装完成后自动检测并显示各工具版本：
  1. **npm / npx**：随 Node.js 安装自动附带，不做任何安装动作，只检测并显示版本；
  2. **Corepack**：Node 内置，提供“启用/禁用”开关（`--corepack enable|disable`、TUI 确认页 `C` 键），
     用来统一管理 Yarn / pnpm；选装 pnpm/yarn 时自动启用；
  3. **Yarn / pnpm**：常用包管理器，推荐经 Corepack 管理版本（`corepack prepare <工具>@latest
     --activate`，全局可用），本工具不单独下载其二进制。TUI 确认页 `N` 键安装 pnpm（默认勾选）、
     `Y` 键安装 yarn。
- **存储位置规范化（`--pnpm-home <目录>`，默认 `<安装目录>\pnpm-repository`）**：彻底规范 pnpm
  的全局包、二进制文件与状态目录，npm 同步规范，全部写入后逐项回读校验：

  ```bat
  pnpm config set global-dir      D:\pnpm-repository\global      :: 全局包目录
  pnpm config set global-bin-dir  D:\pnpm-repository\bin         :: 全局命令（二进制）目录
  pnpm config set state-dir       D:\pnpm-repository\state       :: 状态目录
  pnpm config set cache-dir       D:\pnpm-repository\cache       :: 下载缓存目录
  npm   config set prefix         D:\pnpm-repository\npm-global  :: 全局包目录
  npm   config set cache          D:\pnpm-repository\npm-cache   :: 下载缓存目录
  ```

  两个全局目录（`bin` 与 `npm-global`）会自动追加进用户 PATH。TUI 模式下亦可在确认页
  直接输入自定义存储根目录（留空使用默认值）。

---

## 全部已装语言管理中心

- **入口**：首页按 `←` / `→`（或菜单选择目标前先看看电脑里有什么）。启动即后台自动检测，
  首页直接显示"已安装语言"摘要行（无安装时不显示）。
- **列表**：逐语言显示已检测到的版本摘要（最多 3 个，超出显示总数）、当前版本标记与
  外部安装标记；选中某语言后在下方明细区显示每个版本的完整安装位置与管理来源。
  **未检测到任何已安装语言时显示引导文案，不显示空列表。**
  列表由路由层直接驱动滚动（`↑↓` / `W S` 翻行、`Home/End` 首尾、`PgUp/PgDn` 翻页），
  长列表自动窗口化，选中项始终可见。
- **进入管理**：`Enter` 跳转该语言的管理界面 —— Rust 进"检查更新"屏，其余语言进
  "已安装检测"屏（安装/更新/卸载/设为当前/打开目录）。`←`/`→`/`Esc` 返回首页。
- **PATH 体检（`P`）**：扫描用户 PATH（HKCU\Environment，先展开环境变量再判断），
  一键移除指向不存在目录的失效项并回写、广播变更；无失效项时不动注册表。
- **缓存清理（`C`）**：清空 `<exe目录>\cache`（版本索引缓存、7zr 工具缓存），显示删除的
  文件数与释放空间；各安装目录下的 archives 安装包缓存不受影响（属用户数据）。
- **重新检测（`U`）**：手动触发一次全量重新检测。

---

## 完整日志

每次下载/安装过程都会写入日志（默认 **`<exe 目录>\log\RustInstall.log`**，可用 `--log` 指定位置），
带毫秒级时间戳，记录：会话开始与命令行、GitHub API 线路（直连/gh-proxy）与状态、
发行清单获取（镜像、大小）、工件解析（URL、SHA-256）、每次镜像下载尝试（断点偏移、
结果、用时）、SHA-256 校验、解压与组件合并、rustc/cargo 验证、PATH 修改等全过程。
TUI 模式下用户的选择（更新/修复/重装、版本、平台、组件包）同样入册。

---

## 工作原理

```
GitHub Releases API ──(直连 → gh-proxy 镜像)──▶ 版本列表（tag + 发布日期）
        │
        ▼
channel-rust-<版本>.toml ──(中科大/官方/上交)──▶ 包清单（每个组件的 URL + SHA-256）
        │                                          （1.10 之前的老版本无清单，
        ▼                                           按 GitHub 发布日期逐日探测）
下载 rust-<版本>-<三元组>.tar.gz ──▶ 断点续传 + 镜像自动切换 ──▶ SHA-256 校验
        │
        ▼
tar.exe 解压 → 合并到安装目录 → 验证 rustc/cargo → 可选写入用户 PATH (HKCU\Environment)
```

SDK 侧的统一安装流程（`providers/provider.cpp` 的 `install_to_root`）：
**下载 → 校验 → 解压（多版本到 `<root>\<版本>`，平铺到 `<root>`）→ 更新 current junction
（仅多版本）→ 写入 PATH/环境变量 → 运行验证命令**。

---

## 目录结构

| 路径 | 说明 |
| --- | --- |
| `RustInstall/RustInstall.cpp` | FTXUI 版 TUI（屏幕组件 + 后台工作线程）+ 批处理引擎 + CLI 参数 |
| `RustInstall/http.hpp` | WinHTTP 封装：GET / HEAD / 断点续传下载（可取消） |
| `RustInstall/github.hpp` | Releases API + gh-proxy 镜像回退 |
| `RustInstall/rust_dist.hpp` | Rust 镜像源、清单解析、下载、解压安装、PATH |
| `RustInstall/python.hpp` | Python FTP 主源枚举、缓存、API 增强、下载、校验、安装、检测 |
| `RustInstall/toml_lite.hpp` | channel 清单（TOML）轻量解析 |
| `RustInstall/json.hpp` | GitHub / Adoptium / Go / .NET API（JSON）轻量解析 |
| `RustInstall/sha256.hpp` · `md5.hpp` | 校验和实现（下载校验） |
| `RustInstall/strutil.hpp` | UTF-8 / 宽字符转换等工具 |
| `RustInstall/logger.hpp` | 毫秒级时间戳日志 |
| `RustInstall/console_ui.hpp` | 批处理模式的 UTF-8 / 进度条助手 |
| `RustInstall/platform/platform.hpp` | 架构探测、三元组、PATH/环境变量写入（HKCU） |
| `RustInstall/providers/provider.hpp` | **Provider 统一接口**与公共数据结构（UI 只依赖此文件） |
| `RustInstall/providers/registry.hpp` | Provider 注册表（工厂） |
| `RustInstall/providers/provider.cpp` | 公共安装流程 `install_to_root`（下载→校验→解压→junction→环境变量→验证） |
| `RustInstall/providers/checksum.hpp` · `archive.hpp` · `http_client.hpp` | SDK 侧校验 / 解压 / 下载公共件 |
| `RustInstall/providers/node_tools.hpp` | Node 工具链：npm/npx 检测显示、Corepack 启/禁开关、pnpm/yarn 经 Corepack 安装与存储位置规范化（含回读校验） |
| `RustInstall/providers/gh_releases.hpp` | 通用 GitHub Releases 查询（assets 直链/大小 + gh-proxy 线路记忆），供 Ruby / Git 使用 |
| `RustInstall/providers/{node,jdk,go,dotnet,zig,php,ruby,git}/*` | 八个 SDK 的 Provider 实现（版本枚举、工件解析、镜像、校验、解压布局） |
| `RustInstall/RustInstall.vcxproj` · `.filters` | MSVC 工程与筛选器（直接编译内置 FTXUI） |
| `RustInstall/third_party/ftxui` | FTXUI v7.0.3 源码（MIT，随项目一起编译） |
| `RustInstall.slnx` | Visual Studio 解决方案 |

---

## 说明与限制

- **旧版本 Rust**：GitHub 上最早的 0.1 也支持，但 1.10 之前没有 channel 清单，工具会按发布日期
  探测 `dist/<日期>/` 下的文件并从 `.sha256` 伴随文件取校验值；早期版本的可用平台有限
  （如 1.0 时代没有 `x86_64-pc-windows-msvc`，可选 `x86_64-pc-windows-gnu`）。
- **跨平台下载**：在 Windows 上也可下载 Linux / macOS 目标的三元组（如
  `x86_64-unknown-linux-gnu`），工具只保存压缩包并提示在目标平台解压。
- **MSI 格式**：仅适用于 Windows 目标；SHA-256 校验值来自 `.sha256` 伴随文件。
- **gh-proxy 的作用范围**：gh-proxy 只加速 GitHub 域名。本工具用它加速 Releases API；
  Rust 安装包不在 GitHub 上，因此走官方源 + 中科大 / 上交镜像。
- **.NET 校验**：官方不提供发布校验和，完整性由下载字节数与 `Content-Length` 比对保证。
- **测试钩子**：设置环境变量 `RUSTINSTALL_TEST_KEYS="119 115 13"`（`_getch` 按键码序列，
  空格分隔）可脚本化驱动交互菜单。

---

## 开源协议

本项目采用 **GNU Affero General Public License v3.0（AGPL-3.0）**，协议全文见 [LICENSE](LICENSE)。

```
Copyright (C) 2026 Ming-QWQ520(明)

本程序是自由软件：你可以遵照 GNU Affero 通用公共许可证（第 3 版，或你选择的任何更新版本）
的条款重新发布和/或修改它。

本程序的发布是希望它有用，但不提供任何担保，甚至不包含适销性或特定用途适用性的默示担保。
详见 GNU Affero 通用公共许可证。

你应该已经随本程序收到了许可证副本；如果没有，请见 <https://www.gnu.org/licenses/>。
```

AGPL-3.0 是强著佐权（copyleft）协议。与 GPL 的关键区别在于第 13 条：**若你修改本程序并
通过网络向用户提供服务，必须向这些用户提供对应的完整源代码。**

### 第三方组件

`RustInstall/third_party/ftxui` 为 [FTXUI](https://github.com/ArthurSonzogni/FTXUI) v7.0.3 的上游源码，
版权归原作者 Arthur Sonzogni 所有，**继续适用其自身的 MIT 协议**
（见 `RustInstall/third_party/ftxui/LICENSE`），不受本项目 AGPL-3.0 覆盖。
MIT 与 AGPL-3.0 兼容，二者可一并分发。

// providers/archive.hpp : 公共解压层（Windows 自带 tar.exe，支持 zip/tar.gz/tar.xz）
// 解压可视化：异步启动 tar.exe + 轮询解压目录体积，经全局回调上报
//   阶段(phase) 0=解压中 1=合并中 2=完成；done/total 字节；bps 平滑速度（B/s）
// 总大小判定：zip 中央目录精确（含 Zip64）/ tar.gz 尾部 ISIZE / tar.xz 流索引精确，
//   其余格式（如 7z）按压缩包大小 ×2 估算并以 99% 封顶，完成后以实际体积为准
#pragma once

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "logger.hpp"
#include "platform/platform.hpp"
#include "strutil.hpp"

namespace archive {

namespace fs = std::filesystem;

// --------------------------------------------------------------- 进度回调

using ExtractHook = std::function<void(int phase, uint64_t done, uint64_t total, double bps)>;

// 全局回调：同一时刻仅一个安装流在解压（TUI / 批处理各自设置，线程结束时清理）
inline ExtractHook& hook() {
    static ExtractHook h;
    return h;
}
inline void set_hook(ExtractHook cb) { hook() = std::move(cb); }
inline void clear_hook() { hook() = nullptr; }
inline void notify(int phase, uint64_t done, uint64_t total, double bps) {
    ExtractHook& h = hook();
    if (h) h(phase, done, total, bps);
}

// --------------------------------------------------------------- 展开后总大小

// 从尾向前扫描缓冲区定位标志
template <size_t N>
inline ptrdiff_t rfind_sig(const std::string& buf, const char (&sig)[N]) {
    for (ptrdiff_t i = (ptrdiff_t)buf.size() - (ptrdiff_t)N; i >= 0; --i)
        if (std::memcmp(buf.data() + i, sig, N - 1) == 0) return i;
    return -1;
}

// zip 中央目录：累加全部条目解压后大小（精确；含 Zip64 条目/目录扩展）
inline uint64_t zip_total_uncompressed(std::ifstream& f, uint64_t fsize) {
    constexpr uint64_t kTail = 65536 + 22;
    uint64_t tail_len = std::min<uint64_t>(fsize, kTail);
    std::string tail(tail_len, '\0');
    f.seekg((uint64_t)fsize - tail_len);
    f.read(tail.data(), (std::streamsize)tail_len);
    if (!f) return 0;

    // 经典 EOCD（PK\x05\x06）：取条目数与中央目录偏移/大小
    ptrdiff_t eocd = rfind_sig(tail, "PK\x05\x06");
    if (eocd < 0 || (size_t)eocd + 22 > tail.size()) return 0;
    auto rd16 = [&](size_t off) -> uint32_t {
        unsigned char a = (unsigned char)tail[off], b = (unsigned char)tail[off + 1];
        return a | (b << 8);
    };
    auto rd32 = [&](size_t off) -> uint32_t {
        return rd16(off) | (rd16(off + 2) << 16);
    };
    uint32_t count = rd16((size_t)eocd + 10);
    uint64_t cd_size = rd32((size_t)eocd + 12);
    uint64_t cd_off = rd32((size_t)eocd + 16);

    // Zip64：条目数/偏移为哨兵值时经 locator（PK\x06\x07）→ Zip64 EOCD（PK\x06\x06）取真实值
    if (count == 0xFFFF || cd_off == 0xFFFFFFFF || cd_size == 0xFFFFFFFF) {
        ptrdiff_t loc = rfind_sig(tail, "PK\x06\x07");
        if (loc < 0) return 0;
        auto lrd64 = [&](size_t off) -> uint64_t {
            unsigned char b[8];
            std::memcpy(b, tail.data() + off, 8);
            uint64_t v = 0;
            for (int k = 7; k >= 0; --k) v = (v << 8) | b[k];
            return v;
        };
        uint64_t z64_eocd = lrd64((size_t)loc + 8);
        if (z64_eocd == 0 || z64_eocd + 56 > fsize) return 0;
        std::string z(56, '\0');
        f.seekg(z64_eocd);
        f.read(z.data(), 56);
        if (!f) return 0;
        auto zrd32 = [&](size_t off) -> uint32_t {
            unsigned char b[4];
            std::memcpy(b, z.data() + off, 4);
            return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
        };
        auto zrd64 = [&](size_t off) -> uint64_t {
            unsigned char b[8];
            std::memcpy(b, z.data() + off, 8);
            uint64_t v = 0;
            for (int k = 7; k >= 0; --k) v = (v << 8) | b[k];
            return v;
        };
        count = zrd32(32);
        cd_size = zrd64(40);
        cd_off = zrd64(48);
    }
    if (cd_size == 0 || cd_off >= fsize) return 0;

    // 读取整个中央目录（上限 512MB 防御异常文件）
    if (cd_size > (512u << 20)) return 0;
    std::string cd(cd_size, '\0');
    f.seekg(cd_off);
    f.read(cd.data(), (std::streamsize)cd_size);
    if (!f) return 0;

    uint64_t total = 0;
    uint32_t seen = 0;
    size_t p = 0;
    while (p + 46 <= cd.size() && seen < count) {
        if (std::memcmp(cd.data() + p, "PK\x01\x02", 4) != 0) break;
        auto crd32 = [&](size_t off) -> uint32_t {
            unsigned char b[4];
            std::memcpy(b, cd.data() + p + off, 4);
            return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
        };
        auto crd16 = [&](size_t off) -> uint32_t {
            unsigned char b[2];
            std::memcpy(b, cd.data() + p + off, 2);
            return b[0] | (b[1] << 8);
        };
        uint32_t usize = crd32(24);
        uint32_t nlen = crd16(28), elen = crd16(30), clen = crd16(32);
        if (usize == 0xFFFFFFFF) { // Zip64：解压后大小在扩展字段（0x0001 首 8 字节）
            size_t e = p + 46 + nlen, eend = e + elen;
            while (e + 4 <= eend) {
                unsigned char b[2];
                std::memcpy(b, cd.data() + e, 2);
                uint32_t id = b[0] | (b[1] << 8);
                uint32_t sz = (unsigned char)cd[e + 2] | ((uint32_t)(unsigned char)cd[e + 3] << 8);
                if (id == 0x0001 && sz >= 8 && e + 4 + 8 <= eend) {
                    unsigned char q[8];
                    std::memcpy(q, cd.data() + e + 4, 8);
                    uint64_t v = 0;
                    for (int k = 7; k >= 0; --k) v = (v << 8) | q[k];
                    usize = (uint32_t)v;
                    break;
                }
                e += 4 + sz;
            }
        }
        total += usize;
        ++seen;
        p += 46 + nlen + elen + clen;
    }
    return seen ? total : 0;
}

inline uint64_t expand_total_bytes(const fs::path& archive, bool& exact) {
    exact = false;
    std::error_code ec;
    uint64_t fsize = fs::file_size(archive, ec);
    if (ec || fsize == 0) return 0;
    std::string ext = su::lower(su::wide_to_utf8(archive.extension().wstring()));

    std::ifstream f(archive, std::ios::binary);
    if (!f) return 0;
    uint64_t total = 0;
    if (ext == ".zip") {
        total = zip_total_uncompressed(f, fsize);
        if (total) {
            exact = true;
            return total;
        }
    } else if (ext == ".gz") {
        // gzip 尾部 ISIZE：解压后大小 mod 2^32（小于 4GiB 时精确）
        if (fsize > 4) {
            std::string t(4, '\0');
            f.seekg((uint64_t)fsize - 4);
            f.read(t.data(), 4);
            if (f) {
                unsigned char b[4];
                std::memcpy(b, t.data(), 4);
                total = (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) |
                        ((uint64_t)b[3] << 24);
                if (total > 0 && total < 0xFFFFFFFFull) {
                    exact = true;
                    return total;
                }
            }
        }
    } else if (ext == ".xz") {
        // xz 流索引：footer（12 字节，magic "YZ"）→ 回退尺寸 → index 首记录解压后大小
        if (fsize > 12) {
            std::string t(12, '\0');
            f.seekg((uint64_t)fsize - 12);
            f.read(t.data(), 12);
            if (f && t[10] == 'Y' && t[11] == 'Z') {
                unsigned char bs[4];
                std::memcpy(bs, t.data() + 4, 4);
                uint64_t bsz = ((uint64_t)bs[0] | ((uint64_t)bs[1] << 8) | ((uint64_t)bs[2] << 16) |
                                ((uint64_t)bs[3] << 24));
                bsz = (bsz + 1) * 4;
                uint64_t idx = fsize - 12;
                if (bsz <= idx) {
                    idx -= bsz; // index 起始
                    std::string ix(64, '\0');
                    f.seekg(idx);
                    f.read(ix.data(), (std::streamsize)ix.size());
                    size_t got = (size_t)f.gcount();
                    if (got > 2 && (unsigned char)ix[0] == 0x00) { // index 指示符
                        auto varint = [&](size_t& p) -> uint64_t {
                            uint64_t v = 0;
                            int shift = 0;
                            while (p < got && shift < 63) {
                                unsigned char c = (unsigned char)ix[p++];
                                v |= (uint64_t)(c & 0x7F) << shift;
                                if (!(c & 0x80)) break;
                                shift += 7;
                            }
                            return v;
                        };
                        size_t p = 1;
                        uint64_t records = varint(p);
                        uint64_t sum = 0;
                        for (uint64_t r = 0; r < records && p + 2 <= got; ++r) {
                            varint(p);  // unpadded size
                            sum += varint(p); // uncompressed size
                        }
                        if (sum) {
                            total = sum;
                            exact = true;
                            return total;
                        }
                    }
                }
            }
        }
    }
    // 兜底估算：压缩包大小 × 2（7z 等格式；进度以 99% 封顶，完成以实际为准）
    (void)total;
    return fsize * 2;
}

// --------------------------------------------------------------- 解压执行

// 统计目录内全部文件字节（容错跳过写入中的文件）
inline uint64_t dir_size_safe(const fs::path& dir) {
    uint64_t sum = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) sum += it->file_size(ec);
    }
    return sum;
}

// tar.exe 异步解压 + 轮询进度：dest 为解压目标目录（自行创建）
// flags 形如 L"-xf"（zip/tar.xz 自动识别）或 L"-xzf"（tar.gz）
inline bool extract_to_dir_with_progress(const fs::path& archive_file, const fs::path& dest,
                                         const wchar_t* flags, std::string& err) {
    wchar_t tar_exe[MAX_PATH * 2] = {};
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\tar.exe", tar_exe, MAX_PATH * 2);
    if (GetFileAttributesW(tar_exe) == INVALID_FILE_ATTRIBUTES) {
        err = "未找到 Windows 自带的 tar.exe";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dest, ec);
    bool exact = false;
    uint64_t total = expand_total_bytes(archive_file, exact);
    logx::linef("解压开始: %s → %s（总大小约 %s）", su::wide_to_utf8(archive_file.wstring()).c_str(),
                su::wide_to_utf8(dest.wstring()).c_str(),
                total ? su::human_size(total).c_str() : "未知");

    std::wstring args = std::wstring(flags) + L" \"" + archive_file.wstring() + L"\" -C \"" +
                        dest.wstring() + L"\"";
    PROCESS_INFORMATION pi{};
    HANDLE proc = platform::run_hidden_async(tar_exe, args, pi, err);
    if (!proc) {
        logx::line("解压失败: " + err);
        return false;
    }

    auto t0 = std::chrono::steady_clock::now();
    double bps = 0;
    uint64_t last_bytes = 0;
    auto last = t0;
    uint64_t done = 0;
    uint64_t cap = exact ? total : (total / 100) * 99; // 估算总量时 99% 封顶
    notify(0, 0, total, 0);
    for (;;) {
        if (WaitForSingleObject(proc, 220) == WAIT_OBJECT_0) {
            done = dir_size_safe(dest); // 终值以实际为准
            break;
        }
        uint64_t cur = dir_size_safe(dest);
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        if (dt >= 0.2) {
            double inst = (double)(cur - last_bytes) / dt;
            bps = bps == 0 ? inst : bps * 0.7 + inst * 0.3;
            last_bytes = cur;
            last = now;
        }
        done = cur;
        if (cap) done = std::min(done, cap);
        notify(0, done, total, bps);
    }
    DWORD code = 0;
    GetExitCodeProcess(proc, &code); // 须在关闭句柄前取退出码
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        err = "解压失败（tar 退出码 " + std::to_string(code) + "，压缩包可能损坏）";
        logx::line("解压失败: " + err);
        return false;
    }
    double avg = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    notify(2, done, total, avg > 0 ? (double)done / avg : bps);
    logx::linef("解压完成: %s（耗时 %.1fs，平均 %s/s）", su::human_size(done).c_str(), avg,
                su::human_size((uint64_t)(avg > 0 ? done / avg : 0)).c_str());
    return true;
}

// 将临时目录内容合并到版本目录（自动识别“单顶层目录”与“扁平”结构）
inline bool merge_tmp_to_ver_dir(const fs::path& tmp, const fs::path& ver_dir, std::string& err) {
    std::error_code ec;
    fs::create_directories(ver_dir, ec);
    std::vector<fs::path> entries;
    for (const fs::directory_entry& e : fs::directory_iterator(tmp)) entries.push_back(e.path());
    if (entries.size() == 1 && fs::is_directory(entries[0])) {
        // 单顶层目录 → 合并其内容到版本目录
        for (const fs::directory_entry& e : fs::directory_iterator(entries[0]))
            if (!platform::move_tree(e.path(), ver_dir, err)) return false;
    } else {
        // 扁平结构 → 全部上移
        for (const fs::path& e : entries)
            if (!platform::move_tree(e, ver_dir, err)) return false;
    }
    return true;
}

// 解压压缩包到版本目录：自动识别“单顶层目录”（node-vX/、jdk-*/、go/）与“扁平”（.NET SDK）
inline bool extract_to_ver_dir(const fs::path& archive_file, const fs::path& ver_dir,
                               std::string& err) {
    std::error_code ec;
    fs::path tmp = ver_dir.parent_path() / (L"_" + ver_dir.filename().wstring() + L"_tmp");
    fs::remove_all(tmp, ec);
    if (!extract_to_dir_with_progress(archive_file, tmp, L"-xf", err)) {
        fs::remove_all(tmp, ec);
        return false;
    }
    notify(1, 0, 0, 0); // 合并阶段（同卷重命名，通常瞬时完成）
    if (!merge_tmp_to_ver_dir(tmp, ver_dir, err)) {
        fs::remove_all(tmp, ec);
        return false;
    }
    fs::remove_all(tmp, ec);
    logx::linef("已解压到版本目录: %s", su::wide_to_utf8(ver_dir.wstring()).c_str());
    return true;
}

} // namespace archive

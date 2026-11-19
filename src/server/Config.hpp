#ifndef __CONFIG_HPP__
#define __CONFIG_HPP__

/**
 * @file Config.hpp
 * @brief 服务器配置：内置默认值 + Redis 风格配置文件 + 命令行覆盖。
 *        只在 server/ 内使用——禁止把整个 Config 传进 net/store/
 *        protocol/persistence，下层构造函数只接收自己需要的标量参数。
 *
 * 配置文件格式（Redis 风格，一行一条 key value，key 大小写不敏感）：
 *
 *     # 整行注释；空白后的 # 起行内注释
 *     port 9000
 *     subreactors 4
 *     appendonly yes
 *     dir /var/lib/kv
 *
 * 值约定：bool 取 yes/no/true/false/on/off/1/0（大小写不敏感）；
 * 内存水位（inputhighwater/outputhighwater）支持 k/kb/m/mb/g/gb 后缀（1024 进制）。
 * 优先级：内置默认 < 配置文件 < 命令行位置参数。
 * 解析进临时副本，任何一行出错整份文件不生效（fail fast，不带病运行）。
 */

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

struct Config
{
    std::string ip = "0.0.0.0";
    uint16_t port = 8080;
    size_t subReactors = std::thread::hardware_concurrency(); // 显式设 0 = 单 Reactor
    size_t shards = 16;                                       // 必须是 2 的幂
    size_t maxEntriesPerShard = 1 << 20;
    bool appendOnly = true;
    std::string aofPath = "appendonly.aof";
    std::string dir = "."; // 数据目录：启动时 chdir 进入，aof/日志的相对路径都落这里
    size_t inputHighWater = 64 << 20;
    size_t outputHighWater = 16 << 20;
    bool strictLru = false; // P6 对照实验
};

namespace cfgfile
{

inline std::string trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

inline std::string toLower(std::string s)
{
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool parseBool(const std::string &v, bool &out)
{
    const std::string s = toLower(trim(v));
    if (s == "yes" || s == "true" || s == "on" || s == "1") { out = true; return true; }
    if (s == "no" || s == "false" || s == "off" || s == "0") { out = false; return true; }
    return false;
}

// 纯十进制无符号整数（拒绝符号/空白/空串，溢出按失败处理）
inline bool parseU64(const std::string &v, unsigned long long &out)
{
    const std::string s = trim(v);
    if (s.empty()) return false;
    unsigned long long x = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        const unsigned long long d = static_cast<unsigned long long>(c - '0');
        if (x > (~0ULL - d) / 10) return false; // 溢出
        x = x * 10 + d;
    }
    out = x;
    return true;
}

// 内存大小：十进制数字 + 可选 k/kb/m/mb/g/gb 后缀（1024 进制，大小写不敏感）
inline bool parseSize(const std::string &v, unsigned long long &out)
{
    std::string s = toLower(trim(v));
    if (s.size() >= 2 && s.back() == 'b') { // kb/mb/gb 简写：仅在 b 前是 k/m/g 时吞掉
        const char u = s[s.size() - 2];
        if (u == 'k' || u == 'm' || u == 'g') s.pop_back();
    }
    unsigned long long mul = 1;
    if (!s.empty() && (s.back() == 'k' || s.back() == 'm' || s.back() == 'g')) {
        mul = (s.back() == 'k') ? (1ULL << 10) : (s.back() == 'm') ? (1ULL << 20) : (1ULL << 30);
        s.pop_back();
    }
    unsigned long long x = 0;
    if (!parseU64(s, x)) return false;
    if (x > ~0ULL / mul) return false; // 溢出
    out = x * mul;
    return true;
}

} // namespace cfgfile

/**
 * 解析配置文件并合并进 cfg（只改文件里出现的键）。
 * 失败返回 false 并在 err 里给 "路径:行号: 原因"；出错时 cfg 保持不变。
 */
inline bool loadConfigFile(const std::string &path, Config &cfg, std::string *err = nullptr)
{
    auto fail = [err, &path](int line, const std::string &msg) {
        if (err) *err = path + ":" + std::to_string(line) + ": " + msg;
        return false;
    };

    std::ifstream in(path);
    if (!in) {
        if (err) *err = path + ": cannot open";
        return false;
    }

    Config tmp = cfg; // 解析进副本，全部成功才提交
    std::string raw;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        // 去注释：行首或空白后的 # 起为注释（值含紧贴字符的 # 原样保留）
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '#' &&
                (i == 0 || std::isspace(static_cast<unsigned char>(raw[i - 1])))) {
                raw.erase(i);
                break;
            }
        }
        const std::string line = cfgfile::trim(raw);
        if (line.empty()) continue;

        const size_t sp = line.find_first_of(" \t");
        const std::string key = cfgfile::toLower(cfgfile::trim(line.substr(0, sp)));
        const std::string val = (sp == std::string::npos) ? "" : cfgfile::trim(line.substr(sp + 1));
        if (val.empty()) return fail(lineNo, "missing value for '" + key + "'");

        unsigned long long n = 0;
        if (key == "ip") {
            tmp.ip = val;
        } else if (key == "port") {
            if (!cfgfile::parseU64(val, n) || n > 65535)
                return fail(lineNo, "invalid port '" + val + "'");
            tmp.port = static_cast<uint16_t>(n);
        } else if (key == "subreactors") {
            if (!cfgfile::parseU64(val, n)) return fail(lineNo, "invalid number '" + val + "'");
            tmp.subReactors = static_cast<size_t>(n);
        } else if (key == "shards") {
            if (!cfgfile::parseU64(val, n)) return fail(lineNo, "invalid number '" + val + "'");
            tmp.shards = static_cast<size_t>(n);
        } else if (key == "maxentriespershard") {
            if (!cfgfile::parseU64(val, n)) return fail(lineNo, "invalid number '" + val + "'");
            tmp.maxEntriesPerShard = static_cast<size_t>(n);
        } else if (key == "appendonly") {
            if (!cfgfile::parseBool(val, tmp.appendOnly))
                return fail(lineNo, "invalid boolean '" + val + "'");
        } else if (key == "aofpath") {
            tmp.aofPath = val;
        } else if (key == "dir") {
            tmp.dir = val;
        } else if (key == "inputhighwater") {
            if (!cfgfile::parseSize(val, n)) return fail(lineNo, "invalid size '" + val + "'");
            tmp.inputHighWater = static_cast<size_t>(n);
        } else if (key == "outputhighwater") {
            if (!cfgfile::parseSize(val, n)) return fail(lineNo, "invalid size '" + val + "'");
            tmp.outputHighWater = static_cast<size_t>(n);
        } else if (key == "strictlru") {
            if (!cfgfile::parseBool(val, tmp.strictLru))
                return fail(lineNo, "invalid boolean '" + val + "'");
        } else {
            return fail(lineNo, "unknown config key '" + key + "'");
        }
    }
    cfg = tmp;
    return true;
}

/**
 * 启动时集中校验：不合法立即返回 false，不允许带病运行。
 * err 可选，收失败原因（默认 nullptr，Server.cc 的调用保持原样）。
 */
inline bool validateConfig(const Config &cfg, std::string *err = nullptr)
{
    auto fail = [err](const std::string &msg) {
        if (err) *err = msg;
        return false;
    };
    if (cfg.port == 0) return fail("port must be non-zero");
    if (cfg.shards == 0 || (cfg.shards & (cfg.shards - 1)) != 0) // 2 的幂
        return fail("shards must be a power of two");
    if (cfg.subReactors > 256) // 防手滑配出天文数字
        return fail("subReactors too large (max 256)");
    return true;
}

#endif

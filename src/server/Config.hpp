#ifndef __CONFIG_HPP__
#define __CONFIG_HPP__

/**
 * @file Config.hpp
 * @brief 服务器配置。只在 server/ 内使用——禁止把整个 Config 传进 net/store/
 *        protocol/persistence，下层构造函数只接收自己需要的标量参数。
 */

#include <cstddef>
#include <cstdint>
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
    size_t inputHighWater = 64 << 20;
    size_t outputHighWater = 16 << 20;
    bool strictLru = false; // P6 对照实验
};

// 启动时集中校验：不合法立即返回 false，不允许带病运行
inline bool validateConfig(const Config &cfg)
{
    if (cfg.port == 0)
        return false;
    if (cfg.shards == 0 || (cfg.shards & (cfg.shards - 1)) != 0) // 2 的幂
        return false;
    if (cfg.subReactors > 256) // 防手滑配出天文数字
        return false;
    return true;
}

#endif

/**
 * @file main.cc
 * @brief 组装入口：解析命令行 → 建 Config/Server → 信号处理与生命周期。
 *
 * 用法：./kvserver [port] [subReactors]
 *   subReactors 省略时用 hardware_concurrency()；显式传 0 是单 Reactor 模式。
 * 当前业务是 echo（回归基线）；P2 起替换为 RESP 解析 + 命令分发。
 */

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <string>
#include <thread>

#include "Config.hpp"
#include "Server.h"
#include "TcpConnection.h"

int main(int argc, char **argv)
{
    Config cfg;
    if (argc > 1) {
        cfg.port = static_cast<uint16_t>(std::strtoul(argv[1], nullptr, 10));
    }
    if (argc > 2) {
        cfg.subReactors = static_cast<size_t>(std::strtoul(argv[2], nullptr, 10));
    }
    if (!validateConfig(cfg)) {
        std::cerr << "invalid config: port = " << cfg.port
                  << ", subReactors = " << cfg.subReactors << std::endl;
        return 1;
    }

    // 先屏蔽信号，再创建任何线程；之后由主线程 sigwait 同步等待。
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    int err = pthread_sigmask(SIG_BLOCK, &set, nullptr);
    if (err != 0) {
        std::cerr << "pthread_sigmask: " << std::strerror(err) << std::endl;
        return 1;
    }

    Server server(cfg);
    server.setOnConnection([](const auto &) {});
    server.setOnMessage([](const auto &conn) { // echo：原样回显（回归基线业务）
        std::string msg(conn->inputBuffer().start(), conn->inputBuffer().length());
        conn->inputBuffer().erase(conn->inputBuffer().length());
        if (!msg.empty()) {
            if (conn->send(msg) == TcpConnection::SendResult::Backpressure) {
                conn->forceClose();
            }
        }
    });
    server.setOnClose([](const auto &) {});

    if (!server.start()) {
        std::cerr << "server start failed, port = " << cfg.port << std::endl;
        return 1;
    }
    std::cout << "kvserver listening on " << cfg.ip << ":" << cfg.port
              << ", subReactors = " << cfg.subReactors << std::endl;

    std::thread loopThread([&server] { server.loop(); });

    int sig = 0;
    err = sigwait(&set, &sig);
    if (err != 0) {
        std::cerr << "sigwait: " << std::strerror(err) << std::endl;
        server.stop();
        loopThread.join();
        return 1;
    }
    std::cout << "received signal " << sig << ", stopping..." << std::endl;

    // stop() 让各 loop 立刻退出（eventfd 唤醒），随后 join
    server.stop();
    loopThread.join();
    return 0;
}

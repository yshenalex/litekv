/**
 * @file main.cc
 * @brief 组装入口：解析命令行 → 建 Config/Store/CommandTable/Server → 信号处理与生命周期。
 *
 * 用法：./kvserver [port] [subReactors]
 *   subReactors 省略时用 hardware_concurrency()；显式传 0 是单 Reactor 模式。
 * 业务接线（P4）：RESP 解析 → CommandTable::dispatch → 编码回包（CALL-CONVENTIONS §10.1）；
 * P5 起把 ctx.onWrite 接到 persistence->append。
 */

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <string>
#include <thread>

#include "CommandTable.h"
#include "Config.hpp"
#include "RespEncoder.h"
#include "RespParser.h"
#include "RespValue.hpp"
#include "Server.h"
#include "ShardedStore.h"
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

    // store/CommandTable 先于 Server 构造：Server 析构（含 loop join）后才析构它们
    //（CALL-CONVENTIONS §4.2 规则 5/6）。CommandTable 初始化后只读，各 SubReactor 并发查表。
    ShardedStore store(cfg.shards, cfg.maxEntriesPerShard);
    CommandTable table;

    Server server(cfg);
    // 每连接一个 RespParser（Session 的雏形），挂在 context 槽上，随连接销毁，
    // 只在归属 SubReactor 线程访问（CALL-CONVENTIONS §3 硬规则 2）
    server.setOnConnection([](const auto &conn) {
        conn->setContext(std::make_shared<RespParser>());
    });
    // onMessage 标准流程（CALL-CONVENTIONS §10.1）：feed → 循环 next 到 NeedMore
    // → dispatch → 编码回包。每条命令重新构造 CommandContext（栈上，不跨命令保存）
    server.setOnMessage([&store, &table](const auto &conn) {
        auto parser = std::static_pointer_cast<RespParser>(conn->context());
        auto &in = conn->inputBuffer();
        parser->feed(in.start(), in.length());
        in.erase(in.length());

        for (;;) {
            RespValue frame;
            ParseStatus st = parser->next(frame);
            if (st == ParseStatus::NeedMore) {
                return;
            }
            if (st == ParseStatus::Error) {
                // 协议错误：回 -ERR 后冲刷关闭（CALL-CONVENTIONS §6 规则 3）
                conn->send(RespEncoder::error("ERR protocol error"));
                conn->closeAfterFlush();
                return;
            }

            if (frame.type != RespValue::Type::Array || frame.array.empty()) {
                conn->send(RespEncoder::error("ERR protocol error"));
                conn->closeAfterFlush();
                return;
            }

            // P5 起把 onWrite 接到 persistence->append；当前为空（无 AOF）
            CommandContext ctx{store, nullptr};
            RespValue reply = table.dispatch(frame.array, ctx);

            auto sr = conn->send(RespEncoder::encode(reply));
            if (sr == TcpConnection::SendResult::Closed) {
                return;
            }
            if (sr == TcpConnection::SendResult::Backpressure) {
                conn->forceClose();
                return;
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

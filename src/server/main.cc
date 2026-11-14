/**
 * @file main.cc
 * @brief 组装入口：解析命令行 → 建 Config/Store/CommandTable/Persistence/Server
 *        → 信号处理与生命周期。
 *
 * 用法：./kvserver [port] [subReactors] [appendonly]
 *   subReactors 省略时用 hardware_concurrency()；显式传 0 是单 Reactor 模式。
 *   appendonly 取 0/1（默认 1）；0 用 NullPersistence（压测对照、纯缓存场景）。
 * 业务接线（P5）：RESP 解析 → CommandTable::dispatch → 编码回包（CALL-CONVENTIONS §10.1）；
 * 写命令经 ctx.onWrite → persistence->append（CommitHook 锁内）；
 * 启动时 persistence->load() 回放 AOF，关闭时 flush()。
 */

#include <cctype>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

#include "AofPlugin.h"
#include "CommandTable.h"
#include "Config.hpp"
#include "IPersistence.hpp"
#include "RespEncoder.h"
#include "RespParser.h"
#include "RespValue.hpp"
#include "Server.h"
#include "ShardedStore.h"
#include "TcpConnection.h"

namespace
{

// 命令名大小写不敏感比较（仅接线层的控制命令路由用）
std::string toLower(const std::string &s)
{
    std::string r = s;
    for (auto &c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

} // namespace

int main(int argc, char **argv)
{
    Config cfg;
    if (argc > 1) {
        cfg.port = static_cast<uint16_t>(std::strtoul(argv[1], nullptr, 10));
    }
    if (argc > 2) {
        cfg.subReactors = static_cast<size_t>(std::strtoul(argv[2], nullptr, 10));
    }
    if (argc > 3) {
        cfg.appendOnly = std::strtoul(argv[3], nullptr, 10) != 0;
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

    // P5：持久化引擎（appendonly no 时 NullPersistence；启动顺序 §10.3：
    // Config → Store → CommandTable → IPersistence → load()，load 先于一切 IO 线程）
    std::unique_ptr<IPersistence> persistence;
    AofPlugin *aof = nullptr; // BGREWRITEAOF 路由用（见 onMessage 注释）
    if (cfg.appendOnly) {
        auto p = std::make_unique<AofPlugin>(cfg.aofPath);
        aof = p.get();
        persistence = std::move(p);
    } else {
        persistence = std::make_unique<NullPersistence>();
    }

    // 启动加载：AOF 逐帧回放（§10.4）。回放期间 onWrite 为空——否则重放会把
    // AOF 再追加一遍（§9.1）。replayer 不校验返回值：命令式持久化，重放即重演
    CommandReplayer replayer = [&store, &table](const std::vector<RespValue> &cmd) {
        CommandContext ctx{store, nullptr};
        table.dispatch(cmd, ctx);
        return true;
    };
    if (!persistence->load(replayer)) {
        std::cerr << "AOF load failed: " << cfg.aofPath << std::endl;
        return 1;
    }

    Server server(cfg);
    // 每连接一个 RespParser（Session 的雏形），挂在 context 槽上，随连接销毁，
    // 只在归属 SubReactor 线程访问（CALL-CONVENTIONS §3 硬规则 2）
    server.setOnConnection([](const auto &conn) {
        conn->setContext(std::make_shared<RespParser>());
    });

    // 写命令落 AOF：ctx.onWrite 接 persistence->append，由 store 在分片
    // unique_lock 内经 CommitHook 调用（§8.2），回包在 append 之后
    std::function<bool(const std::vector<RespValue> &)> onWrite =
        [&persistence](const std::vector<RespValue> &cmd) {
            return persistence->append(cmd);
        };

    // onMessage 标准流程（CALL-CONVENTIONS §10.1）：feed → 循环 next 到 NeedMore
    // → dispatch → 编码回包。每条命令重新构造 CommandContext（栈上，不跨命令保存）
    server.setOnMessage([&store, &table, &onWrite, aof](const auto &conn) {
        auto parser = std::static_pointer_cast<RespParser>(conn->context());
        auto &in = conn->inputBuffer();
        parser->feed(in.start(), in.length());
        in.erase(in.length());

        // 统一回包 + 背压处理（§10.1 规则 4）
        auto reply = [&conn](const RespValue &v) -> bool {
            auto sr = conn->send(RespEncoder::encode(v));
            if (sr == TcpConnection::SendResult::Closed) return false;
            if (sr == TcpConnection::SendResult::Backpressure) {
                conn->forceClose();
                return false;
            }
            return true;
        };

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

            // BGREWRITEAOF 是持久化引擎的控制命令：命令层禁止依赖 IPersistence
            //（§7.2.1），它不进 CommandTable，只能在接线层路由到 AofPlugin。
            // requestRewrite() 只置标志，真正重写在重写线程执行（§9.3 禁止在命令
            // 路径同步调用 maybeRewrite——此时未持分片锁也不行，会阻塞 SubReactor）
            if (toLower(frame.array[0].str) == "bgrewriteaof") {
                if (frame.array.size() != 1) {
                    reply(valError(
                        "ERR wrong number of arguments for 'bgrewriteaof' command"));
                } else if (aof) {
                    aof->requestRewrite();
                    reply(valSimple("Background append only file rewriting started"));
                } else {
                    reply(valError("ERR AOF is disabled"));
                }
                continue;
            }

            CommandContext ctx{store, onWrite};
            RespValue replyVal = table.dispatch(frame.array, ctx);
            if (!reply(replyVal)) return;
        }
    });
    server.setOnClose([](const auto &) {});

    if (!server.start()) {
        std::cerr << "server start failed, port = " << cfg.port << std::endl;
        return 1;
    }
    std::cout << "kvserver listening on " << cfg.ip << ":" << cfg.port
              << ", subReactors = " << cfg.subReactors
              << ", appendonly = " << (cfg.appendOnly ? "yes" : "no") << std::endl;

    // P5：AOF 重写线程——独立于 fsync 线程与命令路径（§9.3：maybeRewrite 只能由
    // 重写线程/主循环触发）。每秒检查阈值（文件 > 上次重写后 2 倍且 > 64MB）
    // 或 BGREWRITEAOF 手动触发（requestRewrite 标志在 maybeRewrite 内消费）
    std::mutex stopMtx;
    std::condition_variable stopCv;
    bool stopping = false;
    std::thread rewriteThread([&] {
        std::unique_lock<std::mutex> lk(stopMtx);
        while (!stopCv.wait_for(lk, std::chrono::seconds(1),
                                [&] { return stopping; })) {
            lk.unlock();
            persistence->maybeRewrite(store);
            lk.lock();
        }
    });

    std::thread loopThread([&server] { server.loop(); });

    int sig = 0;
    err = sigwait(&set, &sig);
    if (err != 0) {
        std::cerr << "sigwait: " << std::strerror(err) << std::endl;
    } else {
        std::cout << "received signal " << sig << ", stopping..." << std::endl;
    }

    // 关闭顺序（§10.3）：停 accept → 各 SubReactor quit → join → flush
    server.stop();
    loopThread.join();
    {
        std::lock_guard<std::mutex> lk(stopMtx);
        stopping = true;
    }
    stopCv.notify_all();
    rewriteThread.join();
    persistence->flush(); // 全部线程 join 后落盘
    return err != 0 ? 1 : 0;
}

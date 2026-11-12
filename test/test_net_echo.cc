/**
 * @file test_net_echo.cc
 * @brief net/server 集成测试：真实端口跑 echo 服务（net/server 层允许依赖真实网络）。
 *
 * 用法：test_net_echo <subReactors>
 *   ctest 注册两个用例：N=0（单 Reactor 退化）与 N=4（多 SubReactor）。
 *
 * 覆盖：小包、二进制（含 \0）、1MB 大包（partial write → EPOLLOUT 路径）、
 * 粘包批量、半关闭（echo 拿全后服务端关闭）、并发连接、stop() 快速退出。
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "Config.hpp"
#include "Server.h"
#include "TcpConnection.h"
#include "test_util.hpp"

namespace
{

uint16_t g_port = 0;

int connectTo(uint16_t port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::connect(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) == 0);
    return fd;
}

void sendAll(int fd, const void *data, size_t n)
{
    const char *p = static_cast<const char *>(data);
    while (n > 0)
    {
        ssize_t s = ::send(fd, p, n, 0);
        CHECK(s > 0);
        p += s;
        n -= static_cast<size_t>(s);
    }
}

void recvExact(int fd, void *data, size_t n)
{
    char *p = static_cast<char *>(data);
    while (n > 0)
    {
        ssize_t s = ::recv(fd, p, n, 0);
        CHECK(s > 0); // 提前关闭即失败
        p += s;
        n -= static_cast<size_t>(s);
    }
}

void checkEcho(int fd, const std::string &msg)
{
    sendAll(fd, msg.data(), msg.size());
    std::string got(msg.size(), '\0');
    recvExact(fd, &got[0], got.size());
    CHECK(got == msg);
}

void runCases()
{
    // 小包
    {
        int fd = connectTo(g_port);
        for (int i = 0; i < 50; ++i)
            checkEcho(fd, "hello-" + std::to_string(i) + "\n");
        ::close(fd);
    }

    // 二进制（含 \0）
    {
        int fd = connectTo(g_port);
        std::string bin;
        for (int i = 0; i < 100; ++i)
            for (int c = 0; c < 256; ++c)
                bin.push_back(static_cast<char>(c));
        checkEcho(fd, bin);
        ::close(fd);
    }

    // 1MB 大包（输出缓冲 partial write → EPOLLOUT 冲刷）
    {
        int fd = connectTo(g_port);
        checkEcho(fd, std::string(1 << 20, 'x') + "\n");
        ::close(fd);
    }

    // 粘包：100 条一次发出，按序全量回显
    {
        int fd = connectTo(g_port);
        std::string batch;
        for (int i = 0; i < 100; ++i)
            batch += "msg" + std::to_string(i) + "\n";
        checkEcho(fd, batch);
        ::close(fd);
    }

    // 半关闭：echo 拿全后，服务端应关闭连接（读到 0）
    {
        int fd = connectTo(g_port);
        std::string msg(100, 'h');
        sendAll(fd, msg.data(), msg.size());
        ::shutdown(fd, SHUT_WR);
        std::string got(msg.size(), '\0');
        recvExact(fd, &got[0], got.size());
        CHECK(got == msg);
        char tail;
        CHECK(::recv(fd, &tail, 1, 0) == 0); // 服务端已关闭
        ::close(fd);
    }

    // 并发连接
    {
        std::atomic<int> errors{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 16; ++i)
        {
            ts.emplace_back([&errors, i] {
                try
                {
                    int fd = connectTo(g_port);
                    std::string msg = "w" + std::to_string(i) + ":" + std::string(8192, 'y');
                    checkEcho(fd, msg);
                    ::close(fd);
                }
                catch (...)
                {
                    errors++;
                }
            });
        }
        for (auto &t : ts)
            t.join();
        CHECK_EQ(errors.load(), 0);
    }
}

} // namespace

int main(int argc, char **argv)
{
    size_t subReactors = (argc > 1) ? static_cast<size_t>(std::atoi(argv[1])) : 0;
    g_port = static_cast<uint16_t>(24730 + subReactors * 10);

    Config cfg;
    cfg.port = g_port;
    cfg.subReactors = subReactors;

    Server server(cfg);
    server.setOnConnection([](const auto &) {});
    server.setOnMessage([](const auto &conn) {
        std::string msg(conn->inputBuffer().start(), conn->inputBuffer().length());
        conn->inputBuffer().erase(conn->inputBuffer().length());
        if (!msg.empty())
        {
            if (conn->send(msg) == TcpConnection::SendResult::Backpressure)
                conn->forceClose();
        }
    });
    server.setOnClose([](const auto &) {});

    CHECK(server.start());
    std::thread loopThread([&server] { server.loop(); });

    runCases();

    // stop() 应通过 eventfd 唤醒快速退出
    auto t0 = std::chrono::steady_clock::now();
    server.stop();
    loopThread.join();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    CHECK(ms < 2000);

    std::printf("test_net_echo(subReactors=%zu) ALL PASS\n", subReactors);
    return 0;
}

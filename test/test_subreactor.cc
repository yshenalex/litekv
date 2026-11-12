/**
 * @file test_subreactor.cc
 * @brief SubReactor / SubReactorGroup 语义测试。
 *
 * 覆盖：N=0 空池（单 Reactor 退化）、round-robin 取循环、
 * queueInLoop 跨线程投递（在归属线程执行）、quit() 唤醒延迟。
 * 不需要监听端口。
 */

#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <thread>

#include "EventLoop.h"
#include "SubReactor.h"
#include "SubReactorGroup.h"
#include "test_util.hpp"

int main()
{
    // N=0：空池是合法状态（单 Reactor 退化模式）
    {
        SubReactorGroup g(0);
        g.start();
        CHECK_EQ(g.size(), 0u);
        CHECK(g.getNextLoop() == nullptr);
        g.quit();
    }

    // N=4：round-robin + loop 独立性 + 跨线程投递 + quit 唤醒
    {
        SubReactorGroup g(4);
        g.start();
        CHECK_EQ(g.size(), 4u);

        const std::vector<EventLoop *> &loops = g.loops();
        std::set<EventLoop *> distinct(loops.begin(), loops.end());
        CHECK_EQ(distinct.size(), 4u); // 4 个独立的 loop

        // round-robin：8 次取循环，每个恰好 2 次且按序轮转
        std::map<EventLoop *, int> cnt;
        for (int i = 0; i < 8; ++i)
        {
            EventLoop *l = g.getNextLoop();
            CHECK(l == loops[static_cast<size_t>(i) % 4]);
            cnt[l]++;
        }
        for (auto &kv : cnt)
            CHECK_EQ(kv.second, 2);

        // queueInLoop：任务在归属 loop 线程被执行（eventfd 唤醒路径）
        std::atomic<bool> ran{false};
        loops[2]->queueInLoop([&ran] { ran = true; });
        for (int i = 0; i < 200 && !ran.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(ran.load());

        // quit()：写 eventfd 唤醒，join 不应等 epoll 超时（保守断言 < 2s）
        auto t0 = std::chrono::steady_clock::now();
        g.quit();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(ms < 2000);

        // 重复 quit 不应崩溃
        g.quit();
    }

    std::printf("test_subreactor ALL PASS\n");
    return 0;
}

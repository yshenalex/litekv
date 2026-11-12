/**
 * @file test_store_stress.cc
 * @brief ShardedStore 压力/并发测试（DEV-GUIDE P3 验收）：
 *   1. 单线程 100 万 set + 100 万 get，打印均值耗时并校验终值；
 *   2. 4 线程 × 25 万并发：共享计数器精确 1M 次自增（无丢更新）、
 *      各线程独占键空间终值确定（无丢数据）、并发 sampleExpire 无死锁。
 * ASan / TSan 下同样应通过（构建时加 -fsanitize 即可）。
 */

#include "ShardedStore.h"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

int main()
{
    using Clock = std::chrono::steady_clock;

    // ---- 1. 单线程 100 万 set / 100 万 get ----
    {
        ShardedStore st(16);
        const int kOps = 1000 * 1000;
        const int kSpace = 100 * 1000;

        auto t0 = Clock::now();
        for (int i = 0; i < kOps; ++i) {
            CHECK(st.set("k" + std::to_string(i % kSpace), "v" + std::to_string(i)) ==
                  ShardedStore::Status::Ok);
        }
        auto t1 = Clock::now();
        long long hit = 0;
        for (int i = 0; i < kOps; ++i) {
            if (st.get("k" + std::to_string(i % kSpace)).has_value()) ++hit;
        }
        auto t2 = Clock::now();
        CHECK(hit == kOps);
        CHECK(st.dbSize() == (size_t)kSpace);
        // 抽查终值：k_j 最后一次写入是 i = kOps - kSpace + j
        for (int j = 0; j < 20; ++j) {
            auto v = st.get("k" + std::to_string(j));
            CHECK(v.has_value() && *v == "v" + std::to_string(kOps - kSpace + j));
        }
        double setUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / kOps;
        double getUs = std::chrono::duration<double, std::micro>(t2 - t1).count() / kOps;
        std::printf("[stress] single-thread 1M set: %.3f us/op, 1M get: %.3f us/op\n", setUs, getUs);
    }

    // ---- 2. 4 线程 × 25 万并发 ----
    {
        ShardedStore st(16);
        const int kThreads = 4;
        const int kOpsPerThread = 250 * 1000;
        const int kCounters = 16;
        const int kOwnSpace = 1000;
        std::atomic<int> readMismatches{0};
        std::vector<std::thread> threads;

        auto t0 = Clock::now();
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < kOpsPerThread; ++i) {
                    // 共享计数器：总自增次数必须精确（无丢更新）
                    long long r;
                    ShardedStore::Status s = st.incrBy("cnt:" + std::to_string(i % kCounters), 1, r);
                    if (s != ShardedStore::Status::Ok) readMismatches.fetch_add(1);
                    // 独占键空间：只有本线程写，立即读回必须一致
                    if ((i & 1) == 0) {
                        std::string key =
                            "own:" + std::to_string(t) + ":" + std::to_string(i % kOwnSpace);
                        std::string val = std::to_string(i);
                        st.set(key, val);
                        auto v = st.get(key);
                        if (!v.has_value() || *v != val) readMismatches.fetch_add(1);
                    }
                    if ((i & 63) == 0) {
                        st.get("cnt:" + std::to_string(i % kCounters));
                        st.sampleExpire((size_t)(i % 16)); // 并发过期抽样
                    }
                }
            });
        }
        for (auto &th : threads) th.join();
        auto t1 = Clock::now();
        CHECK(readMismatches.load() == 0);

        // 计数器总和 == 总操作数（1M，无丢更新）
        long long total = 0;
        for (int c = 0; c < kCounters; ++c) {
            auto v = st.get("cnt:" + std::to_string(c));
            CHECK(v.has_value());
            total += std::stoll(*v);
        }
        CHECK(total == (long long)kThreads * kOpsPerThread);

        // 独占键终值确定：key j 只写入 i ≡ j (mod 1000) 且 i 为偶
        // → j 偶：最后写入 i = 249000 + j；j 奇：从未写入
        for (int t = 0; t < kThreads; ++t) {
            for (int j = 0; j < kOwnSpace; ++j) {
                std::string key = "own:" + std::to_string(t) + ":" + std::to_string(j);
                if ((j & 1) == 0) {
                    auto v = st.get(key);
                    CHECK(v.has_value() && *v == std::to_string(kOpsPerThread - kOwnSpace + j));
                } else {
                    CHECK(!st.get(key).has_value());
                }
            }
        }

        double totalOps = (double)kThreads * kOpsPerThread * 1.5; // 约当操作数
        double secs = std::chrono::duration<double>(t1 - t0).count();
        std::printf("[stress] 4 threads x 250k ops: %.2f s, ~%.0f kops/s aggregate\n", secs,
                    totalOps / secs / 1000.0);
    }

    std::printf("test_store_stress: all checks passed\n");
    return 0;
}

/**
 * @file test_store.cc
 * @brief ShardedStore 语义单测：CRUD / NX-XX / TTL 惰性过期 / 采样 LRU 淘汰 /
 *        CommitHook 契约 / glob KEYS / 快照遍历。纯数据结构，不碰网络。
 */

#include "ShardedStore.h"
#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using Status = ShardedStore::Status;
using SetOpts = ShardedStore::SetOpts;

static void msleep(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int main()
{
    // ---- 1. 基础 set/get/覆盖/缺失 ----
    {
        ShardedStore st;
        CHECK(!st.get("k").has_value());
        CHECK(st.set("k", "v") == Status::Ok);
        auto v = st.get("k");
        CHECK(v.has_value() && *v == "v");
        CHECK(st.set("k", "v2") == Status::Ok);
        v = st.get("k");
        CHECK(v.has_value() && *v == "v2");
    }

    // ---- 2. 二进制安全：key/value 可含 \0 ----
    {
        ShardedStore st;
        std::string key("k\0e", 3), val("v\0l", 3);
        CHECK(st.set(key, val) == Status::Ok);
        auto v = st.get(key);
        CHECK(v.has_value() && *v == val);
    }

    // ---- 3. del / exists：多键、重复键、跨分片 ----
    {
        ShardedStore st(4);
        CHECK(st.set("a", "1") == Status::Ok);
        CHECK(st.set("b", "2") == Status::Ok);
        CHECK(st.set("c", "3") == Status::Ok);
        CHECK(st.exists({"a", "a", "b", "zz"}) == 3); // 重复键重复计（Redis 语义）
        CHECK(st.del({"a", "b", "zz", "b"}) == 2);    // 重复键只删一次
        CHECK(st.exists({"a", "b", "c"}) == 1);
        CHECK(st.del({"a"}) == 0);
        CHECK(st.dbSize() == 1);
    }

    // ---- 4. SET NX / XX 条件语义 ----
    {
        ShardedStore st;
        SetOpts nx;
        nx.nx = true;
        CHECK(st.set("k", "v1", nx) == Status::Ok);
        CHECK(st.set("k", "v2", nx) == Status::ConditionNotMet);
        CHECK(*st.get("k") == "v1");
        SetOpts xx;
        xx.xx = true;
        CHECK(st.set("k", "v3", xx) == Status::Ok);
        CHECK(*st.get("k") == "v3");
        CHECK(st.set("k2", "v", xx) == Status::ConditionNotMet);
        CHECK(!st.get("k2").has_value());
        SetOpts bad;
        bad.nx = bad.xx = true;
        CHECK(st.set("k3", "v", bad) == Status::ConditionNotMet);
    }

    // ---- 5. TTL：set 带 TTL / expire / persist / ttl 语义 ----
    {
        ShardedStore st;
        SetOpts o;
        o.exMs = 1000;
        CHECK(st.set("k", "v", o) == Status::Ok);
        long long t = st.ttl("k");
        CHECK(t > 500 && t <= 1000);
        CHECK(st.expire("k", 5000));
        t = st.ttl("k");
        CHECK(t > 4000 && t <= 5000);
        CHECK(st.persist("k"));
        CHECK(st.ttl("k") == -1);
        CHECK(!st.persist("k")); // 已无 TTL
        CHECK(!st.expire("nope", 1000));
        CHECK(st.ttl("nope") == -2);
        // 不带 TTL 的 set 清除旧 TTL（Redis 语义）
        CHECK(st.set("k", "v2") == Status::Ok);
        CHECK(st.ttl("k") == -1);
    }

    // ---- 6. expire <= 0 立即删除 ----
    {
        ShardedStore st;
        CHECK(st.set("k", "v") == Status::Ok);
        CHECK(st.expire("k", 0));
        CHECK(!st.get("k").has_value());
        CHECK(st.dbSize() == 0);
        CHECK(st.set("k2", "v") == Status::Ok);
        CHECK(st.expire("k2", -5));
        CHECK(st.dbSize() == 0);
    }

    // ---- 7. 惰性过期：读不删、写顺带删、sampleExpire 清理 ----
    {
        ShardedStore st(1);
        SetOpts o;
        o.exMs = 5;
        CHECK(st.set("k", "v", o) == Status::Ok);
        msleep(30);
        // 读：按不存在返回，不物理删除
        CHECK(!st.get("k").has_value());
        CHECK(st.exists({"k"}) == 0);
        CHECK(st.ttl("k") == -2);
        CHECK(st.keys("*").empty()); // 逻辑视图不含过期键
        CHECK(st.dbSize() == 1);     // 原始视图：物理条目还在
        // 写命中：按不存在计数，顺带物理删除
        CHECK(st.del({"k"}) == 0);
        CHECK(st.dbSize() == 0);

        CHECK(st.set("k", "v", o) == Status::Ok);
        msleep(30);
        CHECK(st.set("k", "v2") == Status::Ok); // 命中过期条目 → 惰性删除后重建
        CHECK(st.dbSize() == 1);
        CHECK(st.ttl("k") == -1);
        CHECK(*st.get("k") == "v2");

        CHECK(st.set("k", "v", o) == Status::Ok);
        msleep(30);
        st.sampleExpire(0);
        CHECK(st.dbSize() == 0);
    }

    // ---- 8. incrBy：创建 / 加减 / NotInt / OutOfRange / TTL 保留 ----
    {
        ShardedStore st(1);
        long long r = 0;
        CHECK(st.incrBy("n", 5, r) == Status::Ok && r == 5);
        CHECK(st.incrBy("n", -2, r) == Status::Ok && r == 3);
        CHECK(st.incrBy("n", 0, r) == Status::Ok && r == 3);
        CHECK(st.set("s", "abc") == Status::Ok);
        CHECK(st.incrBy("s", 1, r) == Status::NotInt);
        CHECK(st.set("s2", "007") == Status::Ok); // 前导零非法（Redis string2ll）
        CHECK(st.incrBy("s2", 1, r) == Status::NotInt);
        CHECK(st.set("s3", " 1") == Status::Ok); // 空白非法
        CHECK(st.incrBy("s3", 1, r) == Status::NotInt);
        CHECK(st.set("big", "9223372036854775807") == Status::Ok);
        CHECK(st.incrBy("big", 1, r) == Status::OutOfRange);
        CHECK(*st.get("big") == "9223372036854775807"); // 溢出不改内存
        CHECK(st.set("min", "-9223372036854775808") == Status::Ok);
        CHECK(st.incrBy("min", -1, r) == Status::OutOfRange);
        CHECK(st.incrBy("min", 1, r) == Status::Ok && r == -9223372036854775807LL);
        // INCR 保留已有 TTL
        SetOpts o;
        o.exMs = 10000;
        CHECK(st.set("t", "10", o) == Status::Ok);
        CHECK(st.incrBy("t", 5, r) == Status::Ok && r == 15);
        CHECK(st.ttl("t") > 5000);
        // 过期键按缺失处理（从 0 起）
        SetOpts o2;
        o2.exMs = 5;
        CHECK(st.set("e", "100", o2) == Status::Ok);
        msleep(30);
        CHECK(st.incrBy("e", 1, r) == Status::Ok && r == 1);
    }

    // ---- 9. appendValue：追加 / 二进制 / 空串语义 ----
    {
        ShardedStore st(1);
        CHECK(st.appendValue("a", "hello") == 5);
        CHECK(st.appendValue("a", std::string(" w\0rld", 6)) == 11);
        auto v = st.get("a");
        CHECK(v.has_value() && v->size() == 11 && v->compare(0, 5, "hello") == 0);
        CHECK(st.appendValue("a", "") == 11); // 空串无变化
        CHECK(st.appendValue("b", "") == 0);  // 新建空键
        CHECK(st.dbSize() == 2);
    }

    // ---- 10. 采样 LRU 淘汰：配额 + 最旧优先（确定性场景）----
    {
        ShardedStore st(1, 2);
        CHECK(st.set("a", "1") == Status::Ok);
        CHECK(st.set("b", "2") == Status::Ok);
        msleep(5);                      // 拉开 lastAccess
        CHECK(st.get("a").has_value()); // 刷新 a → b 最旧
        CHECK(st.set("c", "3") == Status::Ok); // 采样全取，淘汰最旧 b
        CHECK(st.exists({"a", "b", "c"}) == 2);
        CHECK(!st.get("b").has_value());
        CHECK(st.get("a").has_value());
        CHECK(st.get("c").has_value());
        CHECK(st.dbSize() == 2);
    }
    {
        // 淘汰带 TTL 的键时 expires 索引必须同步（不悬垂、不误删）
        ShardedStore st(1, 1);
        SetOpts o;
        o.exMs = 100000;
        CHECK(st.set("a", "1", o) == Status::Ok);
        CHECK(st.set("b", "2") == Status::Ok); // 淘汰 a
        CHECK(st.dbSize() == 1);
        st.sampleExpire(0);
        CHECK(st.get("b").has_value());
    }

    // ---- 11. CommitHook 契约：时机 / 条件 / 失败不回滚 ----
    {
        ShardedStore st(1);
        int calls = 0;
        auto hook = [&calls]() {
            ++calls;
            return true;
        };
        CHECK(st.set("k", "v", {}, hook) == Status::Ok);
        CHECK(calls == 1);
        CHECK(st.set("k", "v", {}, hook) == Status::Ok); // 同值也回调
        CHECK(calls == 2);
        SetOpts nx;
        nx.nx = true;
        CHECK(st.set("k", "x", nx, hook) == Status::ConditionNotMet); // 未生效不回调
        CHECK(calls == 2);
        CHECK(st.del({"zz"}, hook) == 0); // 删 0 个不回调
        CHECK(calls == 2);
        CHECK(st.del({"k"}, hook) == 1);
        CHECK(calls == 3);

        // hook 失败 → PersistenceError（或 err 传出），内存不回滚
        auto fail = []() { return false; };
        CHECK(st.set("k2", "v", {}, fail) == Status::PersistenceError);
        CHECK(*st.get("k2") == "v"); // 不回滚
        Status err = Status::Ok;
        CHECK(st.del({"k2"}, fail, &err) == 1);
        CHECK(err == Status::PersistenceError);
        CHECK(!st.get("k2").has_value()); // 删除本身不回滚

        CHECK(st.set("k3", "v") == Status::Ok);
        err = Status::Ok;
        CHECK(st.expire("k3", 1000, fail, &err));
        CHECK(err == Status::PersistenceError);
        CHECK(st.ttl("k3") > 0); // 不回滚
        err = Status::Ok;
        CHECK(st.persist("k3", fail, &err));
        CHECK(err == Status::PersistenceError);
        err = Status::Ok;
        CHECK(st.appendValue("k3", "x", fail, &err) == 2);
        CHECK(err == Status::PersistenceError);
        CHECK(st.dbSize() == 1); // 不回滚
        err = Status::Ok;
        CHECK(!st.flushDb(fail, &err));
        CHECK(err == Status::PersistenceError);
        CHECK(st.dbSize() == 0); // flush 不回滚：清空效果保留
        CHECK(st.set("k4", "v") == Status::Ok);
        CHECK(st.flushDb());
        CHECK(st.dbSize() == 0);
    }

    // ---- 12. keys() glob 匹配 ----
    {
        ShardedStore st(1);
        for (const char *k : {"hello", "hallo", "h_llo", "heee", "hello world", "*star", "x"}) {
            CHECK(st.set(k, "1") == Status::Ok);
        }
        CHECK(st.keys("h?llo").size() == 3);      // hello hallo h_llo
        CHECK(st.keys("h[ae]llo").size() == 2);   // hello hallo
        CHECK(st.keys("h[^ae]llo").size() == 1);  // h_llo
        CHECK(st.keys("he*").size() == 3);        // heee "hello" "hello world"
        CHECK(st.keys("\\*star").size() == 1);    // 字面 '*'
        CHECK(st.keys("*star").size() == 1);
        CHECK(st.keys("*").size() == 7);
        CHECK(st.keys("x").size() == 1);
    }

    // ---- 13. withAllShardsUnique + forEachEntryUnlocked 快照（跳过过期）----
    {
        ShardedStore st(2);
        CHECK(st.set("a", "1") == Status::Ok);
        CHECK(st.set("b", "2") == Status::Ok);
        SetOpts o;
        o.exMs = 5;
        CHECK(st.set("dead", "3", o) == Status::Ok);
        msleep(30);
        std::vector<std::string> seen;
        st.withAllShardsUnique([&]() {
            st.forEachEntryUnlocked([&](const std::string &k, const Entry &e) {
                seen.push_back(k + "=" + e.value);
            });
        });
        std::sort(seen.begin(), seen.end());
        CHECK(seen.size() == 2);
        CHECK(seen[0] == "a=1" && seen[1] == "b=2");
    }

    // ---- 14. sampleExpire：批量过期 + >25% 续轮 ----
    {
        ShardedStore st(1);
        SetOpts o;
        o.exMs = 5;
        for (int i = 0; i < 50; ++i) {
            CHECK(st.set("k" + std::to_string(i), "v", o) == Status::Ok);
        }
        CHECK(st.set("keep", "v") == Status::Ok);
        msleep(30);
        for (int i = 0; i < 30 && st.dbSize() > 1; ++i) st.sampleExpire(0);
        CHECK(st.dbSize() == 1);
        CHECK(st.get("keep").has_value());
    }

    // ---- 15. 构造：shardNum 向上取整到 2 的幂 ----
    {
        ShardedStore st(3);
        CHECK(st.shardNum() == 4);
        ShardedStore st2(0);
        CHECK(st2.shardNum() == 16);
    }

    std::printf("test_store: all checks passed\n");
    return 0;
}

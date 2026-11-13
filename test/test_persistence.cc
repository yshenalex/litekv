/**
 * @file test_persistence.cc
 * @brief 持久化层单测：AofPlugin 的 append/load/重写/背压 + NullPersistence。
 * 直接 new ShardedStore + CommandTable，不碰网络（CALL-CONVENTIONS §13）。
 * 跑法：ctest --test-dir build -R persistence
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "AofPlugin.h"
#include "CommandTable.h"
#include "IPersistence.hpp"
#include "RespEncoder.h"
#include "ShardedStore.h"
#include "test_util.hpp"

namespace
{

std::string tmpPath(const char *tag)
{
    return "/tmp/litekv_aof_" + std::to_string(getpid()) + "_" + tag + ".aof";
}

long fileSize(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

// 裸命令帧（BulkString 数组）
std::vector<RespValue> cmd(std::initializer_list<std::string> parts)
{
    std::vector<RespValue> v;
    v.reserve(parts.size());
    for (const auto &p : parts) v.push_back(valBulk(p));
    return v;
}

std::string enc(std::initializer_list<std::string> parts)
{
    RespValue arr;
    arr.type = RespValue::Type::Array;
    for (const auto &p : parts) arr.array.push_back(valBulk(p));
    return RespEncoder::encode(arr);
}

void writeRaw(const std::string &path, const std::string &bytes)
{
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(::write(fd, bytes.data(), bytes.size()) == (ssize_t)bytes.size());
    ::close(fd);
}

using OnWrite = std::function<bool(const std::vector<RespValue> &)>;

RespValue callWith(CommandTable &t, ShardedStore &s, OnWrite onWrite,
                   const std::vector<std::string> &raw)
{
    std::vector<RespValue> args;
    args.reserve(raw.size());
    for (const auto &r : raw) args.push_back(valBulk(r));
    CommandContext ctx{s, std::move(onWrite)};
    return t.dispatch(args, ctx);
}

RespValue call(CommandTable &t, ShardedStore &s, const std::vector<std::string> &raw)
{
    return callWith(t, s, nullptr, raw);
}

// 把 AOF 文件回放进空 store（回放期间 onWrite 为空，§9.1）
bool replayInto(ShardedStore &dst, CommandTable &table, const std::string &path)
{
    // 大 fsync 间隔：不让后台线程干扰确定性（flush/析构时瞬间唤醒）
    AofPlugin aof(path, 64 << 20, 64 << 20, 3600000);
    return aof.load([&](const std::vector<RespValue> &c) {
        CommandContext ctx{dst, nullptr};
        table.dispatch(c, ctx);
        return true;
    });
}

} // namespace

int main()
{
    CommandTable table;

    // ---- 1. NullPersistence：全空实现，replayer 不被调用 ----
    {
        NullPersistence np;
        bool called = false;
        CHECK(np.load([&](const std::vector<RespValue> &) {
            called = true;
            return true;
        }));
        CHECK(!called);
        CHECK(np.append(cmd({"set", "k", "v"})));
        ShardedStore s;
        np.maybeRewrite(s);
        np.flush();
    }

    // ---- 2. append → flush → load 回放：语义 + 二进制安全 ----
    {
        std::string path = tmpPath("rt");
        {
            AofPlugin aof(path, 64 << 20, 64 << 20, 3600000);
            const std::string bin("a\0b\r\nc", 6);
            CHECK(aof.append(cmd({"set", "foo", "bar"})));
            CHECK(aof.append(cmd({"set", "bin", bin})));
            CHECK(aof.append(cmd({"incr", "counter"})));
            CHECK(aof.append(cmd({"incr", "counter"})));
            CHECK(aof.append(cmd({"append", "foo", "baz"})));
            CHECK(aof.append(cmd({"expire", "foo", "60"})));
            CHECK(aof.append(cmd({"persist", "foo"})));
            CHECK(aof.append(cmd({"set", "tkey", "tv"})));
            CHECK(aof.append(cmd({"pexpire", "tkey", "60000"})));
            CHECK(aof.append(cmd({"del", "gone"})));
            aof.flush();
        }
        ShardedStore dst;
        CHECK(replayInto(dst, table, path));
        CHECK(dst.get("foo") == std::optional<std::string>("barbaz"));
        CHECK(dst.ttl("foo") == -1); // persist 后无 TTL
        CHECK(dst.get("bin") == std::optional<std::string>(std::string("a\0b\r\nc", 6)));
        CHECK(dst.get("counter") == std::optional<std::string>("2"));
        long long t = dst.ttl("tkey");
        CHECK(t > 0 && t <= 60000);
        CHECK(dst.dbSize() == 4);
        ::unlink(path.c_str());
    }

    // ---- 2b. FLUSHDB 回放：其后命令生效、之前的清空 ----
    {
        std::string path = tmpPath("flush");
        {
            AofPlugin aof(path, 64 << 20, 64 << 20, 3600000);
            CHECK(aof.append(cmd({"set", "a", "1"})));
            CHECK(aof.append(cmd({"flushdb"})));
            CHECK(aof.append(cmd({"set", "b", "2"})));
            aof.flush();
        }
        ShardedStore dst;
        CHECK(replayInto(dst, table, path));
        CHECK(!dst.get("a"));
        CHECK(dst.get("b") == std::optional<std::string>("2"));
        ::unlink(path.c_str());
    }

    // ---- 3. 命令层全链路：dispatch（onWrite → append）→ load → 状态一致 ----
    {
        std::string path = tmpPath("cmd");
        ShardedStore src;
        {
            AofPlugin aof(path, 64 << 20, 64 << 20, 3600000);
            OnWrite w = [&](const std::vector<RespValue> &c) { return aof.append(c); };
            CHECK(callWith(table, src, w, {"set", "k1", "v1"}).str == "OK");
            CHECK(callWith(table, src, w, {"set", "k2", "v2", "EX", "100"}).str == "OK");
            CHECK(callWith(table, src, w, {"incrby", "n", "41"}).integer == 41);
            CHECK(callWith(table, src, w, {"incr", "n"}).integer == 42);
            // SET NX 不生效：不得落 AOF（回放后 k1 仍是 v1）
            RespValue nx = callWith(table, src, w, {"set", "k1", "other", "NX"});
            CHECK(nx.type == RespValue::Type::Null);
            // TTL 调整 + 删除
            CHECK(callWith(table, src, w, {"expire", "k2", "200"}).integer == 1);
            CHECK(callWith(table, src, w, {"del", "k2"}).integer == 1);
            aof.flush();
        }
        ShardedStore dst;
        CHECK(replayInto(dst, table, path));
        CHECK(dst.get("k1") == src.get("k1"));
        CHECK(dst.get("k1") == std::optional<std::string>("v1"));
        CHECK(!dst.get("k2"));
        CHECK(dst.get("n") == std::optional<std::string>("42"));
        CHECK(dst.dbSize() == src.dbSize());
        ::unlink(path.c_str());
    }

    // ---- 4. 文件尾残帧：truncate 到最后完整帧，前缀照常回放 ----
    {
        std::string path = tmpPath("tail");
        std::string good = enc({"set", "a", "1"});
        writeRaw(path, good + std::string("$5\r\nab")); // 半条 bulk
        ShardedStore dst;
        CHECK(replayInto(dst, table, path));
        CHECK(dst.get("a") == std::optional<std::string>("1"));
        CHECK((long)good.size() == fileSize(path)); // 残帧被截掉
        ::unlink(path.c_str());
    }

    // ---- 5. 中途损坏帧：截断到最后完整帧，其后全部丢弃 ----
    {
        std::string path = tmpPath("bad");
        std::string first = enc({"set", "a", "1"});
        std::string bad = "$3\r\nabcXY"; // bulk 尾部不是 \r\n → 协议错误
        writeRaw(path, first + bad + enc({"set", "b", "2"}));
        ShardedStore dst;
        CHECK(replayInto(dst, table, path));
        CHECK(dst.get("a") == std::optional<std::string>("1"));
        CHECK(!dst.get("b")); // 损坏帧之后的命令全部丢弃
        CHECK((long)first.size() == fileSize(path));
        ::unlink(path.c_str());
    }

    // ---- 6. 高水位背压：append 返回 false → 命令层 -MISCONF ----
    {
        std::string path = tmpPath("hw");
        AofPlugin aof(path, 1024, 64 << 20, 3600000); // 1KB 水位，fsync 不来救场
        std::string big(200, 'x');
        bool overflowed = false;
        for (int i = 0; i < 1000; ++i) {
            if (!aof.append(cmd({"set", "k", big}))) {
                overflowed = true;
                break;
            }
        }
        CHECK(overflowed);
        // 水位是逐次判断：被拒的大帧可能给小帧留了空间。继续以大帧走命令层，
        // 缓冲不排空（fsync 间隔极大）时必然撞水位 → -MISCONF（§7.4）
        ShardedStore s;
        OnWrite w = [&](const std::vector<RespValue> &c) { return aof.append(c); };
        std::string big2(200, 'y');
        RespValue r;
        for (int i = 0; i < 100; ++i) {
            r = callWith(table, s, w, {"set", "k", big2});
            if (r.type == RespValue::Type::Error) break;
        }
        CHECK(r.type == RespValue::Type::Error);
        CHECK(r.str.find("MISCONF") != std::string::npos);
        aof.flush();
        ::unlink(path.c_str());
    }

    // ---- 7. 阈值重写：文件收敛 + 状态/TTL 保全 ----
    {
        std::string path = tmpPath("rw");
        ShardedStore src;
        {
            // rewriteMinSize=1：写满即触发（阈值：>= min 且 >= 上次重写后 2 倍）
            AofPlugin aof(path, 64 << 20, 1, 3600000);
            OnWrite w = [&](const std::vector<RespValue> &c) { return aof.append(c); };
            for (int i = 0; i < 50; ++i) {
                CHECK(callWith(table, src, w, {"set", "hot", std::to_string(i)}).str == "OK");
            }
            CHECK(callWith(table, src, w, {"set", "keep", "kv"}).str == "OK");
            CHECK(callWith(table, src, w, {"pexpire", "keep", "600000"}).integer == 1);
            CHECK(callWith(table, src, w, {"set", "plain", "pv"}).str == "OK");
            aof.flush();
            long before = fileSize(path);
            CHECK(before > 0);
            aof.maybeRewrite(src); // 同步重写（重写线程语义）
            long after = fileSize(path);
            CHECK(after > 0 && after < before); // 50 条 SET hot 收敛为 1 条
        }
        ShardedStore dst;
        CHECK(replayInto(dst, table, path));
        CHECK(dst.get("hot") == src.get("hot"));
        CHECK(dst.get("hot") == std::optional<std::string>("49"));
        CHECK(dst.get("keep") == std::optional<std::string>("kv"));
        CHECK(dst.get("plain") == std::optional<std::string>("pv"));
        long long t = dst.ttl("keep");
        CHECK(t > 0 && t <= 600000);
        CHECK(dst.dbSize() == src.dbSize());
        ::unlink(path.c_str());
    }

    // ---- 8. 重写与写并发：INCR 恰好一次语义（快照 < 尾部 < 后续追加）----
    {
        std::string path = tmpPath("race");
        ShardedStore src;
        {
            AofPlugin aof(path, 64 << 20, 64 << 20, 3600000);
            OnWrite w = [&](const std::vector<RespValue> &c) { return aof.append(c); };
            const int kRounds = 5000;
            std::thread writer([&] {
                for (int i = 0; i < kRounds; ++i) {
                    CHECK(callWith(table, src, w, {"incr", "counter"}).integer == i + 1);
                }
            });
            // 与写入并发地触发重写（requestRewrite + maybeRewrite = BGREWRITEAOF 路径）
            for (int i = 0; i < 5; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                aof.requestRewrite();
                aof.maybeRewrite(src);
            }
            writer.join();
            aof.flush();
        }
        ShardedStore dst;
        CHECK(replayInto(dst, table, path));
        // INCR 不幂等：多回放一次就错。快照+尾部+缓冲拼起来必须恰好 5000 次
        CHECK(dst.get("counter") == std::optional<std::string>("5000"));
        ::unlink(path.c_str());
    }

    // ---- 9. 手动触发（BGREWRITEAOF 路径）：低于阈值不重写，request 后重写 ----
    {
        std::string path = tmpPath("manual");
        ShardedStore src;
        {
            AofPlugin aof(path, 64 << 20, 64 << 20, 3600000); // 阈值高，自动重写不触发
            OnWrite w = [&](const std::vector<RespValue> &c) { return aof.append(c); };
            for (int i = 0; i < 20; ++i) {
                CHECK(callWith(table, src, w, {"set", "k", std::to_string(i)}).str == "OK");
            }
            aof.flush();
            long before = fileSize(path);
            aof.maybeRewrite(src);
            CHECK(fileSize(path) == before); // 阈值未到：不动
            aof.requestRewrite();
            aof.maybeRewrite(src);
            CHECK(fileSize(path) < before); // 手动触发：收敛
        }
        ShardedStore dst;
        CHECK(replayInto(dst, table, path));
        CHECK(dst.get("k") == std::optional<std::string>("19"));
        ::unlink(path.c_str());
    }

    // ---- 10. everysec 冒烟：不 flush 也自动落盘（fsync 线程周期 200ms）----
    {
        std::string path = tmpPath("sec");
        {
            AofPlugin aof(path, 64 << 20, 64 << 20, 200);
            CHECK(aof.append(cmd({"set", "k", "v"})));
            bool landed = false;
            for (int i = 0; i < 100 && !landed; ++i) { // 最多等 3s
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                landed = fileSize(path) > 0;
            }
            CHECK(landed);
        }
        ShardedStore dst;
        CHECK(replayInto(dst, table, path));
        CHECK(dst.get("k") == std::optional<std::string>("v"));
        ::unlink(path.c_str());
    }

    // ---- 11. 空/不存在的 AOF：load 成功，store 空 ----
    {
        std::string path = tmpPath("empty");
        ::unlink(path.c_str());
        ShardedStore dst;
        CHECK(replayInto(dst, table, path));
        CHECK(dst.dbSize() == 0);
        ::unlink(path.c_str());
    }

    std::printf("test_persistence: all tests passed\n");
    return 0;
}

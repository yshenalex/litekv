/**
 * @file test_command.cc
 * @brief 命令层单测：CommandTable 分发语义 + cmd_string 的 RespValue 映射。
 * 直接 new ShardedStore，不碰网络（CALL-CONVENTIONS §13）。
 * 跑法：ctest --test-dir build -R command
 */

#include <string>
#include <vector>

#include "CommandTable.h"
#include "test_util.hpp"

namespace
{

// 把裸字符串包装成 BulkString 参数帧并分发
RespValue call(CommandTable &t, ShardedStore &s, const std::vector<std::string> &raw)
{
    std::vector<RespValue> args;
    args.reserve(raw.size());
    for (const auto &r : raw) args.push_back(valBulk(r));
    CommandContext ctx{s, nullptr};
    return t.dispatch(args, ctx);
}

// 带 onWrite 计数的分发（验证 CommitHook 契约：写命令调用、读命令不调用）
RespValue callCounting(CommandTable &t, ShardedStore &s,
                       const std::vector<std::string> &raw, int &writes)
{
    std::vector<RespValue> args;
    args.reserve(raw.size());
    for (const auto &r : raw) args.push_back(valBulk(r));
    CommandContext ctx{s, [&](const std::vector<RespValue> &) {
                           ++writes;
                           return true;
                       }};
    return t.dispatch(args, ctx);
}

bool isError(const RespValue &v, const char *needle)
{
    return v.type == RespValue::Type::Error && v.str.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    ShardedStore store;
    CommandTable table;

    // ---- dispatch 语义（§7.3）----
    {
        // 未知命令：措辞与真 Redis 逐字节一致（含参数列表与尾随空格）
        RespValue r = call(table, store, {"frobnicate", "k"});
        CHECK(r.str == "ERR unknown command 'frobnicate', with args beginning with: 'k' ");
        // 无参数时没有参数列表
        r = call(table, store, {"frobnicate"});
        CHECK(r.str == "ERR unknown command 'frobnicate', with args beginning with: ");
    }
    {
        // arity 不符
        RespValue r = call(table, store, {"get"});
        CHECK(isError(r, "wrong number of arguments"));
        r = call(table, store, {"set", "k"});
        CHECK(isError(r, "wrong number of arguments"));
    }
    {
        // 命令名大小写不敏感
        CHECK_EQ(call(table, store, {"SeT", "k", "v"}).str == "OK", 1);
        RespValue r = call(table, store, {"GET", "k"});
        CHECK(r.type == RespValue::Type::BulkString && r.str == "v");
    }
    {
        // 参数类型非法（Array/Integer）→ 协议错误
        std::vector<RespValue> args;
        args.push_back(valBulk("get"));
        RespValue bad;
        bad.type = RespValue::Type::Integer;
        bad.integer = 1;
        args.push_back(bad);
        CommandContext ctx{store, nullptr};
        CHECK(isError(table.dispatch(args, ctx), "protocol error"));
    }

    // ---- GET/SET ----
    {
        CHECK_EQ(call(table, store, {"get", "missing"}).type == RespValue::Type::Null, 1);
        CHECK_EQ(call(table, store, {"set", "a", "hello"}).str == "OK", 1);
        RespValue r = call(table, store, {"get", "a"});
        CHECK(r.type == RespValue::Type::BulkString && r.str == "hello");
    }
    {
        // 二进制安全：value 含 \0
        std::string bin("x\0y", 3);
        CHECK_EQ(call(table, store, {"set", "bin", bin}).str == "OK", 1);
        RespValue r = call(table, store, {"get", "bin"});
        CHECK(r.str == bin);
        CHECK_EQ(r.str.size() == 3, 1);
        CHECK_EQ(call(table, store, {"strlen", "bin"}).integer == 3, 1);
    }
    {
        // NX / XX 条件语义：未生效返回 nil，不调 hook
        int writes = 0;
        RespValue r = callCounting(table, store, {"set", "a", "new", "NX"}, writes);
        CHECK_EQ(r.type == RespValue::Type::Null, 1);
        CHECK_EQ(writes, 0);
        CHECK(call(table, store, {"get", "a"}).str == "hello"); // 未被覆盖

        r = callCounting(table, store, {"set", "missing-xx", "v", "XX"}, writes);
        CHECK_EQ(r.type == RespValue::Type::Null, 1);
        CHECK_EQ(writes, 0);

        r = callCounting(table, store, {"set", "b", "v", "NX"}, writes);
        CHECK_EQ(r.str == "OK", 1);
        CHECK_EQ(writes, 1);
        r = callCounting(table, store, {"set", "b", "v2", "XX"}, writes);
        CHECK_EQ(r.str == "OK", 1);
        CHECK_EQ(writes, 2);

        // NX+XX 互斥、未知选项
        CHECK(isError(call(table, store, {"set", "c", "v", "NX", "XX"}), "syntax error"));
        CHECK(isError(call(table, store, {"set", "c", "v", "XX", "NX"}), "syntax error"));
        CHECK(isError(call(table, store, {"set", "c", "v", "FOO"}), "syntax error"));
    }
    {
        // EX/PX 选项
        CHECK_EQ(call(table, store, {"set", "ttl1", "v", "EX", "10"}).str == "OK", 1);
        long long ms = store.ttl("ttl1");
        CHECK(ms > 9000 && ms <= 10000); // 10s，允许毫秒级误差
        CHECK_EQ(call(table, store, {"set", "ttl2", "v", "PX", "60000"}).str == "OK", 1);
        ms = store.ttl("ttl2");
        CHECK(ms > 59000 && ms <= 60000);

        CHECK(isError(call(table, store, {"set", "d", "v", "EX", "0"}), "invalid expire time"));
        CHECK(isError(call(table, store, {"set", "d", "v", "EX", "-1"}), "invalid expire time"));
        // 解析失败 vs 数值非法是两种错误（对齐真 Redis）
        CHECK(isError(call(table, store, {"set", "d", "v", "EX", "abc"}), "not an integer"));
        CHECK(isError(call(table, store, {"set", "d", "v", "EX", "9223372036854775807"}),
                      "invalid expire time")); // 秒→毫秒溢出
        CHECK(isError(call(table, store, {"set", "d", "v", "PX", "9223372036854775807"}),
                      "invalid expire time")); // 相对时长过大（对拍发现）
        CHECK(isError(call(table, store, {"set", "d", "v", "EX"}), "syntax error"));
    }

    // ---- INCR/DECR/INCRBY ----
    {
        CHECK_EQ(call(table, store, {"incr", "n"}).integer == 1, 1);
        CHECK_EQ(call(table, store, {"incrby", "n", "5"}).integer == 6, 1);
        CHECK_EQ(call(table, store, {"decr", "n"}).integer == 5, 1);
        CHECK_EQ(call(table, store, {"get", "n"}).str == "5", 1);

        CHECK_EQ(call(table, store, {"set", "s", "abc"}).str == "OK", 1);
        CHECK(isError(call(table, store, {"incr", "s"}), "not an integer"));
        CHECK(isError(call(table, store, {"incrby", "n", "1x"}), "not an integer"));
        CHECK(isError(call(table, store, {"incrby", "n", "007"}), "not an integer")); // 前导零
        CHECK(isError(call(table, store, {"incrby", "n", "+1"}), "not an integer"));  // 禁 '+'

        // 溢出：不改内存
        CHECK_EQ(call(table, store, {"set", "big", "9223372036854775807"}).str == "OK", 1);
        CHECK(isError(call(table, store, {"incr", "big"}), "overflow"));
        CHECK_EQ(call(table, store, {"get", "big"}).str == "9223372036854775807", 1);
    }

    // ---- APPEND/STRLEN ----
    {
        CHECK_EQ(call(table, store, {"append", "ap", "hello"}).integer == 5, 1);
        CHECK_EQ(call(table, store, {"append", "ap", " world"}).integer == 11, 1);
        CHECK_EQ(call(table, store, {"strlen", "ap"}).integer == 11, 1);
        CHECK_EQ(call(table, store, {"get", "ap"}).str == "hello world", 1);
        CHECK_EQ(call(table, store, {"strlen", "nope"}).integer == 0, 1);
    }

    // ---- 读命令不触发 onWrite ----
    {
        int writes = 0;
        callCounting(table, store, {"get", "a"}, writes);
        callCounting(table, store, {"strlen", "a"}, writes);
        callCounting(table, store, {"incr", "ro"}, writes); // 写命令：+1
        CHECK_EQ(writes, 1);
    }

    // ---- DEL / EXISTS ----
    {
        CHECK_EQ(call(table, store, {"set", "d1", "v"}).str == "OK", 1);
        CHECK_EQ(call(table, store, {"set", "d2", "v"}).str == "OK", 1);
        // EXISTS 重复键重复计（Redis 语义）
        CHECK_EQ(call(table, store, {"exists", "d1", "d1", "nope"}).integer == 2, 1);
        // DEL 重复键去重、过期/不存在按 0 计
        CHECK_EQ(call(table, store, {"del", "d1", "d1", "nope"}).integer == 1, 1);
        CHECK_EQ(call(table, store, {"del", "d1"}).integer == 0, 1);
        CHECK_EQ(call(table, store, {"exists", "d2"}).integer == 1, 1);
    }

    // ---- EXPIRE/PEXPIRE/TTL/PTTL/PERSIST ----
    {
        CHECK_EQ(call(table, store, {"set", "e1", "v"}).str == "OK", 1);
        CHECK_EQ(call(table, store, {"expire", "e1", "10"}).integer == 1, 1);
        long long ms = store.ttl("e1");
        CHECK(ms > 9000 && ms <= 10000);
        CHECK_EQ(call(table, store, {"ttl", "e1"}).integer == 10, 1); // 秒（10s 无舍入歧义）

        // TTL 就近取整：剩余 ~1800ms → 2（截断则是 1；真 Redis 对拍确认的语义）
        CHECK_EQ(call(table, store, {"pexpire", "e1", "1800"}).integer == 1, 1);
        CHECK_EQ(call(table, store, {"ttl", "e1"}).integer == 2, 1);
        CHECK_EQ(call(table, store, {"pttl", "e1"}).integer > 1500, 1);

        // PERSIST：移除 TTL → 1；无 TTL → 0
        CHECK_EQ(call(table, store, {"persist", "e1"}).integer == 1, 1);
        CHECK_EQ(call(table, store, {"ttl", "e1"}).integer == -1, 1);
        CHECK_EQ(call(table, store, {"persist", "e1"}).integer == 0, 1);
        CHECK_EQ(call(table, store, {"ttl", "nope"}).integer == -2, 1);
        CHECK_EQ(call(table, store, {"pttl", "nope"}).integer == -2, 1);

        // 键不存在 → 0；<=0 → 立即删除返回 1
        CHECK_EQ(call(table, store, {"expire", "nope", "10"}).integer == 0, 1);
        CHECK_EQ(call(table, store, {"expire", "e1", "0"}).integer == 1, 1);
        CHECK_EQ(call(table, store, {"exists", "e1"}).integer == 0, 1);
        CHECK_EQ(call(table, store, {"set", "e2", "v"}).str == "OK", 1);
        CHECK_EQ(call(table, store, {"pexpire", "e2", "-5"}).integer == 1, 1);

        // 解析失败 vs 溢出是两种错误（对齐真 Redis）
        CHECK(isError(call(table, store, {"expire", "x", "bad"}), "not an integer"));
        CHECK(isError(call(table, store, {"pexpire", "x", "bad"}), "not an integer"));
        CHECK(isError(call(table, store, {"expire", "x", "9223372036854775807"}),
                      "invalid expire time in 'expire' command"));
        CHECK(isError(call(table, store, {"pexpire", "x", "9223372036854775807"}),
                      "invalid expire time in 'pexpire' command"));
    }

    // ---- TYPE / KEYS ----
    {
        CHECK_EQ(call(table, store, {"type", "a"}).str == "string", 1);
        CHECK_EQ(call(table, store, {"type", "nope"}).str == "none", 1);
        CHECK_EQ(call(table, store, {"set", "kk:1", "a"}).str == "OK", 1);
        CHECK_EQ(call(table, store, {"set", "kk:2", "b"}).str == "OK", 1);
        RespValue r = call(table, store, {"keys", "kk:*"});
        CHECK_EQ(r.type == RespValue::Type::Array, 1);
        CHECK_EQ(r.array.size() == 2, 1);
        CHECK_EQ(r.array[0].str == "kk:1" && r.array[1].str == "kk:2", 1);
        r = call(table, store, {"keys", "zzz*"});
        CHECK_EQ(r.type == RespValue::Type::Array && r.array.empty(), 1);
    }

    // ---- DBSIZE / FLUSHDB / COMMAND / INFO ----
    {
        CHECK_EQ(call(table, store, {"flushdb"}).str == "OK", 1);
        CHECK_EQ(call(table, store, {"dbsize"}).integer == 0, 1);
        CHECK_EQ(call(table, store, {"set", "z", "v"}).str == "OK", 1);
        CHECK_EQ(call(table, store, {"dbsize"}).integer == 1, 1);

        RespValue r = call(table, store, {"command"});
        CHECK_EQ(r.type == RespValue::Type::Array && r.array.empty(), 1);
        r = call(table, store, {"command", "docs"});
        CHECK_EQ(r.type == RespValue::Type::Array && r.array.empty(), 1);
        r = call(table, store, {"info"});
        CHECK_EQ(r.type == RespValue::Type::BulkString, 1);
        CHECK(r.str.find("redis_version") != std::string::npos);
    }

    std::printf("test_command: all checks passed\n");
    return 0;
}

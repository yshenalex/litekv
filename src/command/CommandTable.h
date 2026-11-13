#ifndef __COMMAND_TABLE_H__
#define __COMMAND_TABLE_H__

#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "RespValue.hpp"
#include "ShardedStore.h"

/**
 * @file CommandTable.h
 * @brief 命令层入口：命令表 + 分发 + handler 共用辅助。
 *
 * handler 只做"翻译"：校验参数 → 调 store → 把 Status/值映射成 RespValue。
 * 禁止 socket/Buffer/EventLoop/IPersistence/线程操作（越层 include 会直接编译失败）。
 * 分片计算、锁顺序、过期判断、淘汰全部由 store 负责，命令层不感知。
 * `CommandTable` 初始化后只读，所有 SubReactor 并发查表。
 */

/** 与 ShardedStore::CommitHook 同型；false = AOF 追加失败 */
using CommitHook = std::function<bool()>;

struct CommandContext
{
    ShardedStore &store;
    // server 注入：写命令落 AOF（P5 接 persistence->append）；可为空。
    // handler 禁止直接调用它，只能经 commitHook 传给 store（§2 补充）
    std::function<bool(const std::vector<RespValue> &)> onWrite;

    // 生成随本次 store 调用、在锁内同步执行一次的 hook。
    // lambda 捕获 this/&args——禁止保存、延迟或带到锁外（§7.2.8）
    CommitHook commitHook(const std::vector<RespValue> &args) const
    {
        if (!onWrite) return {};
        return [this, &args] { return onWrite(args); };
    }
};

using CommandHandler =
    std::function<RespValue(CommandContext &, const std::vector<RespValue> &)>;

struct Command
{
    CommandHandler handler;
    int arity;     // >0 精确参数个数（含命令名）；<0 至少 |arity| 个
    bool isWrite;  // 是否可能修改 store
    bool isGlobal; // 元数据（观测/限流），不参与加锁
};

class CommandTable
{
public:
    CommandTable(); // 构造期注册全部命令，之后只读

    // 仅初始化期调用；name 大小写不敏感（内部转小写）
    void reg(const std::string &name, Command cmd);
    // 大小写不敏感查表；未知命令返回 nullptr
    const Command *find(const std::string &name) const;
    // §7.3：查表 → 校验 arity → try/catch 调 handler → 返回 RespValue。
    // 不做编码、不做 I/O；异常兜底转 -ERR internal
    RespValue dispatch(const std::vector<RespValue> &args, CommandContext &ctx) const;

private:
    std::unordered_map<std::string, Command> table_;
};

// ---- handler 共用辅助 ----

inline RespValue valError(const std::string &msg)
{
    RespValue v;
    v.type = RespValue::Type::Error;
    v.str = msg;
    return v;
}
inline RespValue valSimple(const std::string &s)
{
    RespValue v;
    v.type = RespValue::Type::SimpleString;
    v.str = s;
    return v;
}
inline RespValue valInteger(long long n)
{
    RespValue v;
    v.type = RespValue::Type::Integer;
    v.integer = n;
    return v;
}
inline RespValue valBulk(const std::string &s)
{
    RespValue v;
    v.type = RespValue::Type::BulkString;
    v.str = s;
    return v;
}
inline RespValue valNull()
{
    RespValue v;
    v.type = RespValue::Type::Null;
    return v;
}

// store Status → RESP 错误
// Ok 不会走到这里；NotFound/ConditionNotMet 防御性返回 nil，调用方按命令语义自行处理
RespValue statusError(ShardedStore::Status st);

// Redis string2ll 语义：可选 '-' + 数字，除 "0"/"-0" 外禁止前导零，禁 '+'/空白
// （EX/PX/INCRBY 的数值参数解析用，与 store 内部同语义）
bool parseLL(const std::string &s, long long &out);

// 参数切片（args[from..] 的 str 值拷贝；key 列表用）
inline std::vector<std::string> argStrings(const std::vector<RespValue> &args,
                                           size_t from)
{
    std::vector<std::string> out;
    out.reserve(args.size() - from);
    for (size_t i = from; i < args.size(); ++i) out.push_back(args[i].str);
    return out;
}

// 相对 TTL 上限：超过它绝对到期时间（now+ms）会溢出 long long。
// 命令层不碰时钟（墙钟换算在 store 内部做），这里用常数留约百年余量，
// 与真 Redis 的精确边界（LLONG_MAX - now）只差一个可忽略的窗口
inline constexpr long long kMaxRelativeMs =
    std::numeric_limits<long long>::max() - 4'000'000'000'000LL;

// 各 cmd_*.cc 的注册入口（CommandTable 构造时调用）
void registerStringCommands(CommandTable &t);
void registerKeyCommands(CommandTable &t);
void registerServerCommands(CommandTable &t);

#endif

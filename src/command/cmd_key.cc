/**
 * @file cmd_key.cc
 * @brief 键命令：DEL/EXISTS/EXPIRE/PEXPIRE/TTL/PTTL/PERSIST/TYPE/KEYS（DEV-GUIDE P4-3）。
 *
 * 时长参数是相对值，墙钟换算在 store 内部完成；TTL 秒数在本层换算
 * （对齐 Redis：(ms+500)/1000 就近取整）。KEYS 返回逻辑视图（跳过过期）。
 * 多键命令（DEL/EXISTS）由 store 内部按键分组升序加锁，不保证跨片原子。
 */

#include <string>
#include <vector>

#include "CommandTable.h"

namespace
{

RespValue invalidExpire(const char *cmdName)
{
    return valError(std::string("ERR invalid expire time in '") + cmdName +
                    "' command");
}

// ---- DEL key [key ...] → 删除数（重复键去重；过期条目按不存在计）----
RespValue cmdDel(CommandContext &ctx, const std::vector<RespValue> &args)
{
    ShardedStore::Status err = ShardedStore::Status::Ok;
    long long n = ctx.store.del(argStrings(args, 1), ctx.commitHook(args), &err);
    if (err != ShardedStore::Status::Ok) return statusError(err);
    return valInteger(n);
}

// ---- EXISTS key [key ...] → 计数（重复键重复计，Redis 语义）----
RespValue cmdExists(CommandContext &ctx, const std::vector<RespValue> &args)
{
    return valInteger(ctx.store.exists(argStrings(args, 1)));
}

// ---- EXPIRE key seconds / PEXPIRE key ms ----
RespValue expireGeneric(CommandContext &ctx, const std::vector<RespValue> &args,
                        long long unitMs, const char *cmdName)
{
    long long when = 0;
    if (!parseLL(args[2].str, when)) {
        return valError("ERR value is not an integer or out of range");
    }

    long long ms = 0;
    if (when <= 0) {
        ms = 0; // <= 0 立即删除（Redis 语义）；负数不参与乘法，避开溢出
    } else if (unitMs == 1000) {
        if (when > std::numeric_limits<long long>::max() / 1000 ||
            when * 1000 > kMaxRelativeMs) {
            return invalidExpire(cmdName);
        }
        ms = when * 1000;
    } else {
        if (when > kMaxRelativeMs) return invalidExpire(cmdName);
        ms = when;
    }

    ShardedStore::Status err = ShardedStore::Status::Ok;
    bool ok = ctx.store.expire(args[1].str, ms, ctx.commitHook(args), &err);
    if (err != ShardedStore::Status::Ok) return statusError(err);
    return valInteger(ok ? 1 : 0);
}

RespValue cmdExpire(CommandContext &ctx, const std::vector<RespValue> &args)
{
    return expireGeneric(ctx, args, 1000, "expire");
}

RespValue cmdPexpire(CommandContext &ctx, const std::vector<RespValue> &args)
{
    return expireGeneric(ctx, args, 1, "pexpire");
}

// ---- TTL key（秒）/ PTTL key（毫秒）：-2 不存在 / -1 无 TTL / 剩余 ----
RespValue ttlGeneric(CommandContext &ctx, const std::vector<RespValue> &args,
                     bool msOutput)
{
    long long ms = ctx.store.ttl(args[1].str); // 剩余毫秒，-2/-1 原样透传
    if (ms < 0) return valInteger(ms);
    // Redis：秒数 = (ms + 500) / 1000，就近取整（真 Redis 对拍确认）
    return valInteger(msOutput ? ms : (ms + 500) / 1000);
}

RespValue cmdTtl(CommandContext &ctx, const std::vector<RespValue> &args)
{
    return ttlGeneric(ctx, args, false);
}

RespValue cmdPttl(CommandContext &ctx, const std::vector<RespValue> &args)
{
    return ttlGeneric(ctx, args, true);
}

// ---- PERSIST key → 1 移除 TTL / 0 无 TTL 或键不存在 ----
RespValue cmdPersist(CommandContext &ctx, const std::vector<RespValue> &args)
{
    ShardedStore::Status err = ShardedStore::Status::Ok;
    bool ok = ctx.store.persist(args[1].str, ctx.commitHook(args), &err);
    if (err != ShardedStore::Status::Ok) return statusError(err);
    return valInteger(ok ? 1 : 0);
}

// ---- TYPE key → string / none（留扩展）----
RespValue cmdType(CommandContext &ctx, const std::vector<RespValue> &args)
{
    bool has = ctx.store.exists({args[1].str}) == 1;
    return valSimple(has ? "string" : "none");
}

// ---- KEYS pattern → glob 匹配，逻辑视图（跳过过期）----
RespValue cmdKeys(CommandContext &ctx, const std::vector<RespValue> &args)
{
    RespValue out;
    out.type = RespValue::Type::Array;
    for (const auto &k : ctx.store.keys(args[1].str)) {
        out.array.push_back(valBulk(k));
    }
    return out;
}

} // namespace

void registerKeyCommands(CommandTable &t)
{
    t.reg("del", {cmdDel, -2, true, false});
    t.reg("exists", {cmdExists, -2, false, false});
    t.reg("expire", {cmdExpire, 3, true, false});
    t.reg("pexpire", {cmdPexpire, 3, true, false});
    t.reg("ttl", {cmdTtl, 2, false, false});
    t.reg("pttl", {cmdPttl, 2, false, false});
    t.reg("persist", {cmdPersist, 2, true, false});
    t.reg("type", {cmdType, 2, false, false});
    t.reg("keys", {cmdKeys, 2, false, true});
}

/**
 * @file cmd_string.cc
 * @brief 字符串命令：SET/GET/APPEND/STRLEN/INCR/DECR/INCRBY。
 *
 * handler 只做参数校验与 Status→RespValue 映射；
 * 写命令把 ctx.commitHook(args) 传给 store，由 store 在锁内回调。
 * key/value 原样二进制处理，禁止转小写/strlen。
 */

#include <limits>
#include <string>
#include <vector>

#include "CommandTable.h"

namespace
{

std::string toUpperAscii(const std::string &s)
{
    std::string out = s;
    for (char &c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// ---- GET key ----
RespValue cmdGet(CommandContext &ctx, const std::vector<RespValue> &args)
{
    auto v = ctx.store.get(args[1].str);
    if (!v) return valNull(); // 过期 = 不存在（读锁内不删除）
    return valBulk(*v);
}

// ---- SET key value [EX s|PX ms] [NX|XX] ----
RespValue cmdSet(CommandContext &ctx, const std::vector<RespValue> &args)
{
    ShardedStore::SetOpts opts;
    long long exMs = 0; // 相对毫秒，墙钟换算在 store 内部完成

    for (size_t i = 3; i < args.size(); ++i) {
        const std::string opt = toUpperAscii(args[i].str);
        if (opt == "EX" || opt == "PX") {
            if (i + 1 >= args.size()) return valError("ERR syntax error");
            long long n = 0;
            if (!parseLL(args[++i].str, n)) {
                // 解析失败与数值非法是两种错误（与真 Redis 对拍确认）
                return valError("ERR value is not an integer or out of range");
            }
            if (n <= 0) {
                return valError("ERR invalid expire time in 'set' command");
            }
            if (opt == "EX") {
                if (n > std::numeric_limits<long long>::max() / 1000 ||
                    n * 1000 > kMaxRelativeMs) {
                    return valError("ERR invalid expire time in 'set' command");
                }
                exMs = n * 1000;
            } else {
                if (n > kMaxRelativeMs) {
                    return valError("ERR invalid expire time in 'set' command");
                }
                exMs = n;
            }
        } else if (opt == "NX") {
            opts.nx = true;
        } else if (opt == "XX") {
            opts.xx = true;
        } else {
            return valError("ERR syntax error");
        }
    }
    if (opts.nx && opts.xx) return valError("ERR syntax error"); // 互斥
    opts.exMs = exMs;

    // SET 只要 NX/XX 通过就视为修改（即使值相同，也调 hook，Redis 语义）
    auto st = ctx.store.set(args[1].str, args[2].str, opts, ctx.commitHook(args));
    switch (st) {
    case ShardedStore::Status::Ok:
        return valSimple("OK");
    case ShardedStore::Status::ConditionNotMet:
        return valNull(); // NX 键已存在 / XX 键不存在
    default:
        return statusError(st);
    }
}

// ---- APPEND key value ----
RespValue cmdAppend(CommandContext &ctx, const std::vector<RespValue> &args)
{
    ShardedStore::Status err = ShardedStore::Status::Ok;
    long long len =
        ctx.store.appendValue(args[1].str, args[2].str, ctx.commitHook(args), &err);
    if (err != ShardedStore::Status::Ok) return statusError(err);
    return valInteger(len); // 返回新长度
}

// ---- STRLEN key ----
RespValue cmdStrlen(CommandContext &ctx, const std::vector<RespValue> &args)
{
    auto v = ctx.store.get(args[1].str);
    return valInteger(v ? static_cast<long long>(v->size()) : 0LL);
}

// INCR/DECR/INCRBY 共用：store 校验值是否可解析为 64 位整数并做溢出检查
RespValue incrByGeneric(CommandContext &ctx, const std::vector<RespValue> &args,
                        long long delta)
{
    long long result = 0;
    auto st = ctx.store.incrBy(args[1].str, delta, result, ctx.commitHook(args));
    if (st != ShardedStore::Status::Ok) return statusError(st);
    return valInteger(result);
}

// ---- INCR key ----
RespValue cmdIncr(CommandContext &ctx, const std::vector<RespValue> &args)
{
    return incrByGeneric(ctx, args, 1);
}

// ---- DECR key ----
RespValue cmdDecr(CommandContext &ctx, const std::vector<RespValue> &args)
{
    return incrByGeneric(ctx, args, -1);
}

// ---- INCRBY key delta ----
RespValue cmdIncrBy(CommandContext &ctx, const std::vector<RespValue> &args)
{
    long long delta = 0;
    if (!parseLL(args[2].str, delta)) {
        return valError("ERR value is not an integer or out of range");
    }
    return incrByGeneric(ctx, args, delta);
}

} // namespace

void registerStringCommands(CommandTable &t)
{
    t.reg("get", {cmdGet, 2, false, false});
    t.reg("set", {cmdSet, -3, true, false});
    t.reg("append", {cmdAppend, 3, true, false});
    t.reg("strlen", {cmdStrlen, 2, false, false});
    t.reg("incr", {cmdIncr, 2, true, false});
    t.reg("decr", {cmdDecr, 2, true, false});
    t.reg("incrby", {cmdIncrBy, 3, true, false});
}

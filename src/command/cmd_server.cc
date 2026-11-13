/**
 * @file cmd_server.cc
 * @brief 服务器命令：DBSIZE/FLUSHDB/极简 COMMAND/INFO（DEV-GUIDE P4-4）。
 *
 * COMMAND/INFO 只为 redis-cli 连接探测，返回固定/极简响应，
 * 不参与真 Redis 逐字节对拍（真 INFO 输出与运行时状态绑定，不可比）。
 */

#include <string>
#include <vector>

#include "CommandTable.h"

namespace
{

// ---- DBSIZE → 原始条目数（含未清理过期条目，Redis dictSize 同语义）----
RespValue cmdDbsize(CommandContext &ctx, const std::vector<RespValue> &args)
{
    (void)args;
    return valInteger(static_cast<long long>(ctx.store.dbSize()));
}

// ---- FLUSHDB → 清空当前库 ----
RespValue cmdFlushdb(CommandContext &ctx, const std::vector<RespValue> &args)
{
    ShardedStore::Status err = ShardedStore::Status::Ok;
    if (!ctx.store.flushDb(ctx.commitHook(args), &err)) {
        return statusError(err);
    }
    return valSimple("OK");
}

// ---- COMMAND [arg ...] → 空数组（redis-cli 探测命令表，空表即可工作）----
RespValue cmdCommand(CommandContext &ctx, const std::vector<RespValue> &args)
{
    (void)ctx;
    (void)args;
    RespValue out;
    out.type = RespValue::Type::Array;
    return out;
}

// ---- INFO [section] → 极简固定响应（bulk string，多行 \r\n 分隔）----
RespValue cmdInfo(CommandContext &ctx, const std::vector<RespValue> &args)
{
    (void)args;
    RespValue out;
    out.type = RespValue::Type::BulkString;
    out.str = "# Server\r\nredis_version:7.0.0-litekv\r\nredis_mode:standalone\r\n"
              "arch_bits:64\r\n# Keyspace\r\ndb0:keys=" +
              std::to_string(ctx.store.dbSize()) + ",expires=0\r\n";
    return out;
}

// ---- ECHO message → 原样返回 ----
// 工具兼容性命令（redis 有）：redis-cli --pipe 的进度探针依赖它——--pipe 周期性
// 发 ECHO <随机负载> 同步读写进度，缺了会报 unknown command 并触发 30s 无进度
// 看门狗（exit 1）。不是业务命令，不计 20 条核心命令
RespValue cmdEcho(CommandContext &ctx, const std::vector<RespValue> &args)
{
    (void)ctx;
    return valBulk(args[1].str);
}

} // namespace

void registerServerCommands(CommandTable &t)
{
    t.reg("dbsize", {cmdDbsize, 1, false, true});
    t.reg("flushdb", {cmdFlushdb, 1, true, true});
    t.reg("command", {cmdCommand, -1, false, true});
    t.reg("info", {cmdInfo, -1, false, true});
    t.reg("echo", {cmdEcho, 2, false, true});
}

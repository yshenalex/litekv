/**
 * @file CommandTable.cc
 * @brief 命令表注册与分发。
 */

#include "CommandTable.h"

#include <cctype>
#include <limits>

namespace
{

std::string toLowerAscii(const std::string &s)
{
    std::string out = s;
    for (char &c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Redis 错误消息打印参数用 %s 语义：截断到第一个 NUL
std::string cstrPart(const std::string &s)
{
    return s.substr(0, s.find('\0'));
}

// 对齐 Redis addReplyError：整条错误消息里的 \r\n 用空格消毒（否则破坏 RESP 帧）
void sanitizeError(std::string &msg)
{
    for (char &c : msg) {
        if (c == '\r' || c == '\n') c = ' ';
    }
}

} // namespace

RespValue statusError(ShardedStore::Status st)
{
    using S = ShardedStore::Status;
    switch (st) {
    case S::NotInt:
        return valError("ERR value is not an integer or out of range");
    case S::OutOfRange:
        return valError("ERR increment or decrement would overflow");
    case S::PersistenceError:
        return valError("MISCONF Errors writing to the AOF file");
    case S::OutOfMemory:
        return valError("OOM command not allowed when used memory > 'maxmemory'");
    case S::NotFound:
    case S::ConditionNotMet:
        return valNull(); // 防御：调用方按命令语义处理，不应传到这里
    case S::Ok:
        break;
    }
    return valNull();
}

bool parseLL(const std::string &s, long long &out)
{
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (s.size() == 1) return false;
    }
    if (s[i] == '0') {
        if (s.size() - i > 1) return false; // 前导零
        out = 0;
        return true;
    }
    unsigned long long acc = 0;
    const unsigned long long limit =
        neg ? (unsigned long long)std::numeric_limits<long long>::max() + 1ull
            : (unsigned long long)std::numeric_limits<long long>::max();
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        if (acc > (limit - (unsigned long long)(c - '0')) / 10ull) return false; // 溢出 → 非整数
        acc = acc * 10ull + (unsigned long long)(c - '0');
    }
    out = neg ? -(long long)acc : (long long)acc;
    return true;
}

CommandTable::CommandTable()
{
    registerStringCommands(*this);
    registerKeyCommands(*this);
    registerServerCommands(*this);
}

void CommandTable::reg(const std::string &name, Command cmd)
{
    table_[toLowerAscii(name)] = std::move(cmd);
}

const Command *CommandTable::find(const std::string &name) const
{
    auto it = table_.find(toLowerAscii(name));
    return it == table_.end() ? nullptr : &it->second;
}

RespValue CommandTable::dispatch(const std::vector<RespValue> &args,
                                 CommandContext &ctx) const
{
    if (args.empty()) {
        return valError("ERR protocol error");
    }
    // 参数只接受 BulkString/SimpleString（inline 已由 parser 归一化）
    for (const auto &a : args) {
        if (a.type != RespValue::Type::BulkString &&
            a.type != RespValue::Type::SimpleString) {
            return valError("ERR protocol error");
        }
    }

    const std::string &name = args[0].str;
    const Command *cmd = find(name);
    if (!cmd) {
        // 对齐 Redis：unknown command '<name>', with args beginning with: '<arg>' '<arg>' ...
        //（每个参数单引号包裹 + 尾随空格，%s 语义 NUL 截断，最后整体 \r\n 消毒）
        std::string msg = "ERR unknown command '" + cstrPart(name) +
                          "', with args beginning with: ";
        for (size_t i = 1; i < args.size(); ++i) {
            msg += "'";
            msg += cstrPart(args[i].str);
            msg += "' ";
        }
        sanitizeError(msg);
        return valError(msg);
    }

    const int n = static_cast<int>(args.size());
    const bool arityOk = cmd->arity > 0 ? (n == cmd->arity) : (n >= -cmd->arity);
    if (!arityOk) {
        return valError("ERR wrong number of arguments for '" +
                        toLowerAscii(name) + "' command");
    }

    try {
        return cmd->handler(ctx, args);
    } catch (...) {
        return valError("ERR internal"); // §7.3.4：唯一异常兜底点
    }
}

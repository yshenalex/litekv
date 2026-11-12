/**
 * @file RespEncoder.cc
 * @brief RespValue → RESP2 字节串，与 RespParser 互为逆操作。
 */

#include "RespEncoder.h"

std::string RespEncoder::encode(const RespValue &v)
{
    switch (v.type) {
    case RespValue::Type::SimpleString:
        return simple(v.str);
    case RespValue::Type::Error:
        return error(v.str);
    case RespValue::Type::Integer:
        return integer(v.integer);
    case RespValue::Type::BulkString:
        return bulk(v.str);
    case RespValue::Type::Null:
        return nullBulk();
    case RespValue::Type::Array: {
        std::string s = arrayHeader(v.array.size());
        for (const auto &e : v.array) {
            s += encode(e);
        }
        return s;
    }
    }
    return nullBulk(); // Type 枚举完备，防御编译器告警
}

std::string RespEncoder::simple(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 3);
    out += '+';
    out += s;
    out += "\r\n";
    return out;
}

std::string RespEncoder::error(const std::string &msg)
{
    std::string out;
    out.reserve(msg.size() + 3);
    out += '-';
    out += msg;
    out += "\r\n";
    return out;
}

std::string RespEncoder::integer(long long v)
{
    std::string out;
    out.reserve(24);
    out += ':';
    out += std::to_string(v);
    out += "\r\n";
    return out;
}

std::string RespEncoder::bulk(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 32);
    out += '$';
    out += std::to_string(s.size());
    out += "\r\n";
    out += s; // 长度语义，二进制安全（可含 \0）
    out += "\r\n";
    return out;
}

std::string RespEncoder::nullBulk()
{
    return "$-1\r\n";
}

std::string RespEncoder::arrayHeader(size_t n)
{
    std::string out;
    out.reserve(24);
    out += '*';
    out += std::to_string(n);
    out += "\r\n";
    return out;
}

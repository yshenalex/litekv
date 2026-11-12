#ifndef __RESP_ENCODER_H__
#define __RESP_ENCODER_H__

#include <cstddef>
#include <string>

#include "RespValue.hpp"

/**
 * @file RespEncoder.h
 * @brief RespValue → RESP2 字节串（CALL-CONVENTIONS §6）。
 *
 * 纯函数：不碰 socket、不加锁、不抛异常。encode 返回自持有的 std::string
 * （禁止返回指向入参的 string_view）。Null 编码为 $-1\r\n（RESP2 的 nil）。
 * 注意 SimpleString/Error 内容不得含 \r\n（由调用方保证，错误消息对齐 Redis 格式，
 * 如 "ERR unknown command 'x'"）。
 */
class RespEncoder
{
public:
    static std::string encode(const RespValue &v);
    static std::string simple(const std::string &s);
    static std::string error(const std::string &msg);
    static std::string integer(long long v);
    static std::string bulk(const std::string &s);
    static std::string nullBulk();
    static std::string arrayHeader(size_t n);
};

#endif

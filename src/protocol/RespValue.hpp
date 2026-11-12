#ifndef __RESP_VALUE_HPP__
#define __RESP_VALUE_HPP__

#include <string>
#include <vector>

/**
 * @file RespValue.hpp
 * @brief 协议层对外的唯一类型（DECOUPLE.md 契约 1，冻结勿改签名）。
 *
 * 所有模块之间传递的"协议值"，谁都不许自己定义第二种。
 * 自持有 std::string / std::vector（长度语义，二进制安全），跨线程传值或 shared_ptr；
 * 不提供 string_view 出口——视图不得离开它的所有者（CALL-CONVENTIONS §11.3）。
 */
struct RespValue
{
    enum class Type { SimpleString, Error, Integer, BulkString, Array, Null };
    Type type = Type::Null;
    std::string str;               // SimpleString / Error / BulkString
    long long integer = 0;         // Integer
    std::vector<RespValue> array;  // Array
};

#endif

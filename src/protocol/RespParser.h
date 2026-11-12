#ifndef __RESP_PARSER_H__
#define __RESP_PARSER_H__

#include <cstddef>
#include <string>

#include "RespValue.hpp"

/**
 * @file RespParser.h
 * @brief RESP2 增量解析器（CALL-CONVENTIONS §6）：纯状态机。
 *
 * 用法：parser.feed(字节); while (auto st = parser.next(v)) ...
 *   - Ok        ：产出一帧（一个完整 RespValue）
 *   - NeedMore  ：数据没到齐，原样等待（不消费任何字节，帧是原子的）
 *   - Error     ：协议错误，此后 parser 状态不可继续使用（next 一律返回 Error）
 *
 * 支持 5 种 RESP2 类型 + inline 命令（非类型前缀开头的文本行，按空白切成
 * BulkString 数组——inline 由 parser 归一化，见 CALL-CONVENTIONS §7.2）。
 * 行尾接受 \r\n 或 \n（telnet 可测）；BulkString 体按长度定界、尾部严格 \r\n。
 * 纯函数/纯状态机：不碰 socket、不加锁、不抛异常、不读全局配置。
 */

enum class ParseStatus { Ok, NeedMore, Error };

class RespParser
{
public:
    /**
     * @param maxBulkLen 单条 BulkString 长度上限，超限 = 协议错误（防恶意报文吃内存）
     * @param maxArrayLen 单个数组元素个数上限
     * @param maxDepth 数组嵌套深度上限
     */
    explicit RespParser(size_t maxBulkLen = 64 << 20,
                        size_t maxArrayLen = 1 << 16,
                        size_t maxDepth = 32);

    // 追加字节到内部缓冲（拷贝持有，Buffer 指针用后即失效没关系）
    void feed(const char *data, size_t len);
    // 取出一帧；Ok / NeedMore / Error
    ParseStatus next(RespValue &out);
    // 内部缓冲中尚未消费的字节数
    size_t buffered() const;
    // 自构造以来已消费的字节数（完整帧 + 被跳过的空行）。
    // 遇 NeedMore/Error 停下时，该值就是"最后一个完整帧的结束偏移"——
    // P5 AOF 恢复把文件尾残帧 truncate 到这里
    size_t consumed() const;

private:
    RespParser(const RespParser &) = delete;
    RespParser &operator=(const RespParser &) = delete;

    ParseStatus parseValue(const char *b, size_t n, size_t &off,
                           RespValue &out, bool &got, size_t depth);
    ParseStatus parseBulk(const char *b, size_t n, size_t &off, RespValue &out);
    ParseStatus parseArray(const char *b, size_t n, size_t &off,
                           RespValue &out, bool &got, size_t depth);
    ParseStatus parseInline(const char *b, size_t n, size_t &off,
                            RespValue &out, bool &got);
    // 扫一行：off = 内容起点；Ok 时 contentEnd = 内容终点（去掉行尾 \r）、off 推到行后
    ParseStatus scanLine(const char *b, size_t n, size_t &off, size_t &contentEnd);

private:
    std::string _buf;   // 自持有的输入缓冲（含 \0 也安全，长度语义）
    size_t _pos = 0;    // 已消费起点（帧与帧之间不留垃圾）
    size_t _consumed = 0;
    bool _poisoned = false; // Error 后置位，状态不可继续使用

    size_t _maxBulkLen;
    size_t _maxArrayLen;
    size_t _maxDepth;
};

#endif

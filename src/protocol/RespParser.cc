/**
 * @file RespParser.cc
 * @brief RESP2 增量解析器实现：帧原子解析（半包不消费、粘包循环取），纯状态机。
 */

#include "RespParser.h"

#include <climits>
#include <utility>

namespace {

constexpr size_t kMaxLine = 64 << 10; // 行式字段长度上限（Simple/Error/Integer/头部/inline）

// 严格解析十进制整数（可带一个 +/- 前缀）；溢出/非法返回 false
bool parseInt64(const char *s, size_t n, long long &out)
{
    if (n == 0) {
        return false;
    }
    size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = (s[0] == '-');
        i = 1;
        if (n == 1) {
            return false;
        }
    }
    unsigned long long acc = 0;
    const unsigned long long cap = neg ? (1ULL << 63) : (1ULL << 63) - 1;
    for (; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        unsigned d = static_cast<unsigned>(s[i] - '0');
        if (acc > (cap - d) / 10) {
            return false; // 溢出
        }
        acc = acc * 10 + d;
    }
    if (neg) {
        out = (acc == (1ULL << 63)) ? LLONG_MIN : -static_cast<long long>(acc);
    } else {
        out = static_cast<long long>(acc);
    }
    return true;
}

} // namespace

RespParser::RespParser(size_t maxBulkLen, size_t maxArrayLen, size_t maxDepth)
    : _maxBulkLen(maxBulkLen)
    , _maxArrayLen(maxArrayLen)
    , _maxDepth(maxDepth)
{
}

void RespParser::feed(const char *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return;
    }
    _buf.append(data, len);
}

ParseStatus RespParser::next(RespValue &out)
{
    if (_poisoned) {
        return ParseStatus::Error;
    }
    for (;;) {
        if (_pos >= _buf.size()) {
            return ParseStatus::NeedMore;
        }
        const char *b = _buf.data();
        size_t n = _buf.size();
        size_t off = _pos;
        RespValue tmp;
        bool got = true;
        ParseStatus st = parseValue(b, n, off, tmp, got, 1);
        if (st == ParseStatus::Error) {
            _poisoned = true;
            return ParseStatus::Error;
        }
        if (st == ParseStatus::NeedMore) {
            return ParseStatus::NeedMore; // 帧是原子的：半包不消费任何字节
        }
        _consumed += off - _pos;
        _pos = off;
        if (_pos == _buf.size()) {
            _buf.clear();
            _pos = 0;
        } else if (_pos > (1 << 16)) {
            _buf.erase(0, _pos);
            _pos = 0;
        }
        if (!got) {
            continue; // 空行：跳过继续取帧
        }
        out = std::move(tmp);
        return ParseStatus::Ok;
    }
}

size_t RespParser::buffered() const
{
    return _buf.size() - _pos;
}

size_t RespParser::consumed() const
{
    return _consumed;
}

// off 指向帧首字节；仅在 Ok 时把 off 推进到帧末尾。got=false 表示遇到空行（未产出帧）。
// depth 是"值的嵌套层数"（顶层 = 1）；深度上限只约束数组嵌套，标量不占深度。
ParseStatus RespParser::parseValue(const char *b, size_t n, size_t &off,
                                   RespValue &out, bool &got, size_t depth)
{
    if (off >= n) {
        return ParseStatus::NeedMore;
    }

    switch (b[off]) {
    case '+':
    case '-':
    case ':': {
        size_t lineStart = off + 1;
        size_t lineOff = lineStart;
        size_t contentEnd = 0;
        ParseStatus st = scanLine(b, n, lineOff, contentEnd);
        if (st != ParseStatus::Ok) {
            return st;
        }
        RespValue tmp;
        if (b[off] == '+') {
            tmp.type = RespValue::Type::SimpleString;
            tmp.str.assign(b + lineStart, contentEnd - lineStart);
        } else if (b[off] == '-') {
            tmp.type = RespValue::Type::Error;
            tmp.str.assign(b + lineStart, contentEnd - lineStart);
        } else {
            if (!parseInt64(b + lineStart, contentEnd - lineStart, tmp.integer)) {
                return ParseStatus::Error;
            }
            tmp.type = RespValue::Type::Integer;
        }
        off = lineOff;
        out = std::move(tmp);
        return ParseStatus::Ok;
    }
    case '$':
        return parseBulk(b, n, off, out);
    case '*':
        return parseArray(b, n, off, out, got, depth);
    case '\n':
        ++off;
        got = false;
        return ParseStatus::Ok;
    case '\r': {
        if (off + 1 >= n) {
            return ParseStatus::NeedMore;
        }
        if (b[off + 1] != '\n') {
            return ParseStatus::Error;
        }
        off += 2;
        got = false;
        return ParseStatus::Ok;
    }
    default:
        return parseInline(b, n, off, out, got);
    }
}

ParseStatus RespParser::parseBulk(const char *b, size_t n, size_t &off, RespValue &out)
{
    // off 指向 '$'，行内容是长度
    size_t lineStart = off + 1;
    size_t lineOff = lineStart;
    size_t contentEnd = 0;
    ParseStatus st = scanLine(b, n, lineOff, contentEnd);
    if (st != ParseStatus::Ok) {
        return st;
    }

    long long len = 0;
    if (!parseInt64(b + lineStart, contentEnd - lineStart, len)) {
        return ParseStatus::Error;
    }
    if (len == -1) {
        off = lineOff;
        out = RespValue{}; // Null
        return ParseStatus::Ok;
    }
    if (len < -1 || static_cast<unsigned long long>(len) > _maxBulkLen) {
        return ParseStatus::Error;
    }

    size_t body = static_cast<size_t>(len);
    // 体按长度定界（二进制安全），尾部严格 \r\n
    if (n - lineOff < body + 2) {
        return ParseStatus::NeedMore;
    }
    if (b[lineOff + body] != '\r' || b[lineOff + body + 1] != '\n') {
        return ParseStatus::Error;
    }

    RespValue tmp;
    tmp.type = RespValue::Type::BulkString;
    tmp.str.assign(b + lineOff, body);
    off = lineOff + body + 2;
    out = std::move(tmp);
    return ParseStatus::Ok;
}

ParseStatus RespParser::parseArray(const char *b, size_t n, size_t &off,
                                   RespValue &out, bool &got, size_t depth)
{
    if (depth > _maxDepth) {
        return ParseStatus::Error; // 数组嵌套深度超上限
    }
    // off 指向 '*'，行内容是元素个数
    size_t lineStart = off + 1;
    size_t lineOff = lineStart;
    size_t contentEnd = 0;
    ParseStatus st = scanLine(b, n, lineOff, contentEnd);
    if (st != ParseStatus::Ok) {
        return st;
    }

    long long count = 0;
    if (!parseInt64(b + lineStart, contentEnd - lineStart, count)) {
        return ParseStatus::Error;
    }
    if (count == -1) {
        off = lineOff;
        out = RespValue{}; // Null（空数组 *0 是合法帧，不是 Null）
        return ParseStatus::Ok;
    }
    if (count < -1 || static_cast<unsigned long long>(count) > _maxArrayLen) {
        return ParseStatus::Error;
    }

    RespValue tmp;
    tmp.type = RespValue::Type::Array;
    size_t cur = lineOff;
    for (long long i = 0; i < count; ++i) {
        RespValue elem;
        bool elemGot = true;
        st = parseValue(b, n, cur, elem, elemGot, depth + 1);
        if (st != ParseStatus::Ok) {
            return st; // NeedMore/Error 原样上抛，本帧整体不消费
        }
        if (!elemGot) {
            return ParseStatus::Error; // 数组元素位置不接受空行
        }
        tmp.array.push_back(std::move(elem));
    }
    off = cur;
    out = std::move(tmp);
    return ParseStatus::Ok;
}

// 行式 inline 命令：按空白切分为 BulkString 数组（parser 归一化，CALL-CONVENTIONS §7.2）
ParseStatus RespParser::parseInline(const char *b, size_t n, size_t &off,
                                    RespValue &out, bool &got)
{
    size_t lineOff = off; // inline 无前缀，从 off 起就是内容
    size_t contentEnd = 0;
    ParseStatus st = scanLine(b, n, lineOff, contentEnd);
    if (st != ParseStatus::Ok) {
        return st;
    }

    RespValue tmp;
    tmp.type = RespValue::Type::Array;
    size_t i = off;
    while (i < contentEnd) {
        while (i < contentEnd && (b[i] == ' ' || b[i] == '\t')) {
            ++i;
        }
        size_t tok = i;
        while (i < contentEnd && b[i] != ' ' && b[i] != '\t') {
            ++i;
        }
        if (i > tok) {
            RespValue arg;
            arg.type = RespValue::Type::BulkString;
            arg.str.assign(b + tok, i - tok);
            tmp.array.push_back(std::move(arg));
        }
    }
    off = lineOff;
    if (tmp.array.empty()) {
        got = false; // 纯空白行：跳过
        return ParseStatus::Ok;
    }
    out = std::move(tmp);
    return ParseStatus::Ok;
}

ParseStatus RespParser::scanLine(const char *b, size_t n, size_t &off, size_t &contentEnd)
{
    size_t i = off;
    while (i < n && b[i] != '\n') {
        if (i - off > kMaxLine) {
            return ParseStatus::Error;
        }
        ++i;
    }
    if (i >= n) {
        return (n - off > kMaxLine) ? ParseStatus::Error : ParseStatus::NeedMore;
    }
    contentEnd = i;
    if (contentEnd > off && b[contentEnd - 1] == '\r') {
        --contentEnd; // 行尾 \r\n 的 \r 不属于内容
    }
    off = i + 1;
    return ParseStatus::Ok;
}

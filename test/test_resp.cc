/**
 * @file test_resp.cc
 * @brief protocol 层单测：RespEncoder / RespParser（增量、半包、粘包、inline、
 * 二进制安全、嵌套数组、残帧截断偏移、协议错误 vs 没到齐、上限保护）。
 * 纯 CHECK 宏 + ctest，不建 socket（CALL-CONVENTIONS §13）。
 */

#include <climits>
#include <string>
#include <utility>
#include <vector>

#include "RespEncoder.h"
#include "RespParser.h"
#include "RespValue.hpp"
#include "test_util.hpp"

namespace {

// 统一走 string::size() 传长度，杜绝手数字节数越界
void feedStr(RespParser &p, const std::string &s)
{
    p.feed(s.data(), s.size());
}

RespValue mkSimple(const std::string &s)
{
    RespValue v;
    v.type = RespValue::Type::SimpleString;
    v.str = s;
    return v;
}

RespValue mkError(const std::string &s)
{
    RespValue v;
    v.type = RespValue::Type::Error;
    v.str = s;
    return v;
}

RespValue mkInt(long long i)
{
    RespValue v;
    v.type = RespValue::Type::Integer;
    v.integer = i;
    return v;
}

RespValue mkBulk(const std::string &s)
{
    RespValue v;
    v.type = RespValue::Type::BulkString;
    v.str = s;
    return v;
}

RespValue mkNull()
{
    return RespValue{};
}

RespValue mkArray(std::vector<RespValue> elems)
{
    RespValue v;
    v.type = RespValue::Type::Array;
    v.array = std::move(elems);
    return v;
}

bool same(const RespValue &a, const RespValue &b)
{
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
    case RespValue::Type::SimpleString:
    case RespValue::Type::Error:
    case RespValue::Type::BulkString:
        return a.str == b.str;
    case RespValue::Type::Integer:
        return a.integer == b.integer;
    case RespValue::Type::Null:
        return true;
    case RespValue::Type::Array:
        if (a.array.size() != b.array.size()) {
            return false;
        }
        for (size_t i = 0; i < a.array.size(); ++i) {
            if (!same(a.array[i], b.array[i])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- 编码

void testEncode()
{
    // 字符串比较用 CHECK(a == b)：CHECK_EQ 的打印走 (long long) 强转
    CHECK(RespEncoder::simple("OK") == "+OK\r\n");
    CHECK(RespEncoder::error("ERR x") == "-ERR x\r\n");
    CHECK(RespEncoder::integer(42) == ":42\r\n");
    CHECK(RespEncoder::integer(-7) == ":-7\r\n");
    CHECK(RespEncoder::bulk("hello") == "$5\r\nhello\r\n");
    CHECK(RespEncoder::bulk("") == "$0\r\n\r\n");
    CHECK(RespEncoder::nullBulk() == "$-1\r\n");
    CHECK(RespEncoder::arrayHeader(2) == "*2\r\n");

    // 二进制安全：value 含 \0，长度定界
    std::string bin("a\0b", 3);
    CHECK(RespEncoder::bulk(bin) == std::string("$3\r\na\0b\r\n", 9));

    // Null 编码为 $-1
    CHECK(RespEncoder::encode(mkNull()) == "$-1\r\n");

    // 嵌套数组整体编码
    RespValue v = mkArray({mkInt(1), mkBulk("foo")});
    CHECK(RespEncoder::encode(v) == "*2\r\n:1\r\n$3\r\nfoo\r\n");

    RespValue nested = mkArray({mkArray({mkInt(1), mkInt(2)}), mkNull()});
    CHECK(RespEncoder::encode(nested) == "*2\r\n*2\r\n:1\r\n:2\r\n$-1\r\n");
}

// ---------------------------------------------------------------- 基本类型解析

void testParseTypes()
{
    RespParser p;
    RespValue v;

    feedStr(p, "+OK\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkSimple("OK")));
    CHECK(p.next(v) == ParseStatus::NeedMore);

    feedStr(p, "-ERR unknown command 'x'\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkError("ERR unknown command 'x'")));

    feedStr(p, ":42\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkInt(42)));

    feedStr(p, ":-7\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkInt(-7)));

    feedStr(p, ":9223372036854775807\r\n"); // LLONG_MAX
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkInt(LLONG_MAX)));

    feedStr(p, "$5\r\nhello\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkBulk("hello")));

    feedStr(p, "$0\r\n\r\n"); // 空 BulkString ≠ Null
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(v.type == RespValue::Type::BulkString);
    CHECK(v.str.empty());

    feedStr(p, "$-1\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(v.type == RespValue::Type::Null);

    feedStr(p, "*0\r\n"); // 空数组 ≠ Null
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(v.type == RespValue::Type::Array);
    CHECK_EQ(v.array.size(), (size_t)0);

    feedStr(p, "*3\r\n:1\r\n$3\r\nfoo\r\n+OK\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkInt(1), mkBulk("foo"), mkSimple("OK")})));
}

void testParseNested()
{
    RespParser p;
    RespValue v;

    // 嵌套数组：*2 [ *2 [:1 :2], *1 [$3 bar] ]
    std::string frame = "*2\r\n*2\r\n:1\r\n:2\r\n*1\r\n$3\r\nbar\r\n";
    feedStr(p, frame);
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkArray({mkInt(1), mkInt(2)}),
                           mkArray({mkBulk("bar")})})));

    // encode → parse 往返一致（含 Null、二进制）
    RespValue deep = mkArray({mkArray({mkArray({mkBulk(std::string("a\0b", 3))}),
                                       mkNull()}),
                              mkInt(-1)});
    std::string bytes = RespEncoder::encode(deep);
    RespParser q;
    feedStr(q, bytes);
    RespValue back;
    CHECK(q.next(back) == ParseStatus::Ok);
    CHECK(same(back, deep));
    CHECK(q.next(back) == ParseStatus::NeedMore);
}

// ---------------------------------------------------------------- 半包：帧原子

void testHalfPacket()
{
    // 逐字节喂一帧：最后一字节之前一律 NeedMore，且不消费任何字节
    const std::string frame = "$5\r\nhello\r\n";
    RespParser p;
    RespValue v;
    for (size_t i = 0; i + 1 < frame.size(); ++i) {
        p.feed(&frame[i], 1);
        CHECK(p.next(v) == ParseStatus::NeedMore);
        CHECK_EQ(p.consumed(), (size_t)0);
        CHECK_EQ(p.buffered(), i + 1);
    }
    p.feed(&frame.back(), 1);
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkBulk("hello")));
    CHECK_EQ(p.consumed(), frame.size());
    CHECK_EQ(p.buffered(), (size_t)0);

    // 各种"没到齐"都必须是 NeedMore 而不是 Error
    RespParser q;
    feedStr(q, "+OK\r");
    CHECK(q.next(v) == ParseStatus::NeedMore);
    feedStr(q, "\n");
    CHECK(q.next(v) == ParseStatus::Ok);

    feedStr(q, ":1");
    CHECK(q.next(v) == ParseStatus::NeedMore);
    feedStr(q, "\r\n");
    CHECK(q.next(v) == ParseStatus::Ok);

    feedStr(q, "$5\r\nhel"); // BulkString 体没到齐
    CHECK(q.next(v) == ParseStatus::NeedMore);
    feedStr(q, "lo\r");
    CHECK(q.next(v) == ParseStatus::NeedMore); // 尾部 \r\n 未齐
    feedStr(q, "\n");
    CHECK(q.next(v) == ParseStatus::Ok);

    feedStr(q, "*1\r\n$1\r\nx"); // 数组帧没到齐（原子）
    CHECK(q.next(v) == ParseStatus::NeedMore);
    CHECK_EQ(q.buffered(), (size_t)9);
    feedStr(q, "\r\n");
    CHECK(q.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkBulk("x")})));

    // 整帧拆成两半跨 feed
    RespParser r;
    std::string f = "*2\r\n:1\r\n$3\r\nfoo\r\n";
    r.feed(f.data(), 6);
    CHECK(r.next(v) == ParseStatus::NeedMore);
    r.feed(f.data() + 6, f.size() - 6);
    CHECK(r.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkInt(1), mkBulk("foo")})));
}

// ---------------------------------------------------------------- 粘包：一次多帧

void testSticky()
{
    RespParser p;
    RespValue v;

    std::string three = ":1\r\n:2\r\n:3\r\n";
    feedStr(p, three);
    for (int i = 1; i <= 3; ++i) {
        CHECK(p.next(v) == ParseStatus::Ok);
        CHECK(same(v, mkInt(i)));
    }
    CHECK(p.next(v) == ParseStatus::NeedMore);

    // 粘包 + 半包混合：两帧半
    std::string mix = "$3\r\nfoo\r\n+OK\r\n$4\r\nba";
    feedStr(p, mix);
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkBulk("foo")));
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkSimple("OK")));
    CHECK(p.next(v) == ParseStatus::NeedMore);
    CHECK_EQ(p.buffered(), (size_t)6); // "$4\r\nba" 半帧留下
}

// ---------------------------------------------------------------- inline 命令

void testInline()
{
    RespParser p;
    RespValue v;

    feedStr(p, "PING\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkBulk("PING")})));

    feedStr(p, "SET foo bar\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkBulk("SET"), mkBulk("foo"), mkBulk("bar")})));

    // 多余空白/Tab 归一化
    feedStr(p, "SET  foo\tbar\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkBulk("SET"), mkBulk("foo"), mkBulk("bar")})));

    // 裸 \n 行尾（telnet 友好）
    feedStr(p, "GET k\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkBulk("GET"), mkBulk("k")})));

    // 空行/纯空白行跳过，不产出帧
    feedStr(p, "\r\n \t\r\nPING\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkBulk("PING")})));

    // inline 与 RESP 帧混流
    feedStr(p, "PING\r\n:9\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkBulk("PING")})));
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkInt(9)));

    // 半个 inline 行
    feedStr(p, "PI");
    CHECK(p.next(v) == ParseStatus::NeedMore);
    feedStr(p, "NG\r\n");
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkArray({mkBulk("PING")})));
}

// ---------------------------------------------------------------- 二进制安全

void testBinary()
{
    RespParser p;
    RespValue v;

    // BulkString 体含 \0、\r、\n——按长度定界，不能被截断
    std::string body("a\0b\r\nc", 6);
    std::string frame = "$6\r\n" + body + "\r\n";
    feedStr(p, frame);
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(v.type == RespValue::Type::BulkString);
    CHECK_EQ(v.str.size(), (size_t)6);
    CHECK(v.str == body);
    CHECK_EQ((int)v.str[1], 0);

    // 数组里嵌二进制 BulkString，encode → parse 往返
    RespValue v1 = mkArray({mkBulk(body), mkBulk(std::string("x\0y", 3))});
    std::string bytes = RespEncoder::encode(v1);
    RespParser q;
    feedStr(q, bytes);
    RespValue back;
    CHECK(q.next(back) == ParseStatus::Ok);
    CHECK(same(back, v1));
}

// ---------------------------------------------------------------- 协议错误（≠ 没到齐）

void testErrors()
{
    const std::string bad[] = {
        ":abc\r\n",                  // 整数非法
        ":5x\r\n",                   // 整数带尾巴
        ":\r\n",                     // 空整数
        ":+\r\n",                    // 只有符号
        ":9223372036854775808\r\n",  // LLONG_MAX+1 溢出
        "$abc\r\n",                  // 长度非法
        "$-2\r\n",                   // 非法负长度
        "*-2\r\n",                   // 非法负计数
        "$3\r\nabcXY",               // 体尾不是 \r\n
        "$3\r\nabc\nX",              // 体尾 \n 不接受（BulkString 尾部严格 \r\n）
        "*1\r\n:abc\r\n",            // 元素非法
        "*2\r\n:1\r\n \r\n",         // 元素位置不接受空行
        "\rX",                       // \r 后不是 \n
        "$5\r\nhel\n\nXY",           // 体尾 \r\n 错位（体 5 字节 "hel\n\n"）
    };
    for (const std::string &s : bad) {
        RespParser p;
        RespValue v;
        feedStr(p, s);
        CHECK(p.next(v) == ParseStatus::Error);
        // Error 后状态不可继续使用
        CHECK(p.next(v) == ParseStatus::Error);
    }

    // Error 之后即使喂了合法数据也不复活
    RespParser p;
    RespValue v;
    feedStr(p, ":x\r\n");
    CHECK(p.next(v) == ParseStatus::Error);
    feedStr(p, "+OK\r\n");
    CHECK(p.next(v) == ParseStatus::Error);
}

// ---------------------------------------------------------------- 上限保护

void testLimits()
{
    RespValue v;

    { // BulkString 长度超上限：头部一到就 Error，不必等数据
        RespParser p(10, 1 << 16, 32);
        feedStr(p, "$100\r\n");
        CHECK(p.next(v) == ParseStatus::Error);
    }
    { // 数组个数超上限
        RespParser p(64 << 20, 4, 32);
        feedStr(p, "*5\r\n");
        CHECK(p.next(v) == ParseStatus::Error);
    }
    { // 嵌套深度超上限（3 层，maxDepth=2）
        RespParser p(64 << 20, 1 << 16, 2);
        feedStr(p, "*1\r\n*1\r\n*1\r\n:1\r\n");
        CHECK(p.next(v) == ParseStatus::Error);
    }
    { // 同样深度不超限则成功
        RespParser p(64 << 20, 1 << 16, 3);
        feedStr(p, "*1\r\n*1\r\n*1\r\n:1\r\n");
        CHECK(p.next(v) == ParseStatus::Ok);
    }
    { // 行式字段超长（>64KB 无行尾）→ Error
        RespParser p;
        std::string s(70000, 'a');
        feedStr(p, s);
        CHECK(p.next(v) == ParseStatus::Error);
    }
}

// ---------------------------------------------------------------- 残帧截断偏移（P5 AOF 用）

void testConsumedAndBuffered()
{
    RespParser p;
    RespValue v;

    std::string f1 = ":1\r\n";
    std::string f2 = "$3\r\nfoo\r\n";
    std::string f3 = "$5\r\nhel"; // 残帧
    std::string all = f1 + f2 + f3;
    feedStr(p, all);

    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(p.next(v) == ParseStatus::Ok);
    CHECK(p.next(v) == ParseStatus::NeedMore);
    // 最后一个完整帧的结束偏移 = consumed；残帧留在缓冲
    CHECK_EQ(p.consumed(), f1.size() + f2.size());
    CHECK_EQ(p.buffered(), f3.size());

    // 逐字节喂完，consumed 累计到整帧长度
    RespParser q;
    std::string frame = "*2\r\n$1\r\na\r\n:2\r\n";
    for (size_t i = 0; i < frame.size(); ++i) {
        q.feed(&frame[i], 1);
    }
    CHECK(q.next(v) == ParseStatus::Ok);
    CHECK_EQ(q.consumed(), frame.size());
    CHECK_EQ(q.buffered(), (size_t)0);

    // 空行也被计入 consumed（truncate 到 consumed 后可直接回放）
    RespParser r;
    std::string withBlank = "\r\n" + f1;
    feedStr(r, withBlank);
    CHECK(r.next(v) == ParseStatus::Ok);
    CHECK(same(v, mkInt(1)));
    CHECK_EQ(r.consumed(), withBlank.size());
}

// ---------------------------------------------------------------- 其他边界

void testMisc()
{
    RespParser p;
    RespValue v;
    CHECK(p.next(v) == ParseStatus::NeedMore); // 空解析器
    p.feed(nullptr, 0);                        // 空 feed 无害
    p.feed("", 0);
    CHECK(p.next(v) == ParseStatus::NeedMore);
    CHECK_EQ(p.buffered(), (size_t)0);
    CHECK_EQ(p.consumed(), (size_t)0);
}

} // namespace

int main()
{
    testEncode();
    testParseTypes();
    testParseNested();
    testHalfPacket();
    testSticky();
    testInline();
    testBinary();
    testErrors();
    testLimits();
    testConsumedAndBuffered();
    testMisc();
    std::printf("test_resp OK\n");
    return 0;
}

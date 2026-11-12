/**
 * @file test_buffer.cc
 * @brief Buffer 单元测试：三区模型的边界行为。
 *
 * 重点回归：
 *  - next() 允许恰好填满到容量边界（旧差一错误：>= 会拒绝推进，丢字节）；
 *  - enlarge(atLeast) 一次调用保证可用区域 >= atLeast；
 *  - erase 后游标推进、清空后回卷到头部；重整/扩容后数据不丢。
 */

#include <cstring>
#include <string>

#include "Buffer.hpp"
#include "test_util.hpp"

int main()
{
    // next()：恰好填满成功；越界拒绝推进且不动游标
    {
        Buffer b(4);
        CHECK(b.next(4));
        CHECK_EQ(b.length(), 4u);
        CHECK_EQ(b.remain(), 0u);
        CHECK(!b.next(1)); // 超出容量
        CHECK_EQ(b.length(), 4u); // 游标不动
    }

    // enlarge(atLeast)：一次调用保证放得下（sendOutput 路径）
    {
        Buffer b(8);
        std::string msg(100, 'x');
        b.enlarge(msg.size());
        CHECK(b.remain() >= msg.size());
        std::memcpy(b.space(), msg.data(), msg.size());
        CHECK(b.next(msg.size()));
        CHECK_EQ(b.length(), msg.size());
        CHECK(std::memcmp(b.start(), msg.data(), msg.size()) == 0);
    }

    // erase：游标推进；清空后回卷到头部
    {
        Buffer b(16);
        std::memcpy(b.space(), "abcdef", 6);
        CHECK(b.next(6));
        CHECK(b.erase(2));
        CHECK_EQ(b.length(), 4u);
        CHECK(std::memcmp(b.start(), "cdef", 4) == 0);
        CHECK(b.erase(4));
        CHECK_EQ(b.length(), 0u);
        CHECK_EQ(b.remain(), 16u); // 清空后从头开始
    }

    // erase 越界应失败且不动数据
    {
        Buffer b(8);
        std::memcpy(b.space(), "ab", 2);
        CHECK(b.next(2));
        CHECK(!b.erase(3));
        CHECK_EQ(b.length(), 2u);
    }

    // 大块数据：连续两块（第二块触发重整/扩容），内容顺序不乱
    {
        Buffer b(32);
        std::string big(1 << 16, 'A');
        std::string big2(1 << 15, 'B');
        if (b.remain() < big.size())
            b.enlarge(big.size());
        std::memcpy(b.space(), big.data(), big.size());
        CHECK(b.next(big.size()));

        if (b.remain() < big2.size())
            b.enlarge(big2.size());
        std::memcpy(b.space(), big2.data(), big2.size());
        CHECK(b.next(big2.size()));

        CHECK_EQ(b.length(), big.size() + big2.size());
        CHECK(std::memcmp(b.start(), big.data(), big.size()) == 0);
        CHECK(std::memcmp(b.start() + big.size(), big2.data(), big2.size()) == 0);
    }

    std::printf("test_buffer ALL PASS\n");
    return 0;
}

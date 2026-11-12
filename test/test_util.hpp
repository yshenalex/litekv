#ifndef __TEST_UTIL_HPP__
#define __TEST_UTIL_HPP__

/**
 * @file test_util.hpp
 * @brief 极简测试宏：失败打印位置并以非零码退出，交给 ctest 判定。
 * 不引入测试框架依赖（DEV-GUIDE：纯 assert + ctest 即可）。
 */

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                             \
    do                                                                          \
    {                                                                           \
        if (!(cond))                                                            \
        {                                                                       \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, \
                         __LINE__);                                             \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do                                                                          \
    {                                                                           \
        auto va = (a);                                                          \
        auto vb = (b);                                                          \
        if (!(va == vb))                                                        \
        {                                                                       \
            std::fprintf(stderr,                                                \
                         "CHECK_EQ failed: %s == %s, got %lld vs %lld (%s:%d)\n", \
                         #a, #b, (long long)va, (long long)vb, __FILE__,        \
                         __LINE__);                                             \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

#endif

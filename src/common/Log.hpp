#ifndef __LOG_HPP__
#define __LOG_HPP__

/**
 * @file Log.hpp
 * @brief 线程安全的轻量日志。
 *
 * 设计要点：
 *  - 时间格式化用 localtime_r（localtime 返回静态缓冲区，多线程并发会串时间）；
 *  - 非 __DEBUG__ 构建下 DEBUG 级调用被宏在编译期整行剔除（连参数求值都没有），
 *    日志不会成为压测瓶颈；
 *  - 日志文件懒打开一次后复用，不做每次调用都 fopen 的事。
 *
 * 输出目标：
 *  - __DEBUG__ 构建：stdout / stderr，保留 DEBUG 级；
 *  - 非 __DEBUG__ 构建：NORMAL/WARNING → log.txt，ERROR/FATAL → log.error。
 */

#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <unistd.h>

#define DEBUG 0
#define NORMAL 1
#define WARNING 2
#define ERROR 3
#define FATAL 4

inline const char *to_levelstr(int level)
{
    switch (level)
    {
    case DEBUG:
        return "DEBUG";
    case NORMAL:
        return "NORMAL";
    case WARNING:
        return "WARNING";
    case ERROR:
        return "ERROR";
    case FATAL:
        return "FATAL";
    default:
        return nullptr;
    }
}

inline void logMessageImpl(int level, const char *format, ...)
{
    char time_str[80];
    time_t rawtime = ::time(nullptr);
    struct tm timeinfo;
    localtime_r(&rawtime, &timeinfo); // 线程安全
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

    char logprefix[256];
    snprintf(logprefix, sizeof(logprefix), "[%s][%s][pid:%d]", to_levelstr(level), time_str, getpid());

    char logcontent[1024];
    va_list arg;
    va_start(arg, format);
    vsnprintf(logcontent, sizeof(logcontent), format, arg);
    va_end(arg);

#ifdef __DEBUG__
    FILE *cur = (level == ERROR || level == FATAL) ? stderr : stdout;
#else
    // 懒打开一次后复用；此路径永远不会被 DEBUG 级触发（宏已剔除）
    static FILE *log = ::fopen("log.txt", "a");
    static FILE *err = ::fopen("log.error", "a");
    FILE *cur = (level == ERROR || level == FATAL) ? err : log;
#endif
    if (cur != nullptr)
        fprintf(cur, "%s %s\n", logprefix, logcontent);
}

#ifdef __DEBUG__
#define logMessage(level, ...) logMessageImpl((level), __VA_ARGS__)
#else
// 非 DEBUG 构建：level 为 DEBUG 的调用连参数求值都被编译期剔除
// （`level != DEBUG` 是编译期常量假，整段成死代码），零开销
#define logMessage(level, ...)                    \
    do                                            \
    {                                             \
        if ((level) != DEBUG)                     \
            logMessageImpl((level), __VA_ARGS__); \
    } while (0)
#endif

#endif

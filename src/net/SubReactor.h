#ifndef __SUBREACTOR_H__
#define __SUBREACTOR_H__

#include <functional>
#include <future>
#include <mutex>
#include <thread>

class EventLoop;

/**
 * @brief 一个 SubReactor：一个线程 + 线程独占的 EventLoop。
 *
 * EventLoop 对象由新线程创建并运行，保证"一个 loop 只在一个线程里跑"；
 * startLoop() 阻塞到 loop 创建好后返回其指针（promise 同步）。
 */
class SubReactor
{
    using Functor = std::function<void(void)>;

public:
    SubReactor();
    ~SubReactor();

    // 启动线程，阻塞等到新线程把 EventLoop 创建好，返回该 loop 指针
    EventLoop *startLoop();

    // 线程安全退出：让本线程的 loop quit 并 join（可重复调用）
    void quit();

private:
    SubReactor(const SubReactor &) = delete;
    SubReactor &operator=(const SubReactor &) = delete;

    void threadFunc(std::promise<EventLoop *> &ready);

private:
    std::thread _thread;
    EventLoop *_loop = nullptr; // 由新线程创建/销毁，主侧只读取
    std::mutex _mutex;          // 保护 _loop 的可见性
};

#endif

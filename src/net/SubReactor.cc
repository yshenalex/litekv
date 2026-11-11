#include "SubReactor.h"
#include <utility>
#include "EventLoop.h"

SubReactor::SubReactor()
{
}

SubReactor::~SubReactor()
{
    quit();
}

EventLoop *SubReactor::startLoop()
{
    std::promise<EventLoop *> ready;
    std::future<EventLoop *> fut = ready.get_future();
    _thread = std::thread([this, &ready] { threadFunc(ready); });

    return fut.get(); // 阻塞到新线程把 loop 创建好
}

void SubReactor::threadFunc(std::promise<EventLoop *> &ready)
{
    EventLoop loop; // loop 对象由本线程创建、本线程运行、本线程销毁
    {
        std::lock_guard<std::mutex> lg(_mutex);
        _loop = &loop;
    }
    ready.set_value(&loop); // 通知 startLoop()：loop 已就绪（此后不再触碰 promise）

    loop.loop(); // 进入事件循环，直到 quit()

    std::lock_guard<std::mutex> lg(_mutex);
    _loop = nullptr;
}

void SubReactor::quit()
{
    EventLoop *loop = nullptr;
    {
        std::lock_guard<std::mutex> lg(_mutex);
        loop = _loop;
    }
    if (loop != nullptr)
        loop->quit(); // 线程安全：置标志 + 写 eventfd 唤醒

    if (_thread.joinable())
        _thread.join();
}

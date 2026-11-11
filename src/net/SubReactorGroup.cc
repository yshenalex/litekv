#include "SubReactorGroup.h"
#include <cassert>
#include "SubReactor.h"
#include "Log.hpp"

SubReactorGroup::SubReactorGroup(size_t threadNum)
{
    _threads.reserve(threadNum);
    _loops.reserve(threadNum);
    for (size_t i = 0; i < threadNum; ++i)
    {
        _threads.emplace_back(new SubReactor());
    }
}

SubReactorGroup::~SubReactorGroup()
{
    quit();
}

void SubReactorGroup::start()
{
    for (auto &t : _threads)
    {
        _loops.push_back(t->startLoop()); // 阻塞到该线程的 loop 就绪
    }
    logMessage(NORMAL, "SubReactorGroup started, %zu sub-reactor(s)", _loops.size());
}

EventLoop *SubReactorGroup::getNextLoop()
{
    if (_loops.empty())
    {
        // N=0 是单 Reactor 退化模式，调用方不得走到这里
        return nullptr;
    }
    size_t i = _next.fetch_add(1, std::memory_order_relaxed) % _loops.size();
    return _loops[i];
}

size_t SubReactorGroup::size() const
{
    return _loops.size();
}

void SubReactorGroup::quit()
{
    for (auto &t : _threads)
    {
        t->quit(); // quit + join
    }
}

const std::vector<EventLoop *> &SubReactorGroup::loops() const
{
    return _loops;
}

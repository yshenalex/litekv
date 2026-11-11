#ifndef __SUBREACTORGROUP_H__
#define __SUBREACTORGROUP_H__

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

class EventLoop;
class SubReactor;

/**
 * @brief SubReactor 组：管理 N 个 SubReactor（各自独占一个 EventLoop），round-robin 取下一个 loop。
 *
 * 允许 N=0（空池，单 Reactor 退化模式），此时不得调用 getNextLoop()。
 * 连接亲和：一条连接固定绑一个 SubReactor，由调用方在 accept 后选定。
 */
class SubReactorGroup
{
public:
    explicit SubReactorGroup(size_t threadNum);
    ~SubReactorGroup();

    // 启动全部线程，阻塞到所有 loop 创建就绪（threadNum == 0 时是空操作）
    void start();
    // round-robin 取下一个 loop；threadNum == 0 时禁止调用（返回 nullptr）
    EventLoop *getNextLoop();
    // SubReactor 数量（0 = 单 Reactor 退化模式）
    size_t size() const;
    // 全部 loop quit 并 join 全部线程（可重复调用）
    void quit();

    // 只读遍历所有 loop（server 接线用）
    const std::vector<EventLoop *> &loops() const;

private:
    SubReactorGroup(const SubReactorGroup &) = delete;
    SubReactorGroup &operator=(const SubReactorGroup &) = delete;

private:
    std::vector<std::unique_ptr<SubReactor>> _threads;
    std::vector<EventLoop *> _loops;
    std::atomic<size_t> _next{0}; // round-robin 下标
};

#endif

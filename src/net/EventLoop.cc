#include "EventLoop.h"
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <cerrno>
#include <cstdio>
#include "Socket.h"
#include "TcpConnection.h"
#include "Log.hpp"

EventLoop::EventLoop()
    : _epfd(createEpfd()), _isLooping(true), _events(eventNum), _evfd(createEvfd())
{
    Socket::setNonBlock(_evfd); // 用于线程间通信的文件描述符也加入epoll红黑树
    addEpReadfd(_evfd);
}
EventLoop::~EventLoop()
{
    close(_epfd);
    close(_evfd);
}

int EventLoop::createEpfd()
{
    int fd = ::epoll_create(1);
    if (-1 == fd)
    {
        perror("epoll_create failed");
    }
    return fd;
}

void EventLoop::addEpReadfd(int fd)
{
    logMessage(DEBUG, "in addEPReadfd, fd = %d", fd);

    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLET;

    // 将文件描述符fd加入epoll内核红黑树数据结构监听
    int ret = ::epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &ev);
    if (-1 == ret)
    {
        perror("epoll_ctl failded(EPOLL_CTL_ADD:EPOLLIN)");
        return;
    }

    logMessage(DEBUG, "out addEPReadfd, fd = %d", fd);
}

void EventLoop::pushWriteEvent(int fd)
{
    logMessage(DEBUG, "in EventLoop::pushWriteEvent, fd = %d", fd);
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET; // 关心写事件一般还是会关心读事件

    if (-1 == ::epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &ev))
    {
        perror("epoll_ctl failed(EPOLL_CTL_MOD:EPOLLOUT)");
        return;
    }

    logMessage(DEBUG, "out EventLoop::pushWriteEvent, fd = %d", fd);
}

void EventLoop::popWriteEvent(int fd)
{
    logMessage(DEBUG, "in EventLoop::popWriteEvent, fd = %d", fd);
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLET; // 取消写事件关心

    if (-1 == ::epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &ev))
    {
        perror("epoll_ctl failed(EPOLL_CTL_MOD:EPOLLOUT)");
        return;
    }

    logMessage(DEBUG, "out EventLoop::popWriteEvent, fd = %d", fd);
}

void EventLoop::delEpEventfd(int fd)
{
    // 将文件描述符fd从epoll内核红黑树中移除，取消监听
    int ret = ::epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr); // Linux 2.6.8后，删除时可以传入nullptr
    if (-1 == ret)
    {
        perror("epoll_ctl failed(EPOLL_CTL_DEL)");
        return;
    }
}

void EventLoop::waitEpfd()
{
    int nready = 0; // 就绪事件个数
    do
    {
        nready = ::epoll_wait(_epfd, &_events[0], static_cast<int>(_events.size()), timeout);
    } while (-1 == nready && errno == EINTR);

    if (-1 == nready)
    {
        perror("epoll_wait failed");
        return;
    }
    else if (0 == nready)
    {
        // 返回0是正常行为，在timeout时间内没有事件就绪
        logMessage(DEBUG, "epoll_wait timeout...");
        return;
    }

    if (nready == static_cast<int>(_events.size()))
    {
        _events.resize(2 * nready); // 给_events扩容，允许容纳更多就绪事件
    }

    for (int i = 0; i < nready; i++)
    {
        int fd = _events[i].data.fd;
        uint32_t events = _events[i].events;
        logMessage(DEBUG, "wait one success, fd = %d", fd);

        if (fd == _evfd) // 线程通信fd：执行_pendings里排队的任务
        {
            readEvfd();
            doPendings();
            continue;
        }

        // 监听 fd 等非连接 fd（新连接到来、timerfd 到期等）
        auto ioIt = _ioHandlers.find(fd);
        if (ioIt != _ioHandlers.end())
        {
            if (events & (EPOLLIN | EPOLLRDHUP))
                ioIt->second();
            continue;
        }

        auto it = _connections.find(fd);
        if (it == _connections.end())
        {
            logMessage(WARNING, "fd = %d not in _connections", fd);
            continue;
        }

        if (events & (EPOLLHUP | EPOLLERR))
        {
            // 对端异常断开：直接走关闭路径，否则事件会一直就绪、epoll_wait 立即返回空转
            closeConnection(fd);
            continue;
        }

        // 先冲刷积压的发送数据再处理读：EPOLLOUT 不能被读事件饿死；
        // ET 模式下这一轮不处理 EPOLLOUT，写边沿可能永久丢失。
        // 读与写都要处理，不用互斥的 else if。
        if (events & EPOLLOUT)
            handlerRemainSend(fd);

        // 发送冲刷可能已关闭连接（写出错），确认还在连接表再处理读
        if ((events & (EPOLLIN | EPOLLRDHUP)) && _connections.count(fd))
            handlerMessage(fd);
    }
}

void EventLoop::handlerMessage(int fd)
{
    logMessage(DEBUG, "in handlerMessage, fd = %d", fd);

    auto it = _connections.find(fd);
    if (it == _connections.end()) // 没有到末尾就说明找到了
    {
        logMessage(WARNING, "fd = %d not in _connections", fd);
        return;
    }
    TcpConnectionPtr tp = it->second;

    if (!tp->receive())
    {
        // 读路径遇 EOF（对端半关闭）或出错：本次已读入的数据先交给上层处理，
        // 再"输出冲刷完即关闭"（缓冲为空则立即关闭）；receive() 内部已关闭的（如输入
        // 超限）不再投递
        if (tp->connected() && tp->inputBuffer().length() > 0)
            tp->handleOnMessageCallback();
        tp->closeAfterFlush();
        logMessage(NORMAL, "connection [fd = %d] EOF, closing", fd);
        return;
    }

    tp->handleOnMessageCallback();
    logMessage(DEBUG, "out handlerMessage, fd = %d", fd);
}

void EventLoop::handlerRemainSend(int fd)
{
    // 写事件就绪：把 TcpConnection 输出缓冲区里剩余的数据冲刷出去
    logMessage(DEBUG, "in handlerRemainSend, fd = %d", fd);
    auto it = _connections.find(fd);
    if (it != _connections.end())
    {
        it->second->handleWrite(); // 内部处理发完后的取消写事件/延迟关闭
    }

    logMessage(DEBUG, "out handlerRemainSend, fd = %d", fd);
}

void EventLoop::closeConnection(int fd)
{
    auto it = _connections.find(fd);
    if (it == _connections.end())
        return;

    TcpConnectionPtr tp = it->second;
    tp->markClosed();       // 标记连接已失效
    delEpEventfd(fd);       // 先从epoll中取消监听
    _connections.erase(it); // 再从连接池中清除
    logMessage(NORMAL, "erase TcpConnection fd = %d from _connections, loop = %p", fd, static_cast<void *>(this));
    tp->handleOnCloseCallback(); // 最后触发断开回调（tp 仍持有对象，安全）
}

void EventLoop::loop()
{
    _tid = std::this_thread::get_id();
    while (_isLooping.load())
    {
        waitEpfd();
    }
}

void EventLoop::quit()
{
    // 线程安全：置原子标志后写 eventfd，让阻塞中的 epoll_wait 立刻返回，
    // 不用等 epoll_wait 的 timeout（3s）超时
    _isLooping.store(false);
    writeEvfd();
}

bool EventLoop::isInLoopThread() const
{
    return std::this_thread::get_id() == _tid;
}

void EventLoop::runInLoop(Functor &&fc)
{
    if (isInLoopThread())
        fc(); // 归属线程内联执行（可能先于 _pendings 中已排队的任务）
    else
        queueInLoop(std::move(fc));
}

void EventLoop::queueInLoop(Functor &&fc) // 总是入队并唤醒，跨线程唯一入口
{
    {
        std::lock_guard<std::mutex> lg(_mutex);
        _pendings.push_back(std::move(fc));
    }
    writeEvfd();
}

void EventLoop::addReadHandler(int fd, Functor handler)
{
    _ioHandlers[fd] = std::move(handler);
    addEpReadfd(fd);
}

void EventLoop::removeReadHandler(int fd)
{
    delEpEventfd(fd);
    _ioHandlers.erase(fd);
}

int EventLoop::createEvfd()
{
    int fd = ::eventfd(0, 0);
    if (-1 == fd)
    {
        perror("eventfd failed");
    }

    return fd;
}

void EventLoop::readEvfd()
{
    uint64_t one = 1;
    ssize_t ret = ::read(_evfd, &one, sizeof(one));
    if (-1 == ret && errno != EAGAIN)
    {
        perror("read evfd failed");
    }
}

void EventLoop::writeEvfd()
{
    uint64_t one = 1;
    ssize_t ret = ::write(_evfd, &one, sizeof(one));
    if (-1 == ret)
    {
        perror("write evfd failed");
    }
}

void EventLoop::doPendings()
{
    logMessage(DEBUG, "in doPendings");
    std::vector<Functor> tmp;
    {
        std::lock_guard<std::mutex> lg(_mutex);
        std::swap(tmp, _pendings); // 先把_pendings里的任务拿出来，然后直接释放锁
    }

    logMessage(DEBUG, "executing %zu pending tasks", tmp.size());
    // 到这个地方再慢慢去执行任务，以免执行任务过程中还长期持有锁。使锁的粒度更低
    for (auto &e : tmp)
    {
        e(); // 执行任务
    }
}

void EventLoop::addConnection(const TcpConnectionPtr &tp)
{
    int fd = tp->fd();
    _connections.insert(std::make_pair(fd, tp)); // 加入Tcp连接池中
    logMessage(NORMAL, "add TcpConnection[fd = %d] in _connections, loop = %p", fd, static_cast<void *>(this));

    addEpReadfd(fd); // 加入epoll监听

    tp->handleOnConnecionCallback(); // 连接就可以执行连接建立事件
}

void EventLoop::removeConnection(int fd)
{
    closeConnection(fd);
}

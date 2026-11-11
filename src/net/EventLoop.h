#ifndef __EVENTLOOP_H__
#define __EVENTLOOP_H__

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class TcpConnection;

/**
 * @brief 封装使用ET模式的epoll系统调用的纯事件循环
 *
 * EventLoop 是纯事件循环（epoll + eventfd + pendings）：
 *  - 监听 fd 等非连接 fd 用 addReadHandler 注册通用读回调；
 *  - 连接经 TcpConnection::start() → addConnection 挂进 _connections，
 *    三个业务回调挂在 TcpConnection 上；
 *  - 跨线程投递唯一入口是 queueInLoop（入队 + 写 eventfd 唤醒）；
 *  - quit() 线程安全：置原子标志 + 写 eventfd，阻塞中的 epoll_wait 立即返回。
 */
class EventLoop
{

    static const int timeout = 3000;                                             // epoll超时时间参数
    static const int eventNum = 10;                                              // epoll监听事件数最大值
    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;                     // std::shared_ptr指针管理Tcp连接指针对象
    using Functor = std::function<void(void)>;                                   // 任务

public:
    EventLoop();
    ~EventLoop();

    // 开启事件监听循环（归属线程调用）
    void loop();
    // 结束循环（线程安全）：置标志后写 eventfd 唤醒，立即退出
    void quit();

    // 归属线程内联执行；其他线程转 queueInLoop
    void runInLoop(Functor &&fc);
    // 总是入队并写 eventfd 唤醒，跨线程唯一入口
    void queueInLoop(Functor &&fc);
    // 当前线程是否为本 loop 的归属线程
    bool isInLoopThread() const;

    // 非连接 fd（监听 fd、timerfd 等）的读事件回调注册/注销
    void addReadHandler(int fd, Functor handler);
    void removeReadHandler(int fd);

    // 对文件描述符追加写事件监听，此调用会添加(如果本身没有关心读事件)epoll对读事件的监听
    void pushWriteEvent(int fd);
    // 对文件描述符取消写事件关心
    void popWriteEvent(int fd);

    // 连接表管理：只允许归属 loop 线程调用
    // addConnection：挂进连接表、注册读事件、触发 onConnection（回调挂在连接上）
    void addConnection(const TcpConnectionPtr &tp);
    // removeConnection：摘 epoll → 出连接表 → 触发 onClose
    void removeConnection(int fd);

private:
    // 创建epoll文件描述符
    int createEpfd();
    // 把文件描述符fd读事件添加到epoll红黑树中，开始监听读事件
    void addEpReadfd(int fd);
    // 把文件描述符fd所有事件从epoll红黑树移除，取消监听
    void delEpEventfd(int fd);
    // 等待事件就绪并分发
    void waitEpfd();

    // 处理收到客户端发来的数据（EOF/出错走关闭路径）
    void handlerMessage(int fd);
    // 处理剩余未发送数据的发送
    void handlerRemainSend(int fd);
    // 关闭连接：摘 epoll → 出连接表 → onClose
    void closeConnection(int fd);

    // 创建eventfd
    int createEvfd();

    // EventLoop的主线程执行_pendings集合里的所有任务时
    void readEvfd();
    // 线程添加任务到_pendings集合时
    void writeEvfd();
    // 执行_pendings的任务
    void doPendings();

private:
    int _epfd; // epoll fd 用于监听事件，实现IO多路复用
    std::atomic<bool> _isLooping;
    std::thread::id _tid;                          // loop() 的归属线程，用于 isInLoopThread
    std::vector<struct epoll_event> _events;       // 就绪事件数组
    std::map<int, TcpConnectionPtr> _connections;  // Tcp连接池
    std::map<int, Functor> _ioHandlers;            // 监听 fd 等非连接 fd 的读回调

    int _evfd;                      // 特殊的文件描述符，用于线程间通信
    std::vector<Functor> _pendings; // 存放EventLoop需要执行的任务
    std::mutex _mutex;              // 互斥锁，用于_pendings的互斥访问
};

#endif

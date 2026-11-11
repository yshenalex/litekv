#ifndef __SERVER_H__
#define __SERVER_H__

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Config.hpp"
#include "Acceptor.h"
#include "EventLoop.h"
#include "SubReactorGroup.h"
#include "TcpConnection.h"

/**
 * @brief 组装层：MainReactor 跑 Acceptor，SubReactor 跑连接（连接亲和）。
 *
 * - `Config.subReactors == 0` 是正式的单 Reactor 退化模式：MainReactor 同时
 *   accept 和处理连接，SubReactorGroup 为空，不得调用 getNextLoop()；
 * - `subReactors > 0` 时 accept 后 round-robin 选一个 SubReactor，
 *   用 queueInLoop 把"创建并注册连接"投递过去（fd 注册必须发生在归属 loop 内）。
 * 只做组装与生命周期管理：new 出对象、用 lambda 接线。
 */
class Server
{
    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
    using TcpConnectionCallback = std::function<void(const TcpConnectionPtr &)>;

public:
    explicit Server(const Config &cfg);
    ~Server();

    // 启动准备：启动 SubReactor 池（等全部就绪）→ bind/listen → 注册监听 fd。
    // 失败（配置非法/端口占用等）返回 false，不进入事件循环
    bool start();
    // MainReactor 事件循环（阻塞，直到 stop()）
    void loop();
    // 线程安全退出：停止 accept → 各 SubReactor quit+join → MainReactor quit
    void stop();

    // 业务回调（start() 之前设置），随每个连接创建挂载
    void setOnConnection(TcpConnectionCallback cb);
    void setOnMessage(TcpConnectionCallback cb);
    void setOnClose(TcpConnectionCallback cb);

private:
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    // MainReactor 监听 fd 的读回调：ET 循环 accept 到 EAGAIN，逐个分发
    void acceptLoop();
    // 在目标 loop 线程内创建 TcpConnection 并挂进事件循环
    void createConnection(int connfd, EventLoop *loop);

private:
    Config _cfg;
    Acceptor _acceptor;
    EventLoop _mainLoop;          // MainReactor
    SubReactorGroup _subReactors; // SubReactor 组（0 个 = 单 Reactor）

    TcpConnectionCallback _onConnection;
    TcpConnectionCallback _onMessage;
    TcpConnectionCallback _onClose;
};

#endif

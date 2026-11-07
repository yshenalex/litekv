#ifndef __TCPSERVER_H__
#define __TCPSERVER_H__
#include <cstdint>
#include <functional>
#include <string>
#include "Acceptor.h"
#include "EventLoop.h"
#include "TcpConnection.h"

/**
 * @brief P0/P1 过渡期组装点：MainReactor 单线程完成 accept 与连接读写。
 *
 * 命令在连接所属线程内串行执行，不使用业务线程池。
 * P1 由 server/Server 取代（MainReactor + N SubReactor，连接亲和）。
 */
class TcpServer
{

    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;                     // std::shared_ptr指针管理Tcp连接指针对象
    using TcpConnectionCallback = std::function<void(const TcpConnectionPtr &)>; // 三个事件回调函数
    static const std::string defaultIp;                                          // Tcp服务器默认Ip地址，类内声明类外定义

public:
    explicit TcpServer(uint16_t port, const std::string &ip = defaultIp);
    ~TcpServer();

    // 启动准备：bind/listen 成功后把监听 fd 注册进事件循环。
    // 失败（端口占用等）返回 false，调用方必须报错退出，不得带病进入事件循环
    bool start();
    // 进入事件循环（阻塞，直到 stop()）
    void loop();
    // 线程安全退出事件循环
    void stop();

    void setAllCallBacks(TcpConnectionCallback &&onConnectionCb, TcpConnectionCallback &&onMessageCb, TcpConnectionCallback &&onCloseCb);

private:
    TcpServer &operator=(const TcpServer &) = delete;
    TcpServer(const TcpServer &) = delete;

    // ET：循环 accept 直到 EAGAIN，收干净全部待接受连接
    void acceptLoop();
    // 在归属 loop 内创建 TcpConnection 并挂进事件循环
    void createConnection(int connfd);

private:
    Acceptor _acceptor;   // 服务器接收连接接收器
    EventLoop _eventLoop; // 服务器主循环逻辑
};

#endif

#include "TcpServer.h"
#include <cerrno>
#include "Log.hpp"

// [1]defaultIp是静态成员变量，在编译时就要确定，但是编译阶段并没有给defaultIp分配内存空间，因此不能赋值
// 到链接阶段时，由链接器分配常量区内存(静态变量、全局变量)给defaultIp，此时才可以定义数值
const std::string TcpServer::defaultIp = "0.0.0.0"; // 类内声明，类外定义。【详见TcpServer.h】

TcpServer::TcpServer(uint16_t port, const std::string &ip)
    : _acceptor(port, ip)
{
}

TcpServer::~TcpServer()
{
}

bool TcpServer::start()
{
    // 启动顺序修正：先 bind/listen 成功，再把监听 fd 注册进 epoll（DESIGN §5.1.3）
    if (!_acceptor.ready())
    {
        logMessage(FATAL, "TcpServer: bind/listen failed (port in use or permission denied)");
        return false;
    }

    _eventLoop.addReadHandler(_acceptor.fd(), [this] { acceptLoop(); });
    return true;
}

void TcpServer::loop()
{
    _eventLoop.loop();
}

void TcpServer::stop()
{
    _eventLoop.quit();
}

void TcpServer::setAllCallBacks(TcpConnectionCallback &&onConnectionCb,
                                TcpConnectionCallback &&onMessageCb,
                                TcpConnectionCallback &&onCloseCb)
{
    _eventLoop.setOnConnection(std::move(onConnectionCb));
    _eventLoop.setOnMessage(std::move(onMessageCb));
    _eventLoop.setOnClose(std::move(onCloseCb));
}

void TcpServer::acceptLoop()
{
    // 对于连接任务，ET 模式必须循环获取全部新连接，直到 EAGAIN
    while (true)
    {
        int connfd = _acceptor.accept();
        if (-1 == connfd)
        {
            if (errno == EINTR)
            {
                continue;
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                logMessage(DEBUG, "TcpServer::acceptLoop / errno == EAGAIN, all accepted");
                break;
            }
            else
            {
                logMessage(WARNING, "TcpServer::acceptLoop / accept a new link failed");
                break;
            }
        }

        createConnection(connfd);
    }
}

void TcpServer::createConnection(int connfd)
{
    // 建立连接后，创建TcpConnecion对象管理并挂进事件循环
    TcpConnectionPtr tp(new TcpConnection(connfd, &_eventLoop));
    tp->start(); // 挂进连接表、注册读事件、触发 onConnection
}

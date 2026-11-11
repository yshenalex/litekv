#include "Server.h"
#include <cerrno>
#include <functional>
#include <thread>
#include "Log.hpp"
#include "Socket.h"

Server::Server(const Config &cfg)
    : _cfg(cfg),
      _acceptor(cfg.port, cfg.ip),
      _subReactors(cfg.subReactors)
{
}

Server::~Server()
{
    stop();
}

bool Server::start()
{
    if (!validateConfig(_cfg))
    {
        logMessage(FATAL, "Server: invalid config");
        return false;
    }

    // 启动 N 个 SubReactor，阻塞到全部 loop 就绪（N=0 时空操作）
    _subReactors.start();

    // 启动顺序：先 bind/listen 成功，再把监听 fd 注册进 epoll
    if (!_acceptor.ready())
    {
        logMessage(FATAL, "Server: bind/listen failed (port in use or permission denied)");
        _subReactors.quit();
        return false;
    }

    _mainLoop.addReadHandler(_acceptor.fd(), [this] { acceptLoop(); });
    logMessage(NORMAL, "Server started, port = %u, subReactors = %zu",
               static_cast<unsigned>(_cfg.port), _cfg.subReactors);
    return true;
}

void Server::loop()
{
    _mainLoop.loop();
}

void Server::stop()
{
    // 关闭顺序：停 accept（MainReactor 退出即停止）→ 各 SubReactor quit+join → MainReactor quit
    _subReactors.quit();
    _mainLoop.quit();
}

void Server::setOnConnection(TcpConnectionCallback cb)
{
    _onConnection = std::move(cb);
}
void Server::setOnMessage(TcpConnectionCallback cb)
{
    _onMessage = std::move(cb);
}
void Server::setOnClose(TcpConnectionCallback cb)
{
    _onClose = std::move(cb);
}

void Server::acceptLoop()
{
    // ET 模式必须一次收干净全部待接受连接
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
                break;
            }
            else
            {
                logMessage(WARNING, "Server::acceptLoop / accept a new link failed");
                break;
            }
        }

        // accept 后 fd 先设为非阻塞，再投递到目标 loop
        Socket::setNonBlock(connfd);

        if (_cfg.subReactors == 0)
        {
            // 单 Reactor 退化模式：连接直接建在 MainReactor
            createConnection(connfd, &_mainLoop);
        }
        else
        {
            // round-robin 选一个 SubReactor；连接注册必须发生在归属 loop 线程内
            EventLoop *io = _subReactors.getNextLoop();
            io->queueInLoop([this, io, connfd] { createConnection(connfd, io); });
        }
    }
}

void Server::createConnection(int connfd, EventLoop *loop)
{
    logMessage(NORMAL, "createConnection fd = %d on loop = %p, tid = %zu", connfd,
               static_cast<void *>(loop),
               static_cast<size_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())));

    TcpConnectionPtr tp(new TcpConnection(connfd, loop, _cfg.inputHighWater, _cfg.outputHighWater));
    tp->setOnConnection(_onConnection);
    tp->setOnMessage(_onMessage);
    tp->setOnClose(_onClose);
    tp->start(); // 挂进目标 loop、注册读事件、触发 onConnection
}

#include "TcpConnection.h"
#include <sstream>
#include <cstring>
#include "EventLoop.h"
#include "Log.hpp"

TcpConnection::TcpConnection(int fd, EventLoop *loop)
    : _sock(fd),
      _socketIO(fd),
      _localaddr(getLocalAddr()),
      _peeraddr(getPeerAddr()),
      _loop(loop)
{
    // ET 模式要求连接 fd 非阻塞
    Socket::setNonBlock(fd);
}

TcpConnection::~TcpConnection()
{
}

void TcpConnection::start()
{
    // 必须在归属 loop 线程执行：挂进连接表、注册读事件、触发 onConnection
    _loop->addConnection(shared_from_this());
}

void TcpConnection::send()
{
    logMessage(DEBUG, "in TcpConnection::send, connfd = %d", _sock.fd());
    if (_outBuffer.length() > 0 && -1 == _socketIO.writeToKernel(_outBuffer))
    {
        // 写出错或对端断开：关闭连接，未发送数据丢弃
        logMessage(ERROR, "TcpConnection::send failed, connfd = %d", _sock.fd());
        forceClose();
        return;
    }

    if (_outBuffer.length() > 0) // 前面循环拷贝数据到内核后发现缓冲区还有数据没发送完
    {
        // 将其添加到epoll监听写事件，等 EPOLLOUT 继续冲刷
        _loop->pushWriteEvent(_sock.fd());
    }
    else // 发送完毕：取消写事件；若处于"发完即关"状态，走关闭路径
    {
        _loop->popWriteEvent(_sock.fd());
        if (_closeAfterFlush)
            forceClose();
    }

    logMessage(DEBUG, "out TcpConnection::send");
}

bool TcpConnection::receive()
{
    logMessage(DEBUG, "in TcpConnection::receive, connfd = %d", _sock.fd());
    bool eof = false;
    if (-1 == _socketIO.readFromKernel(_inBuffer, eof))
    {
        logMessage(ERROR, "TcpConnection::receive failed, connfd = %d", _sock.fd());
        return false;
    }
    logMessage(DEBUG, "out TcpConnection::receive, eof = %d", eof);
    return !eof;
}

void TcpConnection::sendOutput(const std::string &msg)
{
    logMessage(DEBUG, "in sendOutput, msg length = %zu", msg.size());
    if (!_loop)
        return;

    if (_outBuffer.remain() < msg.size()) // 输出缓冲区放不下时才扩容
        _outBuffer.enlarge(msg.size());   // 一次扩到至少能放下 msg

    // 把string类型的msg拷贝到输出缓冲区中
    msg.copy(_outBuffer.space(), msg.size());
    if (!_outBuffer.next(msg.size())) // 游标推进（返回 false 属内部状态不一致，防御性丢弃）
    {
        logMessage(ERROR, "TcpConnection::sendOutput: buffer next failed, drop %zu bytes", msg.size());
        return;
    }

    _loop->runInLoop(std::bind(&TcpConnection::send, shared_from_this())); // 安排冲刷
    logMessage(DEBUG, "out sendOutput");
}

void TcpConnection::forceClose()
{
    if (_loop)
        _loop->removeConnection(_sock.fd());
}

void TcpConnection::closeAfterFlush()
{
    if (_outBuffer.length() == 0)
    {
        forceClose();
        return;
    }
    // 还有输出没发完：置标志，等 send() 冲刷完再关；补一次写事件确保有冲刷机会
    _closeAfterFlush = true;
    _loop->pushWriteEvent(_sock.fd());
}

int TcpConnection::fd() const
{
    return _sock.fd();
}

InetAddress TcpConnection::getLocalAddr()
{
    struct sockaddr_in local;
    socklen_t len = sizeof(struct sockaddr_in);
    memset(&local, 0, len);

    int ret = getsockname(_sock.fd(), reinterpret_cast<sockaddr *>(&local), &len); // 获取本端地址端口号等信息
    if (ret == -1)
    {
        perror("getsockname failed");
    }

    return InetAddress(local);
}
InetAddress TcpConnection::getPeerAddr()
{
    struct sockaddr_in peer;
    socklen_t len = sizeof(struct sockaddr_in);
    memset(&peer, 0, len);

    int ret = getpeername(_sock.fd(), reinterpret_cast<sockaddr *>(&peer), &len); // 获取对端地址端口号等信息
    if (ret == -1)
    {
        perror("getpeername failed");
    }

    return InetAddress(peer);
}

std::string TcpConnection::toString()
{
    std::ostringstream oss;
    oss << "[TcpConnection]"
        << _localaddr.ip() << ":" << _localaddr.port()
        << "--->" << _peeraddr.ip() << ":" << _peeraddr.port();
    return oss.str();
}

void TcpConnection::setOnConnection(const TcpConnectionCallback &cb)
{
    _onConnection = cb;
}
void TcpConnection::setOnMessage(const TcpConnectionCallback &cb)
{
    _onMessage = cb;
}
void TcpConnection::setOnClose(const TcpConnectionCallback &cb)
{
    _onClose = cb;
}

void TcpConnection::handleOnConnecionCallback()
{
    if (_onConnection)
        _onConnection(shared_from_this());
}
void TcpConnection::handleOnMessageCallback()
{
    if (_onMessage)
        _onMessage(shared_from_this());
}
void TcpConnection::handleOnCloseCallback()
{
    if (_onClose)
        _onClose(shared_from_this());
}

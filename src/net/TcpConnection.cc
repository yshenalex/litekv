#include "TcpConnection.h"
#include <sstream>
#include <cstring>
#include "EventLoop.h"
#include "Log.hpp"

TcpConnection::TcpConnection(int fd, EventLoop *loop,
                             size_t inputHighWater, size_t outputHighWater)
    : _sock(fd),
      _socketIO(fd),
      _localaddr(getLocalAddr()),
      _peeraddr(getPeerAddr()),
      _inputHighWater(inputHighWater),
      _outputHighWater(outputHighWater),
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

TcpConnection::SendResult TcpConnection::send(const char *data, size_t len)
{
    if (!_connected)
        return SendResult::Closed;

    // 输出缓冲超水位：不入缓冲（防无界增长），交给调用方决定停读或断连
    if (_outBuffer.length() + len > _outputHighWater)
    {
        logMessage(WARNING, "TcpConnection::send backpressure, fd = %d, pending = %zu", _sock.fd(), _outBuffer.length());
        return SendResult::Backpressure;
    }

    if (_outBuffer.remain() < len) // 放不下先扩容（一次扩到至少能放下）
        _outBuffer.enlarge(len);

    std::memcpy(_outBuffer.space(), data, len); // 二进制安全
    if (!_outBuffer.next(len))
    {
        // enlarge 后仍放不下属内部状态不一致，防御性关闭
        logMessage(ERROR, "TcpConnection::send buffer next failed, fd = %d", _sock.fd());
        forceClose();
        return SendResult::Closed;
    }

    _loop->runInLoop(std::bind(&TcpConnection::handleWrite, shared_from_this()));
    return SendResult::Ok;
}

TcpConnection::SendResult TcpConnection::send(const std::string &data)
{
    return send(data.data(), data.size());
}

void TcpConnection::handleWrite()
{
    logMessage(DEBUG, "in TcpConnection::handleWrite, connfd = %d", _sock.fd());
    if (_outBuffer.length() > 0 && -1 == _socketIO.writeToKernel(_outBuffer))
    {
        // 写出错或对端断开：关闭连接，未发送数据丢弃
        logMessage(ERROR, "TcpConnection::handleWrite failed, connfd = %d", _sock.fd());
        forceClose();
        return;
    }

    if (_outBuffer.length() > 0) // 缓冲区还有数据没发送完
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

    logMessage(DEBUG, "out TcpConnection::handleWrite");
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

    if (_inBuffer.length() > _inputHighWater)
    {
        // 输入超限（防恶意/失控客户端吃内存）：断连
        logMessage(WARNING, "TcpConnection::receive input overflow (%zu bytes), fd = %d, closing",
                   _inBuffer.length(), _sock.fd());
        forceClose();
        return false;
    }

    logMessage(DEBUG, "out TcpConnection::receive, eof = %d", eof);
    return !eof;
}

Buffer &TcpConnection::inputBuffer()
{
    return _inBuffer;
}

void TcpConnection::forceClose()
{
    markClosed();
    if (_loop)
        _loop->removeConnection(_sock.fd());
}

void TcpConnection::closeAfterFlush()
{
    if (!_connected)
        return;

    if (_outBuffer.length() == 0)
    {
        forceClose();
        return;
    }
    // 还有输出没发完：置标志，等 handleWrite() 冲刷完再关；补一次写事件确保有冲刷机会
    _closeAfterFlush = true;
    _loop->pushWriteEvent(_sock.fd());
}

bool TcpConnection::connected() const
{
    return _connected;
}

void TcpConnection::setContext(std::shared_ptr<void> ctx)
{
    _context = std::move(ctx);
}

std::shared_ptr<void> TcpConnection::context() const
{
    return _context;
}

void TcpConnection::markClosed()
{
    _connected = false;
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
    _context.reset(); // onClose 之后释放上下文槽
}

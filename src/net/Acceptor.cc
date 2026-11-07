#include "Acceptor.h"
#include "Log.hpp"

Acceptor::Acceptor(uint16_t port, const std::string &ip) : _sock(), _inetaddr(ip, port)
{
}

Acceptor::~Acceptor()
{
}

bool Acceptor::ready()
{
    // ET 模式要求监听 fd 非阻塞：accept 循环依赖 EAGAIN 判断收干净
    if (!Socket::setNonBlock(_sock.fd()))
        return false;

    setIpReuse();
    setPortReuse();
    if (!bind())
        return false;
    if (!listen())
        return false;
    return true;
}

int Acceptor::accept()
{
    logMessage(DEBUG, "in accept");
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer); // 必须初始化：未初始化就传给 accept 是未定义行为
    int connfd = ::accept(_sock.fd(), reinterpret_cast<sockaddr *>(&peer), &len);
    logMessage(DEBUG, "ing accept, connfd = %d", connfd);
    if (-1 == connfd)
    {
        // ET模式下返回-1可能是正常情况，具体要看errno的结果，交给调用accept的TcpServer::acceptLoop判断
        return -1;
    }

    return connfd;
}

int Acceptor::fd() const
{
    return _sock.fd();
}

bool Acceptor::bind()
{
    int ret = ::bind(_sock.fd(), reinterpret_cast<const struct sockaddr *>(_inetaddr.getInetAddrPtr()), sizeof(struct sockaddr_in));
    if (-1 == ret)
    {
        perror("bind failed");
        return false;
    }
    return true;
}

bool Acceptor::listen()
{
    int ret = ::listen(_sock.fd(), gbacklog);
    if (-1 == ret)
    {
        perror("listen failed");
        return false;
    }
    return true;
}

void Acceptor::setIpReuse()
{
    int opt = 1; // 1表示希望下面setsockopt有效，如果是0则是无效
    int ret = setsockopt(_sock.fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (-1 == ret)
    {
        perror("setsockopt failed");
        return;
    }
}

void Acceptor::setPortReuse()
{
    int opt = 1;
    int ret = setsockopt(_sock.fd(), SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    if (-1 == ret)
    {
        perror("setsockopt failed");
        return;
    }
}

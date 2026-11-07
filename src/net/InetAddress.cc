#include "InetAddress.h"
#include <cstring>

InetAddress::InetAddress(const std::string &ip, uint16_t port)
{
    memset(&_addr, 0, sizeof(_addr));
    _addr.sin_family = AF_INET;
    _addr.sin_addr.s_addr = inet_addr(ip.c_str());
    _addr.sin_port = htons(port);
}

InetAddress::InetAddress(const struct sockaddr_in &addr) : _addr(addr)
{
}

InetAddress::~InetAddress()
{
}

std::string InetAddress::ip() const
{
    // inet_ntoa 返回静态缓冲区，多线程并发调用会串地址；改用 inet_ntop
    char buf[INET_ADDRSTRLEN] = {0};
    if (nullptr == inet_ntop(AF_INET, &_addr.sin_addr, buf, sizeof(buf)))
    {
        return std::string();
    }
    return std::string(buf);
}
uint16_t InetAddress::port() const
{
    return ntohs(_addr.sin_port);
}
const struct sockaddr_in *InetAddress::getInetAddrPtr() const
{
    return &_addr;
}

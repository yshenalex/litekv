#include "Socket.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

Socket::Socket()
{
    _fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == _fd)
    {
        perror("socket faild");
        return;
    }
}

Socket::Socket(int fd) : _fd(fd)
{
}

Socket::~Socket()
{
    close(_fd);
}

int Socket::fd() const
{
    return _fd;
}

bool Socket::setNonBlock(int fd)
{
    int fl = ::fcntl(fd, F_GETFL);
    if (fl < 0)
    {
        perror("fcntl(F_GETFL) failed");
        return false;
    }
    if (-1 == ::fcntl(fd, F_SETFL, fl | O_NONBLOCK))
    {
        perror("fcntl(F_SETFL:O_NONBLOCK) failed");
        return false;
    }
    return true;
}
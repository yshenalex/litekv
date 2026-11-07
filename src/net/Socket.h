#ifndef __SOCKET_H__
#define __SOCKET_H__

class Socket
{
public:
    Socket();
    explicit Socket(int fd);
    ~Socket();
    int fd() const;

    // 把 fd 设为非阻塞（ET 模式前置要求）；成功返回 true
    static bool setNonBlock(int fd);

private:
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

private:
    int _fd;
};

#endif

#include "SocketIO.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include "Buffer.hpp"
#include "Log.hpp"

SocketIO::SocketIO(int fd) : _fd(fd)
{
}

SocketIO::~SocketIO()
{
    // SocketIO只负责对某个文件描述符执行读写操作，但并不关心文件描述符关闭
    // 文件描述符的关闭，交给Socket类的析构函数，以免对同个文件描述符重复关闭
    // close(_fd);
}

int SocketIO::readn(char *buffer, int n)
{
    if (!buffer || n <= 0)
    {
        errno = EINVAL; // 无效参数
        perror("read failed(invalid input)");
        return -1;
    }

    char *pstr = buffer;
    int rest = n;
    while (rest > 0)
    {
        ssize_t s = read(_fd, pstr, rest);
        if (-1 == s)
        {
            if (errno == EINTR)
                continue; // 被信号意外中断，下一轮续读

            perror("read failed(read error)"); // 其他读取失败情况，直接返回-1
            return -1;
        }
        else if (0 == s) // 对端关闭，退出循环
        {
            break;
        }
        else if (s > 0)
        {
            pstr += s;
            rest -= s;
        }
    }

    return n - rest; // 返回实际读取数，如果正常读完返回就是n
}

int SocketIO::readLine(char *buffer, int n)
{
    if (!buffer || n <= 0)
    {
        errno = EINVAL; // 无效参数
        perror("readLine failed(invalid input)");
        return -1;
    }

    char *pstr = buffer;
    int rest = n - 1; // 预留一个位置最后填充'\0'
    while (rest > 0)
    {
        ssize_t s = recv(_fd, pstr, rest, MSG_PEEK); // 设置为MSG_PEEK后，意味着内核缓冲区的数据不会被清除，而只是被拷贝到用户区
        // 相当于recv我们并不跟之前一样拷贝到用户缓冲区后直接清除内核缓冲区的数据，而是先"拿出来"看一下，在后面再调用readn()拷贝与清除
        if (-1 == s)
        {
            if (errno == EINTR)
                continue;

            perror("readLine failed(read error)");
            return -1;
        }
        else if (0 == s)
        {
            break; // 对端关闭，直接退出循环
        }
        else if (s > 0)
        {
            int i;
            for (i = 0; i < s; i++)
            {
                if (pstr[i] == '\n')
                {
                    int size = i + 1;
                    readn(pstr, size); // 再调用一次readn(), 拷贝并清除内核缓冲区数据
                    pstr += size;
                    *pstr = '\0'; // c语言风格字符串结尾
                    rest -= size;
                    return n - rest; // 在这次读取中找到了换行符，也就是读到了一行，在这这里终结即可
                }
            }

            readn(pstr, s);
            pstr += s;
            rest -= s;
        }
    }

    // 此处应该是缓冲区不足或者s==0的情况，此处在末尾处添加\0即可
    *pstr = '\0';
    return n - rest;
}

int SocketIO::writen(const char *buffer, int n)
{
    if (buffer == nullptr || n <= 0)
    {
        errno = EINVAL;
        perror("write failed(invalid input)");
        return -1;
    }

    int rest = n;
    int pos = 0;

    while (rest > 0)
    {
        ssize_t s = write(_fd, buffer + pos, rest);
        if (-1 == s)
        {
            if (errno == EINTR)
                continue;

            perror("write failded(write error)");
            return -1;
        }
        else if (0 == s)
        {
            break;
        }
        else if (s > 0)
        {
            pos += s;
            rest -= s;
        }
    }

    return n - rest;
}

int SocketIO::readFromKernel(Buffer &buffer, bool &eof)
{
    logMessage(DEBUG, "SocketIO::readFromKernel, oldLength = %zu", buffer.length());
    size_t oldLength = buffer.length();
    eof = false;
    while (true)
    {
        // 可用区域用尽才扩容；绝不调用 read(fd, p, 0)（会立即返回 0，与 EOF 无法区分）
        if (buffer.remain() == 0)
        {
            buffer.enlarge();
            if (buffer.remain() == 0)
            {
                // resize 后仍无可用空间，属于致命错误（OOM 边缘），保守返回
                return -1;
            }
        }

        ssize_t s = ::read(_fd, buffer.space(), buffer.remain());
        if (-1 == s)
        {
            if (errno == EINTR)
            {
                continue;
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK) // 非阻塞 fd 读干净了
            {
                logMessage(DEBUG, "SocketIO::readFromKernel/ -1 == s/ errno == EAGAIN");
                break;
            }
            else // 其它情况说明出错
            {
                perror("SocketIO::readFromKernel failed");
                return -1;
            }
        }
        else if (0 == s)
        {
            // 对端半关闭（FIN）：EOF。必须显式上报，否则 ET 下没有下一次事件，
            // 连接会永远挂在 _connections 里，或在读循环里空转打满 CPU。
            logMessage(DEBUG, "SocketIO::readFromKernel/ s == 0, EOF");
            eof = true;
            break;
        }
        else
        {
            // 读到数据立即推进游标；返回 false 说明内部状态不一致（放得下却被拒），防御性报错
            if (!buffer.next(static_cast<size_t>(s)))
            {
                return -1;
            }
        }
    }
    logMessage(DEBUG, "out SocketIO::readFromKernel, return %zu", buffer.length() - oldLength);
    return static_cast<int>(buffer.length() - oldLength); // 返回读取字节数
}

int SocketIO::writeToKernel(Buffer &buffer)
{
    int total = 0;
    while (true)
    {
        // MSG_NOSIGNAL：对端已关闭时返回 EPIPE 而不是用 SIGPIPE 杀死整个进程
        ssize_t s = ::send(_fd, buffer.start(), buffer.length(), MSG_NOSIGNAL);
        if (-1 == s)
        {
            if (errno == EINTR)
            {
                continue;
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                logMessage(DEBUG, "SocketIO::writeToKernel/ -1 == s/ errno == EAGAIN");
                break;
            }
            else
            {
                perror("SocketIO::writeToKernel failed");
                return -1;
            }
        }
        else if (s > 0)
        {
            buffer.erase(s);
            total += s;
            // 缓冲区数据发送完毕，直接退出循环
            if (buffer.length() == 0)
                break;
        }
        else // 0 == s：写出 0 字节视为异常，保守返回错误
        {
            return -1;
        }
    }

    return total;
}
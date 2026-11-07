#ifndef __ACCEPTOR_H__
#define __ACCEPTOR_H__

#include <string>
#include "Socket.h"
#include "InetAddress.h"

/**
 * @brief 接收器，创建监听套接字，监听新的Tcp连接
 *
 *
 */
class Acceptor
{
    static const int gbacklog = 128; // 半连接队列和全连接队列的长度之和-1；太小会在压测/突发连接时丢 SYN

public:
    Acceptor(uint16_t port, const std::string &ip);
    ~Acceptor();

    // 初始化监听套接字, 包括sock, bind, listen系统调用
    // 返回 false 表示 bind/listen 失败（端口占用、权限不足等），调用方必须报错退出，
    // 不能带病进入事件循环
    bool ready();

    // 接收新连接
    int accept();

    // 获取监听文件描述符
    int fd() const;

private:
    // 绑定端口和ip
    bool bind();

    // 设置为监听状态
    bool listen();

    // ip地址复用
    void setIpReuse();

    // 端口复用
    void setPortReuse();

private:
    Socket _sock;
    InetAddress _inetaddr;
};

#endif
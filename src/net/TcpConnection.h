#ifndef __TCPCONNECTION_H__
#define __TCPCONNECTION_H__
#include <functional>
#include <memory>
#include "InetAddress.h"
#include "Socket.h"
#include "SocketIO.h"
#include "Buffer.hpp"

class EventLoop; // 前向声明

// TcpConnection类继承自std::enable_shared_from_this<TcpConnection>模板类
// 【作用】让TcpConnection类的对象能够在自身成员函数中，安全地获取指向自己的std::shared_ptr智能指针
class TcpConnection : public std::enable_shared_from_this<TcpConnection>
{
    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;                     // std::shared_ptr指针管理Tcp连接指针对象
    using TcpConnectionCallback = std::function<void(const TcpConnectionPtr &)>; // 三个事件回调函数

public:
    /**
     * @brief Construct a new Tcp Connection object
     *
     * @param fd 对应Tcp连接的文件描述符
     * @param loop Tcp连接所属EventLoop对象指针
     */
    explicit TcpConnection(int fd, EventLoop *loop);

    ~TcpConnection();

    // 把连接挂进归属 EventLoop 的连接表并注册读事件，触发 onConnection。
    // 必须在归属 loop 线程内调用
    void start();

    // 将Tcp连接对象TcpConnection的发送缓冲区_outBuffer数据发送给客户端（冲刷）
    void send();
    // 接收客户端发送的数据到_inBuffer。返回 false 表示 EOF/出错，调用方应关闭连接
    bool receive();

    // 将需要发送给客户端的数据拷贝进输出缓冲区，并安排冲刷
    void sendOutput(const std::string &msg);

    // 立即关闭连接，丢弃输出（归属线程调用）
    void forceClose();
    // 输出缓冲区冲刷完后关闭；缓冲区为空则立即关闭（归属线程调用）
    void closeAfterFlush();

    // 连接对应的文件描述符
    int fd() const;

    // 获取本Tcp连接的服务器ip地址和端口号
    InetAddress getLocalAddr();
    // 获取本Tcp连接的客户端ip地址和端口号
    InetAddress getPeerAddr();
    // 自己实现的Tcp连接转为可视化字符串的方法
    std::string toString();

    // 由EventLoop给每个Tcp连接注册三个回调函数（const 引用拷贝）
    void setOnConnection(const TcpConnectionCallback &cb);
    void setOnMessage(const TcpConnectionCallback &cb);
    void setOnClose(const TcpConnectionCallback &cb);

    // 三个回调函数执行
    void handleOnConnecionCallback();
    void handleOnMessageCallback();
    void handleOnCloseCallback();

public:
    // Tcp连接的用户一级缓冲区，存放直接从内核缓冲区拷贝到用户缓冲区的数据
    Buffer _inBuffer;  // 读取缓冲区(接收客户端发送的数据)
    Buffer _outBuffer; // 写出缓冲区(发送数据给客户端)

private:
    Socket _sock; // 该Tcp连接对应的文件描述符(服务器端)，析构时关闭
    SocketIO _socketIO;

    // 类初始化顺序与初始化列表顺序无关，而是与类中声明顺序有关，
    // 由于_localaddr与_peeraddr初始化需要_sock.fd()，因而必须在其后声明

    InetAddress _localaddr;
    InetAddress _peeraddr;

    TcpConnectionCallback _onConnection; // 连接建立
    TcpConnectionCallback _onMessage;    // 消息到来
    TcpConnectionCallback _onClose;      // 连接断开

    bool _closeAfterFlush = false; // true = 输出冲刷完后关闭连接

    EventLoop *_loop; // 每个Tcp连接归属的EventLoop
};

#endif

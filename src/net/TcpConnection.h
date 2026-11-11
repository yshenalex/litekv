#ifndef __TCPCONNECTION_H__
#define __TCPCONNECTION_H__
#include <cstddef>
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
    // send() 的结果：Ok 正常入缓冲并安排冲刷；Backpressure 输出超水位（调用方应关闭连接）；
    // Closed 连接已关闭
    enum class SendResult { Ok, Backpressure, Closed };

    /**
     * @brief Construct a new Tcp Connection object
     *
     * @param fd 对应Tcp连接的文件描述符
     * @param loop Tcp连接所属EventLoop对象指针
     * @param inputHighWater 输入缓冲上限（字节），超限断连
     * @param outputHighWater 输出缓冲高水位（字节），超水位 send 返回 Backpressure
     */
    explicit TcpConnection(int fd, EventLoop *loop,
                           size_t inputHighWater = 64 << 20,
                           size_t outputHighWater = 16 << 20);

    ~TcpConnection();

    // 把连接挂进归属 EventLoop 的连接表并注册读事件，触发 onConnection。
    // 必须在归属 loop 线程内调用
    void start();

    // 把数据拷贝进输出缓冲区并安排冲刷（只在归属 loop 线程调用）
    SendResult send(const char *data, size_t len);
    SendResult send(const std::string &data);

    // 冲刷输出缓冲区剩余数据（EPOLLOUT / 内部安排时由归属线程调用）
    void handleWrite();

    // 接收客户端发送的数据到输入缓冲区。
    // 返回 false 表示 EOF/出错/超输入水位，调用方应关闭连接
    bool receive();

    // 输入缓冲区（归属线程调用；server 读取/erase）
    Buffer &inputBuffer();

    // 立即关闭连接，丢弃输出（归属线程调用）
    void forceClose();
    // 输出缓冲区冲刷完后关闭；缓冲区为空则立即关闭（归属线程调用）
    void closeAfterFlush();
    // 连接是否仍然有效
    bool connected() const;

    // 通用上下文槽，net 不解释其类型（Session 等由 server 挂载）
    void setContext(std::shared_ptr<void> ctx);
    std::shared_ptr<void> context() const;

    // 连接对应的文件描述符
    int fd() const;

    // 获取本Tcp连接的服务器ip地址和端口号
    InetAddress getLocalAddr();
    // 获取本Tcp连接的客户端ip地址和端口号
    InetAddress getPeerAddr();
    // 自己实现的Tcp连接转为可视化字符串的方法
    std::string toString();

    // 三个回调函数注册（const 引用拷贝）
    void setOnConnection(const TcpConnectionCallback &cb);
    void setOnMessage(const TcpConnectionCallback &cb);
    void setOnClose(const TcpConnectionCallback &cb);

    // 三个回调函数执行
    void handleOnConnecionCallback();
    void handleOnMessageCallback();
    void handleOnCloseCallback();

private:
    // 由 EventLoop 关闭路径调用：标记连接已关闭
    void markClosed();

private:
    Buffer _inBuffer;  // 读取缓冲区(接收客户端发送的数据)
    Buffer _outBuffer; // 写出缓冲区(发送数据给客户端)

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
    bool _connected = true;        // 连接是否仍然有效

    size_t _inputHighWater;  // 输入缓冲上限（字节）
    size_t _outputHighWater; // 输出缓冲高水位（字节）

    std::shared_ptr<void> _context; // 通用上下文槽（Session 等）

    EventLoop *_loop; // 每个Tcp连接归属的EventLoop

    friend class EventLoop; // closeConnection 路径需要 markClosed
};

#endif

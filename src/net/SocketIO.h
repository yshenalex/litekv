#ifndef __SOCKETIO_H__
#define __SOCKETIO_H__
#include <vector>

class Buffer;

class SocketIO
{
public:
    /**
     * @brief Construct a new Socket I O object
     *
     * @param fd 文件描述符
     */
    explicit SocketIO(int fd);
    ~SocketIO();

    /**
     * @brief 从内核缓冲区中固定读取n个字节数据
     *
     * @param buffer 用户缓冲区
     * @param n 读取的字节数
     * @return int 实际读取的字节数
     */
    int readn(char *buffer, int n);

    /**
     * @brief 从内核缓冲区读取一行数据，换行符为'\n'
     *
     * @param buffer 用户缓冲区
     * @param n 最大读取的字节数
     * @return int 实际读取的字节数
     */
    int readLine(char *buffer, int n);

    /**
     * @brief 向内核缓冲区中固定写入n个字节数据
     *
     * @param buffer 用户缓冲区
     * @param n 写入字节数
     * @return int 实际读取的字节数
     */
    int writen(const char *buffer, int n);

    /**
     * @brief 把内核缓冲区数据循环读取到用户缓冲区（读到 EAGAIN 为止）
     *
     * @param buffer 输入输出型参数
     * @param eof 输出参数：对端关闭（read 返回 0）时置 true，调用方应走关闭路径；
     *            本次调用可能既读到了数据又遇 EOF，两个信息都要传达给调用方
     * @return 成功读取的字节数, 返回-1意为出错
     */
    int readFromKernel(Buffer &buffer, bool &eof);

    /**
     * @brief 把用户输出缓冲区数据循环发送到内核缓冲区
     *
     * @param pos 当前缓冲区可以从pos位置开始读取，[0, pos)已有数据
     * @param outBuffer 输入输出型参数
     * @return int
     */
    int writeToKernel(Buffer &buffer);

private:
    int _fd;
};

#endif
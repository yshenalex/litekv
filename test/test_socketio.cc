/**
 * @file test_socketio.cc
 * @brief SocketIO 读写路径测试（socketpair，不需要监听端口）。
 *
 * 重点回归：
 *  - readFromKernel 的 EOF 语义：read 返回 0（对端半关闭）必须置 eof 标志，
 *    且可能"既读到数据又遇 EOF"，两个信息都要传达；
 *  - 恰好填满用户缓冲区不丢字节（配合 Buffer::next 的边界语义）；
 *  - 读循环读到 EAGAIN 才停（要求 fd 非阻塞）；writeToKernel 冲刷干净。
 */

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

#include "Buffer.hpp"
#include "Socket.h"
#include "SocketIO.h"
#include "test_util.hpp"

namespace
{
void makePair(int fds[2])
{
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    // readFromKernel 的读循环要求非阻塞 fd（与服务端连接 fd 一致）
    CHECK(Socket::setNonBlock(fds[1]));
}
} // namespace

int main()
{
    // 普通读取：一次读干净到 EAGAIN
    {
        int fds[2];
        makePair(fds);
        CHECK(::write(fds[0], "hello", 5) == 5);

        Buffer buf;
        bool eof = true;
        SocketIO io(fds[1]);
        int n = io.readFromKernel(buf, eof);
        CHECK_EQ(n, 5);
        CHECK(!eof);
        CHECK_EQ(buf.length(), 5u);
        CHECK(std::memcmp(buf.start(), "hello", 5) == 0);
        ::close(fds[0]);
        ::close(fds[1]);
    }

    // EOF：写端半关闭——本次既读到数据又遇 EOF，之后读直接 EOF
    {
        int fds[2];
        makePair(fds);
        CHECK(::write(fds[0], "bye", 3) == 3);
        ::close(fds[0]); // 半关闭（FIN）

        Buffer buf;
        bool eof = false;
        SocketIO io(fds[1]);
        int n = io.readFromKernel(buf, eof);
        CHECK_EQ(n, 3);
        CHECK(eof); // 数据 + EOF 同时上报
        CHECK(std::memcmp(buf.start(), "bye", 3) == 0);

        Buffer buf2;
        bool eof2 = false;
        int n2 = io.readFromKernel(buf2, eof2);
        CHECK_EQ(n2, 0);
        CHECK(eof2);
        ::close(fds[1]);
    }

    // 恰好填满用户缓冲区：边界推进不丢字节
    {
        int fds[2];
        makePair(fds);
        Buffer buf(8); // 容量恰为 8
        CHECK(::write(fds[0], "12345678", 8) == 8);

        SocketIO io(fds[1]);
        bool eof = true;
        int n = io.readFromKernel(buf, eof);
        CHECK_EQ(n, 8);
        CHECK(!eof);
        CHECK_EQ(buf.length(), 8u);
        CHECK(std::memcmp(buf.start(), "12345678", 8) == 0);
        ::close(fds[0]);
        ::close(fds[1]);
    }

    // writeToKernel：输出缓冲冲刷干净，对端按序收到
    {
        int fds[2];
        makePair(fds);
        Buffer buf;
        std::memcpy(buf.space(), "xyz", 3);
        CHECK(buf.next(3));

        SocketIO io(fds[1]);
        int total = io.writeToKernel(buf);
        CHECK_EQ(total, 3);
        CHECK_EQ(buf.length(), 0u);

        char r[8] = {0};
        CHECK(::read(fds[0], r, sizeof(r)) == 3);
        CHECK(std::memcmp(r, "xyz", 3) == 0);
        ::close(fds[0]);
        ::close(fds[1]);
    }

    std::printf("test_socketio ALL PASS\n");
    return 0;
}

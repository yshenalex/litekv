#ifndef __INETADDRESS_H__
#define __INETADDRESS_H__
#include <arpa/inet.h>
#include <string>

/**
 * @brief 封装sockaddr_in，使得相关网络字节序转换过程对调用者透明
 *
 */
class InetAddress
{
public:
    // 提供string类型点分十进制ip地址和uint16_t的端口号即可构造一个对象
    explicit InetAddress(const std::string &ip, uint16_t port);

    explicit InetAddress(const struct sockaddr_in &addr);

    ~InetAddress();

    // 点分十进制string类型ip地址
    std::string ip() const;

    // 16位无符号端口号
    uint16_t port() const;

    // 获取原始对象sockaddr_in类型的指针
    const struct sockaddr_in *getInetAddrPtr() const;

private:
    struct sockaddr_in _addr;
};

#endif
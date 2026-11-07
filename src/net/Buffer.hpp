#ifndef __BUFFER_HPP__
#define __BUFFER_HPP__
/**
 * @file Buffer.hpp
 * @brief 用户级缓冲区，作为 TcpConnection 连接的读入缓冲区和写出缓冲区。
 *
 * 只认字节，不认识任何协议或业务类型。
 */

#include <string>
#include <vector>

#include "Log.hpp"

/**
 * @brief 缓冲区类，作为TcpConnection连接的读入用户级缓冲区和写出用户级缓冲区
 * 优化了直接使用vector<char>直接使用erase效率低下问题, 优化了erase时机与策略
 * 不过由于其底层使用的仍然是vector，有的时候仍需要使用erase擦除，效率较低(使用双端队列+内存块将是最好的方法，此处暂不使用)
 */
class Buffer
{
public:
    Buffer(int initSize = 1024) : _M_buffer(initSize, 0), _M_start(0), _M_continue(0)
    {
    }
    ~Buffer()
    {
    }

    /**
     * @brief 返回缓冲区可用区域(可以接着填充)的起始位置
     * @return 返回char*类型, buffer可用区域起始指针
     */
    char *space()
    {
        return _M_buffer.data() + _M_continue;
    }

    /**
     * @brief 返回缓冲区有效区域的起始位置
     * @return 返回char*类型, buffer有效区域起始指针
     */
    char *start()
    {
        return _M_buffer.data() + _M_start;
    }

    /**
     * @brief 缓冲区有效区域第i个元素
     */
    char &operator[](size_t i)
    {
        return _M_buffer[_M_start + i];
    }

    /**
     * @brief 调整可用区域起始位置, 将下一次写入缓冲区时的起始位置后移动num位
     *
     * 允许恰好填满到容量边界（`>` 而不是 `>=`）；超出容量才拒绝推进。
     * 调用方必须检查返回值：false 表示 num 超过可用区域，游标不会前进。
     *
     * @return true 修改成功
     * @return false 修改失败
     */
    bool next(size_t num)
    {
        logMessage(DEBUG, "in Buffer::next, num = %zu", num);
        if (_M_continue + num > _capacity())
            return false;

        _M_continue += num;
        return true;
    }

    /**
     * @brief 缓冲区有效区域大小
     */
    size_t length() const
    {
        return _M_continue - _M_start;
    }

    /**
     * @brief 缓冲区可用区域大小
     */
    size_t remain() const
    {
        return _M_buffer.size() - _M_continue;
    }

    /**
     * @brief
     *
     * @param size 擦除用户视角下缓冲区[0, size - 1]的缓冲区数据, 也就是相对于_start的有效区域的大小
     * @return 擦除成功返回true
     * @return 擦除失败返回false
     */
    bool erase(size_t size)
    {
        if (size > length())
            return false;

        _M_start += size; // 更改有效区域起始位置，前面size字节已经被擦除

        // 关键：如果有效区域起始位置与可用区域起始位置相同，说明有效区域为空，前面都是无效区域，直接从头开始
        if (_M_start == _M_continue)
        {
            _M_start = _M_continue = 0;
        }

        // 我们设计erase结束时整理底层vector, 如果可用区域小于总容量的1/10时，就重整一下底层_buffer
        if (remain() <= (_capacity() / 10))
            _reorganize();

        return true;
    }

    /**
     * @brief 扩充缓冲区可用区域大小
     *
     * @param atLeast 扩容后可用区域至少要达到的字节数（默认 0 表示只要求 > 0）。
     *                一次 resize 保证满足 atLeast，不需要调用方循环调用。
     */
    void enlarge(size_t atLeast = 0)
    {
        // 先调用重整函数：把有效数据搬到头部，可能直接腾出足够空间
        _reorganize();

        if (remain() >= atLeast && remain() > 0)
            return;

        // 重整后仍不够，扩容底层 vector：至少翻倍，且保证能放下 atLeast
        size_t newCap = _capacity() * 2;
        if (newCap < _M_continue + atLeast)
            newCap = _M_continue + atLeast;
        _M_buffer.resize(newCap, 0);
    }

private:
    void _reorganize()
    {
        // 如果缓冲区已经没有可用区域了，此时没办法了，只能调用vector的erase函数整理底层_buffer
        if (_M_start > 0)
        {
            _M_buffer.erase(_M_buffer.begin(), _M_buffer.begin() + _M_start);
            _M_continue -= _M_start;
            _M_start = 0;
        }
    }

    /**
     * @brief 缓冲区底层结构容量大小
     */
    size_t _capacity() const
    {
        return _M_buffer.size();
    }

private:
    std::vector<char> _M_buffer;
    // 通过_start和_continue划分为3个区域:
    // [0, _start): 已经被erase擦除的无效区域
    // [_start, _continue): 当前缓冲区有效区域
    // [_continue, _buffer.size() - 1]: 缓冲区可用区域(未使用过区域)
    size_t _M_start;    // 有效区域开始的位置
    size_t _M_continue; // 缓冲区可用区域开始的位置
};

#endif

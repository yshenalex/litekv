#ifndef __ENTRY_HPP__
#define __ENTRY_HPP__

#include <atomic>
#include <cstdint>
#include <string>

/**
 * @file Entry.hpp
 * @brief 分片字典里的值条目（DESIGN §6.1）。
 *
 * key 由 `Shard::map` 持有，Entry 只存 value / expireMs / lastAccess；
 * Entry 由 `unique_ptr` 持有、地址稳定（unordered_map rehash 只失效迭代器，
 * 不失效元素指针），因此 `expires` 索引里的 `Entry*` 可以安全跨操作持有。
 * 禁止跨操作保存 map 迭代器（CALL-CONVENTIONS §8.3）。
 */
struct Entry
{
    std::string value;
    uint64_t expireMs = 0;                // 0 = 无 TTL；否则 CLOCK_REALTIME 绝对毫秒
                                          //（会被持久化，必须用墙钟，DESIGN §7.3）
    std::atomic<uint64_t> lastAccess{0};  // CLOCK_MONOTONIC 毫秒；采样 LRU 用
                                          //（只比间隔，读命令 shared_lock 下原子更新）
};

#endif

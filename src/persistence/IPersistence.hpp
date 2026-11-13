#ifndef __IPERSISTENCE_HPP__
#define __IPERSISTENCE_HPP__

#include <functional>
#include <vector>

#include "RespValue.hpp"

/**
 * @file IPersistence.hpp
 * @brief 持久化插件接口。
 *
 * 关键解耦点：`load()` 不认识 CommandTable——谁执行命令是 server 的事，
 * persistence 只管"把文件里的命令一条条交出去"（CommandReplayer 由 server 注入）。
 *
 * 调用约束：
 * - `load()`   只在启动、任何 SubReactor 启动前调用（单线程）；
 *              回放期间 `CommandContext::onWrite` 必须为空，否则会把 AOF 再追加一遍。
 * - `append()` 只允许在 `CommitHook` 内调用（持分片 unique_lock 时）：只追加到内存缓冲，
 *              禁止 fsync / 文件 I/O / 调用 store / 抛异常；缓冲超高水位必须返回
 *              `false`（背压 → 命令层回 `-MISCONF`），不得无界增长，
 *              也不得在锁内阻塞等待 fsync 追平。
 * - `maybeRewrite()` 只能由独立重写线程/主循环调用，禁止在命令处理路径同步调用
 *              （命令路径已持分片锁，再取全部分片锁会自锁）。
 * - `flush()`   在所有 loop join 之后调用（关闭前落盘）。
 * - 锁顺序固定：分片锁（升序）→ AOF 追加锁 / 重写缓冲锁，禁止反向。
 */

class ShardedStore; // maybeRewrite 需要；persistence 层只允许
                    // withAllShardsUnique + forEachEntryUnlocked

/** 把一条 RESP 命令交给命令层执行；false = 回放失败（load 应中止） */
using CommandReplayer = std::function<bool(const std::vector<RespValue> &)>;

class IPersistence
{
public:
    virtual ~IPersistence() = default;
    virtual bool load(CommandReplayer replayer) = 0;             // 启动时逐条回放
    virtual bool append(const std::vector<RespValue> &cmd) = 0;  // 持分片 unique_lock 时调用
    virtual void maybeRewrite(ShardedStore &store) = 0;          // 重写线程
    virtual void flush() = 0;                                    // 关闭时，单线程
};

/** 关闭态空实现（`appendonly no` 时用）：append 恒成功，什么都不做 */
class NullPersistence final : public IPersistence
{
public:
    bool load(CommandReplayer) override { return true; }
    bool append(const std::vector<RespValue> &) override { return true; }
    void maybeRewrite(ShardedStore &) override {}
    void flush() override {}
};

#endif

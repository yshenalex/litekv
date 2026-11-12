#ifndef __SHARDED_STORE_H__
#define __SHARDED_STORE_H__

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Entry.hpp"

/**
 * @file ShardedStore.h
 * @brief 存储层对外的唯一入口（DECOUPLE.md 契约 2，冻结勿改签名）。
 *
 * 16 分片 `unordered_map` + `shared_mutex` + 采样 LRU + TTL 过期索引（DESIGN §6）。
 * 分片计算、多键分组与升序加锁全部在 store 内部完成，命令层只传 key 列表
 * （CALL-CONVENTIONS §8）。store 不认识网络与协议类型，可被单测直接 new。
 *
 * ## CommitHook 契约（CALL-CONVENTIONS §8.2，违反即死锁/写盘乱序）
 *
 * - 执行时机：在相关分片的 `unique_lock` 内、内存修改完成之后、释放锁之前，
 *   同步执行一次。**禁止保存、延迟或带到锁外执行**——`CommandContext::commitHook()`
 *   生成的 lambda 捕获 `this`/`&args`，离开调用点即悬垂。
 * - 调用条件：只有真正改变了数据才调用（`SET` 只要 NX/XX 通过就视为修改，即使值相同；
 *   `SET NX` 未生效、`DEL` 删除 0 个、`EXPIRE` 键不存在、`PERSIST` 无 TTL、
 *   `APPEND` 空串且键已存在时都不调用）。
 * - 允许：追加到内存缓冲、记录计数器。禁止：调用 store/net/command、文件 I/O、
 *   `fsync`、抛异常、在锁内打日志。禁止在 hook 里调用 store（不可重入，会死锁）。
 * - 返回 false = AOF 追加失败：store 返回 `PersistenceError`（或经 `Status *err`
 *   传出），**不回滚**已写入内存的数据。
 *
 * ## 时间与过期语义
 *
 * - `expireMs` 是 CLOCK_REALTIME 绝对毫秒（可持久化）；`lastAccess` 是
 *   CLOCK_MONOTONIC 毫秒（只测间隔）。
 * - 所有过期参数（`SetOpts::exMs`、`expire` 的 `ms`）都是**相对时长**，
 *   墙钟换算在 store 内部完成，命令层不碰时钟。
 * - 过期 = 逻辑不存在：所有读命令按 miss 返回，**读锁内不删除**；写命令命中过期
 *   条目时顺带物理删除（结果仍按"不存在"计）；`sampleExpire` 定期抽样清理。
 * - `dbSize()` 返回原始条目数（含未清理的过期条目，与 Redis `dictSize` 同语义，
 *   O(1) 代价）；`keys()` / `forEachEntryUnlocked` 给逻辑视图，跳过过期条目。
 *
 * ## 其他实现约定
 *
 * - `shardNum` 自动向上取整到 2 的幂；`maxEntriesPerShard == 0` 表示不淘汰。
 *   淘汰：插入超配额时从主 map 随机采 K=5 个淘汰最旧（采样 LRU，近似）。
 *   淘汰选择是随机的、不落 AOF；崩溃重启后淘汰路径可能与原运行不一致（重写收敛），
 *   工程上只保证"数据不丢"，不保证淘汰集合可复现。
 * - `OutOfMemory`：分片已达配额且采样淘汰找不到可淘汰键时返回（防御性路径），
 *   命令层映射为 `-OOM ...`（CALL-CONVENTIONS §7.4）。
 * - `Status *err`（非 Status 返回值的写接口尾参）：传出 `PersistenceError` /
 *   `OutOfMemory` 等错误，正常时为 `Ok`；默认 nullptr 忽略。
 */

/** 分片内部结构（实现细节，因模板方法需在头文件可见）。DESIGN §6.1。 */
struct Shard
{
    std::unordered_map<std::string, std::unique_ptr<Entry>> map;
    std::unordered_map<std::string_view, Entry *> expires; // 只放带 TTL 的键；
                                                           // string_view 指向 map 的 key
                                                           //（地址稳定），删除时先 erase
                                                           // expires 再 erase map
    mutable std::shared_mutex mtx;
    size_t maxEntries = 0; // 每片配额；0 = 不淘汰
};

class ShardedStore
{
public:
    enum class Status { Ok, NotFound, NotInt, OutOfRange, ConditionNotMet, PersistenceError, OutOfMemory };
    struct SetOpts
    {
        // 显式构造函数而非 NSDMI：嵌套类的 NSDMI 在外围类默认参数（= {}）里
        // 使用过早（GCC 报 "required before the end of its enclosing class"）
        SetOpts(bool nx_ = false, bool xx_ = false, long long exMs_ = 0)
            : nx(nx_), xx(xx_), exMs(exMs_) {}
        bool nx;
        bool xx;
        long long exMs; // 相对毫秒 TTL；<=0 表示不设 TTL（非法 EX 值由命令层校验）
    };
    using CommitHook = std::function<bool()>; // false = AOF 追加失败

    ShardedStore(size_t shardNum = 16, size_t maxEntriesPerShard = 0);

    size_t shardNum() const { return shards_.size(); }
    size_t maxEntriesPerShard() const { return shards_.empty() ? 0 : shards_[0].maxEntries; }

    // ---- 读：内部 shared_lock；命中只更新 lastAccess，不移动任何结构 ----
    std::optional<std::string> get(const std::string &key);
    long long exists(const std::vector<std::string> &keys) const; // 计数语义，重复键重复计
    long long ttl(const std::string &key) const;                  // -2 不存在 / -1 无 TTL / 剩余毫秒
    std::vector<std::string> keys(const std::string &pattern) const; // glob；逻辑视图
    size_t dbSize() const; // 原始条目数（含未清理过期条目），非原子快照

    // ---- 写：内部 unique_lock；onCommit 在锁内、修改完成后同步调用 ----
    Status set(const std::string &key, std::string val, const SetOpts &o = {},
               CommitHook onCommit = {});
    long long del(const std::vector<std::string> &keys, CommitHook onCommit = {},
                  Status *err = nullptr); // 返回删除数（过期条目按不存在计）
    bool expire(const std::string &key, long long ms, CommitHook onCommit = {},
                Status *err = nullptr); // ms <= 0 立即删除；true = 键存在
    bool persist(const std::string &key, CommitHook onCommit = {},
                 Status *err = nullptr); // true = TTL 被移除
    Status incrBy(const std::string &key, long long delta, long long &result,
                  CommitHook onCommit = {});
    long long appendValue(const std::string &key, const std::string &val,
                          CommitHook onCommit = {}, Status *err = nullptr); // 返回新长度
    bool flushDb(CommitHook onCommit = {}, Status *err = nullptr); // false = PersistenceError

    // ---- 快照/重写（persistence 层专用）----
    // 内部按升序取全部分片 unique_lock 执行 fn；fn 内不得再调用 store 方法。
    template <typename Fn>
    void withAllShardsUnique(Fn &&fn)
    {
        std::vector<std::unique_lock<std::shared_mutex>> locks;
        locks.reserve(shards_.size());
        for (auto &sh : shards_) { // 分片号升序加锁（全局锁序，CALL-CONVENTIONS §11.1）
            locks.emplace_back(sh.mtx);
        }
        fn();
    }

    // 只在 withAllShardsUnique 的 fn 内调用；本身不加锁。回调签名
    // fn(const std::string &key, const Entry &e)，必须短、不可调用 store、
    // 不可保存 key/Entry 指针/string_view。跳过已过期条目（逻辑视图）。
    template <typename Fn>
    void forEachEntryUnlocked(Fn &&fn) const
    {
        uint64_t now = nowMsRealtime();
        for (const auto &sh : shards_) {
            for (const auto &kv : sh.map) {
                const Entry &e = *kv.second;
                if (e.expireMs != 0 && e.expireMs <= now) continue;
                fn(kv.first, e);
            }
        }
    }

    // 定期过期抽样：一次只拿一个分片锁，从 expires 随机采样（默认 20 个/轮）
    // 过期即删，过期比例 > 25% 再来一轮；禁止全库遍历。只由负责该分片的 loop 调用。
    void sampleExpire(size_t shardIndex);

private:
    static uint64_t nowMsRealtime();  // CLOCK_REALTIME 毫秒（expireMs 用）
    static uint64_t nowMsMonotonic(); // CLOCK_MONOTONIC 毫秒（lastAccess 用）
    static bool isExpired(const Entry &e, uint64_t nowRt);

    size_t shardIndex(const std::string &key) const
    {
        return std::hash<std::string>{}(key) & mask_;
    }

    // 删除顺序固定：先 expires.erase，再 map.erase（避免 string_view 悬垂）
    void eraseEntry(Shard &sh, std::unordered_map<std::string, std::unique_ptr<Entry>>::iterator it);
    // 同步 expires 索引：expireMs != 0 则插入/更新，否则移除
    void syncExpires(Shard &sh, std::unordered_map<std::string, std::unique_ptr<Entry>>::iterator it);
    // 插入前淘汰到配额以下；找不到可淘汰键返回 false
    bool ensureCapacity(Shard &sh);
    // 随机采 K=5 个候选淘汰最旧，成功返回 true
    bool evictOne(Shard &sh);
    // hook 为空视为成功；失败不回滚、不打日志（CALL-CONVENTIONS §8.2）
    static bool hookOk(const CommitHook &hook) { return !hook || hook(); }

    std::deque<Shard> shards_; // Shard 含 shared_mutex 不可移动，deque 就地构造
    size_t mask_;
};

#endif

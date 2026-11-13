#ifndef __AOF_PLUGIN_H__
#define __AOF_PLUGIN_H__

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "IPersistence.hpp"

/**
 * @file AofPlugin.h
 * @brief AOF 持久化插件（DESIGN §7.2）：RESP 文本追加 + everysec 后台 fsync + AOF 重写。
 *
 * 文件格式与 Redis AOF 同构（原始 RESP 命令帧），`redis-cli --pipe < aof` 可直接回放。
 *
 * ## 线程模型（CALL-CONVENTIONS §3）
 *
 * - **SubReactor 线程**：只调 `append()`（经 CommitHook，在分片 unique_lock 内）——
 *   仅追加到用户态缓冲，无系统调用、无日志。
 * - **fsync 线程**（本类内部）：每 `fsyncIntervalMs` 把追加缓冲 swap 出来
 *   `write` + `fsync` 一次（everysec）；顺带回收退役 fd。禁止碰 store。
 * - **重写线程**（server 侧独立线程，定期调 `maybeRewrite(store)`）：
 *   触发重写时取全部分片锁序列化快照，全程与 fsync 线程互不阻塞（除文件写互斥）。
 *
 * ## 锁协议（违反即死锁/写盘乱序）
 *
 * - `mu_`：保护 buf_/rewriteBuf_/stale_/rewriting_/fd_/retired_ 等状态。
 *   **绝不跨文件 I/O 持有**（append 在分片锁内，不能被 write/fsync 阻塞）。
 * - `fileMu_`：串行化 write/fsync/open/close 等系统调用。与 `mu_` 不嵌套。
 * - 锁序：分片锁 → `mu_`（重写切换/append 用）；`fileMu_` 独立，永不与 `mu_` 嵌套。
 *
 * ## 重写协议（CALL-CONVENTIONS §9.7，核心正确性）
 *
 * 1. `withAllShardsUnique`（持全部分片锁，写者全部被阻塞）内：
 *    把 buf_ 里未落盘内容挪进 `stale_`（其内存效果必被随后的快照覆盖——CommitHook 在
 *    内存修改完成后才调用），置 `rewriting_=true`（此后 append 进 rewriteBuf_），
 *    然后 `forEachEntryUnlocked` 序列化快照（SET + PEXPIRE 相对毫秒）。
 * 2. 释放分片锁 → 写临时文件 + fsync → `rename` 原子替换（重写缓冲的起点与
 *    "取全部分片锁"瞬间对齐：锁释放前不可能有新 append）。
 * 3. 把 rewriteBuf_（快照之后的命令）写入新文件 → 翻转：剩余 rewriteBuf_ 并入
 *    buf_、`fd_` 换新、退出重写模式、丢弃 stale_（效果已在快照里）。
 *    翻转前完成尾部写入，保证新文件内的顺序恒为 快照 < 尾部 < 后续追加。
 *
 * 任何一步失败（rename 之前）走 `abortRewrite()`：stale_ + rewriteBuf_ 按序并回
 * buf_，写路径无缝恢复，不丢一条命令。rename 之后的失败是防御性路径：置 broken_
 * 让后续 `append()` 返回 false（命令层回 -MISCONF），绝不静默丢已应答数据之外的命令。
 *
 * ## 语义边界
 *
 * - everysec：进程崩溃（kill -9）最多丢约 1 秒未落盘命令；`flush()` 后不丢。
 * - 采样 LRU 淘汰 / 过期清理不落 AOF（重启后淘汰集合可能不同，重写收敛）。
 * - 高水位（默认 64MB）内只增缓冲；超限 `append()` 返回 false，不阻塞、不打日志。
 */
class AofPlugin final : public IPersistence
{
public:
    /**
     * @param path             AOF 文件路径（不存在则创建）
     * @param bufferHighWater  追加缓冲（buf_+rewriteBuf_ 总和）高水位，超限 append 返回 false
     * @param rewriteMinSize   重写最小文件尺寸：文件不足它即使翻倍也不重写
     * @param fsyncIntervalMs  fsync 线程周期（生产默认 1000 = everysec；测试可调大做确定性）
     */
    explicit AofPlugin(std::string path,
                       size_t bufferHighWater = 64 << 20,
                       size_t rewriteMinSize = 64 << 20,
                       unsigned fsyncIntervalMs = 1000);
    ~AofPlugin() override;

    // 启动期单线程调用：逐帧解析 AOF 交 replayer 执行；残帧/损坏帧 truncate 到
    // 最后一个完整帧偏移（RespParser::consumed()）。文件不存在按空 AOF 成功。
    bool load(CommandReplayer replayer) override;
    // 只追加到内存缓冲（CommitHook 锁内调用）；false = 超高水位（背压）或已损坏
    bool append(const std::vector<RespValue> &cmd) override;
    // 阈值（文件 > 上次重写后 2 倍且 >= rewriteMinSize）或 requestRewrite() 后执行重写。
    // 只允许重写线程/主循环调用（同步执行，禁入命令路径）
    void maybeRewrite(ShardedStore &store) override;
    // 关闭前落盘：停 fsync 线程 → 排空全部缓冲 → fsync（不关 fd，析构时关）
    void flush() override;

    // BGREWRITEAOF 手动触发：只置标志，重写在 maybeRewrite 所在线程执行（线程安全）
    void requestRewrite();

private:
    AofPlugin(const AofPlugin &) = delete;
    AofPlugin &operator=(const AofPlugin &) = delete;

    void fsyncLoop();                            // everysec 后台线程体
    void stopFsyncThread();                      // 幂等：置 stop_ → 唤醒 → join
    void doRewrite(ShardedStore &store);         // 重写全程（阶段 1-3，见头注释）
    void abortRewrite();                         // rename 前失败回滚：缓冲按序并回 buf_
    void buildSnapshot(ShardedStore &store, std::string &out); // 锁内序列化 SET/PEXPIRE
    // 写全（处理短写/EINTR）；只在持有 fileMu_ 时调用
    static bool writeAll(int fd, const char *data, size_t len);
    static uint64_t nowRealtimeMs();             // CLOCK_REALTIME 毫秒（PEXPIRE 换算用）

    // ---- 配置（构造后不变）----
    std::string path_;
    std::string tmpPath_;       // 重写临时文件 path_ + ".tmp"
    size_t highWater_;
    size_t rewriteMinSize_;
    unsigned fsyncIntervalMs_;

    // ---- mu_ 保护的状态 ----
    std::mutex mu_;
    std::condition_variable cv_;   // 唤醒 fsync 线程（stop 用）
    std::string buf_;              // 正常追加缓冲
    std::string rewriteBuf_;       // 重写缓冲（rewriting_ 期间的 append）
    std::string stale_;            // 重写切换时挪出的 buf_ 残留；成功丢弃/失败并回
    bool rewriting_ = false;       // true：append 进 rewriteBuf_
    bool stop_ = false;            // fsync 线程退出标志
    bool broken_ = false;          // 灾难性失败后置位，append 恒 false
    int fd_ = -1;                  // 当前 AOF 文件（O_WRONLY|O_APPEND）
    std::vector<int> retired_;     // rename 顶掉的旧 fd，fsync 线程/flush 回收
    std::thread fsyncThread_;

    // ---- fileMu_ 串行化文件系统调用（与 mu_ 不嵌套）----
    std::mutex fileMu_;

    // ---- 重写触发与规模（跨线程原子读写）----
    std::atomic<bool> rewriteRequested_{false};
    std::atomic<uint64_t> fileBytes_{0};       // 当前文件字节数（近似，阈值判断用）
    std::atomic<uint64_t> lastRewriteSize_{0}; // 上次重写后的文件字节数
};

#endif

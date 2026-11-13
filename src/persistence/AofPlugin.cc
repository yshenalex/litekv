/**
 * @file AofPlugin.cc
 * @brief AOF 插件实现：RESP 追加 + everysec fsync + 进程内重写。
 * 并发协议与重写协议见 AofPlugin.h 头注释。
 */

#include "AofPlugin.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include "Log.hpp"
#include "RespEncoder.h"
#include "RespParser.h"
#include "ShardedStore.h"

namespace
{

// 命令帧 → RESP 数组编码（AOF 与请求格式同构，redis-cli --pipe 可回放）
std::string encodeCmd(const std::vector<std::string> &parts)
{
    RespValue arr;
    arr.type = RespValue::Type::Array;
    arr.array.reserve(parts.size());
    for (const auto &p : parts) {
        RespValue b;
        b.type = RespValue::Type::BulkString;
        b.str = p;
        arr.array.push_back(std::move(b));
    }
    return RespEncoder::encode(arr);
}

std::string dirOf(const std::string &path)
{
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

// 命令帧校验：Array 非空且全部元素为字符串类型（BulkString/SimpleString）。
// load 回放只接受这种帧，其余按损坏处理
bool isCommandFrame(const RespValue &v)
{
    if (v.type != RespValue::Type::Array || v.array.empty()) return false;
    for (const auto &e : v.array) {
        if (e.type != RespValue::Type::BulkString &&
            e.type != RespValue::Type::SimpleString)
            return false;
    }
    return true;
}

} // namespace

AofPlugin::AofPlugin(std::string path, size_t bufferHighWater,
                     size_t rewriteMinSize, unsigned fsyncIntervalMs)
    : path_(std::move(path)), tmpPath_(path_ + ".tmp"), highWater_(bufferHighWater),
      rewriteMinSize_(rewriteMinSize), fsyncIntervalMs_(fsyncIntervalMs)
{
    // O_APPEND：所有写入恒追加到文件尾，fsync 线程与重写尾部写并发也有序
    fd_ = ::open(path_.c_str(), O_RDWR | O_APPEND | O_CREAT, 0644);
    if (fd_ < 0) {
        logMessage(ERROR, "AofPlugin: open %s failed: %s", path_.c_str(),
                   std::strerror(errno));
    }
    fsyncThread_ = std::thread(&AofPlugin::fsyncLoop, this);
}

AofPlugin::~AofPlugin()
{
    flush(); // 停 fsync 线程 + 排空缓冲 + fsync（幂等）
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    for (int d : retired_)
        if (d >= 0) ::close(d);
    retired_.clear();
}

// ---------------- append（CommitHook 锁内，只碰内存） ----------------

bool AofPlugin::append(const std::vector<RespValue> &cmd)
{
    // 参数归一化成 BulkString 数组（inline 命令已由 parser 归一化；这里兜底
    // Integer 等异常参数，保证 AOF 里只有字符串帧，回放时不会被当损坏）
    std::vector<std::string> parts;
    parts.reserve(cmd.size());
    for (const auto &c : cmd) {
        if (c.type == RespValue::Type::Integer)
            parts.push_back(std::to_string(c.integer));
        else
            parts.push_back(c.str);
    }
    std::string bytes = encodeCmd(parts);

    std::lock_guard<std::mutex> lk(mu_);
    if (broken_ || fd_ < 0) return false;
    // 背压：超高水位立即 false（→ store 回 PersistenceError → -MISCONF），
    // 不无界增长、不阻塞等 fsync 追平、不在锁内打日志
    if (buf_.size() + rewriteBuf_.size() + bytes.size() > highWater_) return false;
    (rewriting_ ? rewriteBuf_ : buf_).append(bytes);
    return true;
}

// ---------------- load（启动期单线程） ----------------

bool AofPlugin::load(CommandReplayer replayer)
{
    if (fd_ < 0) return false;
    if (::lseek(fd_, 0, SEEK_SET) < 0) return false;

    RespParser parser;
    char chunk[64 * 1024];
    size_t keep = 0;      // 最后一个完整帧的结束偏移（truncate 目标）
    bool corrupt = false; // 遇到损坏帧/协议错误：从该处截断丢弃

    for (;;) {
        ssize_t n = ::read(fd_, chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            logMessage(ERROR, "AofPlugin::load: read %s failed: %s", path_.c_str(),
                       std::strerror(errno));
            return false;
        }
        bool eof = (n == 0);
        parser.feed(chunk, static_cast<size_t>(n));

        for (;;) {
            size_t before = parser.consumed(); // 帧起点偏移（损坏帧从这里丢）
            RespValue frame;
            ParseStatus st = parser.next(frame);
            if (st == ParseStatus::NeedMore) break;
            if (st == ParseStatus::Error) {
                // 协议错误：parser 已毒化，无法重同步，整条尾巴丢弃。
                // consumed() 停在最后一个完整帧结束处
                keep = parser.consumed();
                corrupt = true;
                break;
            }
            if (!isCommandFrame(frame)) {
                // 能解析但不是命令帧（如裸 Integer）：按损坏处理，连同其后一起丢
                keep = before;
                corrupt = true;
                break;
            }
            if (!replayer(frame.array)) {
                logMessage(ERROR, "AofPlugin::load: replay failed at offset %zu",
                           before);
                return false;
            }
            keep = parser.consumed();
        }
        if (corrupt || eof) break;
    }

    // 文件尾残帧（宕机于半条命令）或损坏点之后：truncate 到最后完整帧偏移
    off_t size = ::lseek(fd_, 0, SEEK_END);
    if (size < 0) return false;
    if (static_cast<size_t>(size) > keep) {
        logMessage(WARNING,
                   "AofPlugin::load: %s truncated %lld -> %zu bytes (%s)",
                   path_.c_str(), (long long)size, keep,
                   corrupt ? "corrupt frame" : "incomplete tail frame");
        if (::ftruncate(fd_, static_cast<off_t>(keep)) != 0) {
            logMessage(ERROR, "AofPlugin::load: ftruncate %s failed: %s",
                       path_.c_str(), std::strerror(errno));
            return false;
        }
        ::fsync(fd_);
    }
    fileBytes_.store(keep);
    lastRewriteSize_.store(keep); // 重写阈值以此为基线
    return true;
}

// ---------------- fsync 线程（everysec） ----------------

void AofPlugin::fsyncLoop()
{
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        // 谓词等待：stop_ 时立即醒来（否则大间隔配置下 join 会等满一个周期）
        cv_.wait_for(lk, std::chrono::milliseconds(fsyncIntervalMs_),
                     [this] { return stop_; });
        if (stop_) break;

        // 回收退役 fd：捕获只发生在本线程同一轮内，此刻无人再持有
        std::vector<int> dead;
        dead.swap(retired_);
        std::string local;
        local.swap(buf_);
        int f = fd_;

        lk.unlock();
        {
            std::lock_guard<std::mutex> fl(fileMu_);
            for (int d : dead)
                if (d >= 0) ::close(d);
            if (!local.empty() && f >= 0) {
                // 写捕获到的 f（可能是 rename 顶掉的旧 fd——写进被替换的旧文件
                // 是无害垃圾：其内容的内存效果必被重写快照覆盖，见头注释）
                if (writeAll(f, local.data(), local.size())) {
                    ::fsync(f);
                    fileBytes_ += local.size(); // 近似值，阈值判断够用
                } else {
                    logMessage(ERROR, "AofPlugin: write %s failed: %s",
                               path_.c_str(), std::strerror(errno));
                }
            }
        }
        lk.lock();
    }
}

void AofPlugin::stopFsyncThread()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true; // 幂等
    }
    cv_.notify_all();
    if (fsyncThread_.joinable()) fsyncThread_.join();
}

// ---------------- flush / 重写 ----------------

void AofPlugin::flush()
{
    stopFsyncThread();
    // 单线程语义：按 stale_ → rewriteBuf_ → buf_
    // 的时间序合并写出（防御性：正常关闭时前两者为空）
    std::string local;
    int f;
    {
        std::lock_guard<std::mutex> lk(mu_);
        local.reserve(stale_.size() + rewriteBuf_.size() + buf_.size());
        local.append(stale_);
        local.append(rewriteBuf_);
        local.append(buf_);
        stale_.clear();
        rewriteBuf_.clear();
        buf_.clear();
        rewriting_ = false;
        f = fd_;
    }
    std::vector<int> dead;
    {
        std::lock_guard<std::mutex> lk(mu_);
        dead.swap(retired_);
    }
    {
        std::lock_guard<std::mutex> fl(fileMu_);
        for (int d : dead)
            if (d >= 0) ::close(d);
        if (!local.empty() && f >= 0 && writeAll(f, local.data(), local.size())) {
            fileBytes_ += local.size();
        }
        if (f >= 0) ::fsync(f);
    }
}

void AofPlugin::requestRewrite()
{
    rewriteRequested_.store(true);
}

void AofPlugin::maybeRewrite(ShardedStore &store)
{
    bool forced = rewriteRequested_.exchange(false);
    if (!forced) {
        uint64_t sz = fileBytes_.load();
        uint64_t base = lastRewriteSize_.load();
        if (sz < rewriteMinSize_ || sz < 2 * base) return;
    }
    doRewrite(store);
}

void AofPlugin::buildSnapshot(ShardedStore &store, std::string &out)
{
    uint64_t now = nowRealtimeMs();
    store.forEachEntryUnlocked([&](const std::string &key, const Entry &e) {
        long long rel = 0;
        if (e.expireMs != 0) {
            if (e.expireMs <= now) return; // 序列化期间刚过期：逻辑不存在，跳过
            rel = static_cast<long long>(e.expireMs - now);
        }
        out += encodeCmd({"set", key, e.value});
        if (e.expireMs != 0) {
            // 相对毫秒（命令表只有 PEXPIRE）。回放时重新起算，与命令式 AOF 的
            // EXPIRE 语义一致（漂移 = 重写到重启的间隔，秒级，可接受）
            out += encodeCmd({"pexpire", key, std::to_string(rel)});
        }
    });
}

void AofPlugin::doRewrite(ShardedStore &store)
{
    // ---- 阶段 1：持全部分片锁 → 切换重写缓冲 + 序列化快照----
    std::string snapshot;
    store.withAllShardsUnique([&] {
        {
            std::lock_guard<std::mutex> lk(mu_);
            // buf_ 未落盘内容的内存效果必被随后的快照覆盖（CommitHook 在内存
            // 修改完成后才回调；切换时写者全被分片锁阻塞）→ 成功后可安全丢弃。
            stale_.swap(buf_);
            rewriteBuf_.clear();
            rewriting_ = true;
        }
        buildSnapshot(store, snapshot);
    });

    // ---- 阶段 2：写临时文件 + 原子 rename（文件 I/O 不持 mu_）----
    int tmpFd = ::open(tmpPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = (tmpFd >= 0);
    if (ok) {
        ok = writeAll(tmpFd, snapshot.data(), snapshot.size());
        if (ok) ok = (::fsync(tmpFd) == 0);
        ::close(tmpFd);
    }
    if (ok) ok = (::rename(tmpPath_.c_str(), path_.c_str()) == 0);
    if (!ok) {
        logMessage(ERROR, "AofPlugin: rewrite %s failed: %s", path_.c_str(),
                   std::strerror(errno));
        ::unlink(tmpPath_.c_str());
        abortRewrite();
        return;
    }
    // rename 成功后 durability 边界：fsync 目录项（防掉电丢 rename）
    {
        int dfd = ::open(dirOf(path_).c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }

    // ---- 阶段 3：尾部（快照之后的命令）写入新文件 → 翻转 ----
    int newFd = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (newFd < 0) {
        // 腾一个 fd 位再试：旧 fd 已被 rename 顶掉（写它=丢数据），先退役
        //（退役 fd 由 fsync 线程下一轮回收，不在此处 close——防捕获者 use-after-close）
        std::lock_guard<std::mutex> lk(mu_);
        if (fd_ >= 0) retired_.push_back(fd_);
        fd_ = -1;
    }
    if (newFd < 0) newFd = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (newFd < 0) {
        // 灾难性防御路径：数据在快照里，尾部命令无法落盘 → broken_，
        // 后续 append 恒 false（-MISCONF 通知客户端），绝不静默丢应答数据
        logMessage(ERROR, "AofPlugin: reopen %s after rewrite failed: %s",
                   path_.c_str(), std::strerror(errno));
        std::lock_guard<std::mutex> lk(mu_);
        broken_ = true;
        stale_.clear(); // 效果已在快照里
        buf_.swap(rewriteBuf_); // 尾部命令并回 buf_（尽量抢救）
        rewriting_ = false;
        return;
    }

    std::string tail;
    {
        std::lock_guard<std::mutex> lk(mu_);
        tail.swap(rewriteBuf_); // rewriting_ 仍为 true：此间 append 继续进 rewriteBuf_
    }
    uint64_t bytes = snapshot.size();
    if (!tail.empty()) {
        std::lock_guard<std::mutex> fl(fileMu_);
        if (writeAll(newFd, tail.data(), tail.size()))
            bytes += tail.size();
        else
            logMessage(ERROR, "AofPlugin: rewrite tail write failed: %s",
                       std::strerror(errno));
    }
    {
        // 翻转必须在尾部写完之后：此后 fsync 线程才可能捕获到 newFd，
        // 新文件内顺序恒为 快照 < 尾部 < 后续追加
        std::lock_guard<std::mutex> lk(mu_);
        buf_.insert(0, rewriteBuf_); // 残余（尾部写期间的 append），排在后续之前
        rewriteBuf_.clear();
        if (fd_ >= 0) retired_.push_back(fd_);
        fd_ = newFd;
        rewriting_ = false;
        stale_.clear();
    }
    {
        std::lock_guard<std::mutex> fl(fileMu_);
        ::fsync(newFd);
    }
    fileBytes_.store(bytes);
    lastRewriteSize_.store(bytes);
    rewriteRequested_.store(false); // 丢弃重写期间的重复请求（状态已全量持久化）
    logMessage(NORMAL, "AofPlugin: rewrite %s done, %llu bytes", path_.c_str(),
               (unsigned long long)bytes);
}

void AofPlugin::abortRewrite()
{
    std::lock_guard<std::mutex> lk(mu_);
    // 按时间序并回 buf_：stale_（切换前）< rewriteBuf_（重写期间）< 后续 append。
    // fsync 线程照常把 buf_ 写入旧 fd（未 rename，旧文件仍然有效）→ 一条不丢
    stale_.append(rewriteBuf_);
    rewriteBuf_.clear();
    buf_.insert(0, stale_);
    stale_.clear();
    rewriting_ = false;
}

// ---------------- 工具 ----------------

bool AofPlugin::writeAll(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

uint64_t AofPlugin::nowRealtimeMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 +
           static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

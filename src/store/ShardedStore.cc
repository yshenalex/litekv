/**
 * @file ShardedStore.cc
 * @brief 16 分片 KV 存储：采样 LRU 淘汰 + TTL 过期索引（DESIGN §6）。
 *
 * 纯数据结构，不碰网络/协议；锁语义见 CALL-CONVENTIONS §8：
 * 读 shared_lock（命中只戳 lastAccess），写 unique_lock（顺带惰性删除、淘汰、
 * 锁内回调 CommitHook），多键写按键分组、分片号升序加锁。
 */

#include "ShardedStore.h"

#include <algorithm>
#include <ctime>
#include <functional>
#include <limits>
#include <random>
#include <thread>

namespace
{

constexpr int kEvictSample = 5;   // LRU 淘汰每轮随机采样数（DESIGN §6.2）
constexpr int kExpireSample = 20; // 过期抽样每轮上限（DESIGN §6.3）
constexpr int kSampleTries = 64;  // 随机 bucket 探测次数上限
constexpr int kExpireRounds = 16; // sampleExpire 轮数上限（每轮至少删 1 才会续轮）

std::mt19937_64 &tlsRng()
{
    static thread_local std::mt19937_64 rng{
        std::random_device{}() ^
        (uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id())};
    return rng;
}

/** Redis string2ll 语义：可选 '-' + 数字，除 "0"/"-0" 外禁止前导零，禁 '+'/空白。 */
bool parseLL(const std::string &s, long long &out)
{
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (s.size() == 1) return false;
    }
    if (s[i] == '0') {
        if (s.size() - i > 1) return false; // 前导零
        out = 0;
        return true;
    }
    unsigned long long acc = 0;
    const unsigned long long limit =
        neg ? (unsigned long long)std::numeric_limits<long long>::max() + 1ull
            : (unsigned long long)std::numeric_limits<long long>::max();
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        if (acc > (limit - (unsigned long long)(c - '0')) / 10ull) return false; // 溢出 → 非整数
        acc = acc * 10ull + (unsigned long long)(c - '0');
    }
    if (neg && acc == limit) {
        out = std::numeric_limits<long long>::min();
    } else {
        out = neg ? -(long long)acc : (long long)acc;
    }
    return true;
}

/** Redis glob：* ? [abc] [a-z] [^a-z] \转义；大小写敏感。 */
bool globMatchRecurse(std::string_view p, std::string_view s)
{
    if (p.empty()) return s.empty();
    if (p[0] == '*') {
        size_t k = 0;
        while (k < p.size() && p[k] == '*') ++k;
        std::string_view rest = p.substr(k);
        for (size_t i = 0; i <= s.size(); ++i) {
            if (globMatchRecurse(rest, s.substr(i))) return true;
        }
        return false;
    }
    if (s.empty()) return false;
    switch (p[0]) {
    case '?':
        return globMatchRecurse(p.substr(1), s.substr(1));
    case '[': {
        size_t i = 1;
        bool neg = false;
        if (i < p.size() && p[i] == '^') {
            neg = true;
            ++i;
        }
        bool matched = false;
        bool first = true;
        bool closed = false;
        while (i < p.size()) {
            if (p[i] == ']' && !first) {
                closed = true;
                ++i;
                break;
            }
            first = false;
            char lo = p[i];
            if (lo == '\\' && i + 1 < p.size()) lo = p[++i];
            ++i;
            if (i + 1 < p.size() && p[i] == '-' && p[i + 1] != ']') {
                ++i; // 跳过 '-'
                char hi = p[i];
                if (hi == '\\' && i + 1 < p.size()) hi = p[++i];
                ++i;
                unsigned char c = (unsigned char)s[0];
                if (c >= (unsigned char)lo && c <= (unsigned char)hi) matched = true;
            } else {
                if (s[0] == lo) matched = true;
            }
        }
        if (!closed) { // 无闭合 ']'：按字面匹配 '['
            return s[0] == '[' && globMatchRecurse(p.substr(1), s.substr(1));
        }
        if (matched != neg) return globMatchRecurse(p.substr(i), s.substr(1));
        return false;
    }
    case '\\':
        if (p.size() > 1) {
            return p[1] == s[0] && globMatchRecurse(p.substr(2), s.substr(1));
        }
        return '\\' == s[0] && globMatchRecurse(p.substr(1), s.substr(1));
    default:
        return p[0] == s[0] && globMatchRecurse(p.substr(1), s.substr(1));
    }
}

bool globMatch(std::string_view p, std::string_view s)
{
    return globMatchRecurse(p, s);
}

} // namespace

ShardedStore::ShardedStore(size_t shardNum, size_t maxEntriesPerShard)
{
    size_t n = shardNum ? shardNum : 16;
    size_t p = 1;
    while (p < n) p <<= 1; // shardNum 必须是 2 的幂，向上取整（CALL-CONVENTIONS §8.3）
    for (size_t i = 0; i < p; ++i) {
        shards_.emplace_back();
        shards_.back().maxEntries = maxEntriesPerShard;
    }
    mask_ = p - 1;
}

uint64_t ShardedStore::nowMsRealtime()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

uint64_t ShardedStore::nowMsMonotonic()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

bool ShardedStore::isExpired(const Entry &e, uint64_t nowRt)
{
    return e.expireMs != 0 && e.expireMs <= nowRt;
}

void ShardedStore::eraseEntry(Shard &sh,
                              std::unordered_map<std::string, std::unique_ptr<Entry>>::iterator it)
{
    if (it->second->expireMs != 0) {
        sh.expires.erase(it->first); // 顺序固定：先 expires 后 map
    }
    sh.map.erase(it);
}

void ShardedStore::syncExpires(Shard &sh,
                               std::unordered_map<std::string, std::unique_ptr<Entry>>::iterator it)
{
    if (it->second->expireMs != 0) {
        sh.expires.insert_or_assign(it->first, it->second.get());
    } else {
        sh.expires.erase(it->first);
    }
}

bool ShardedStore::evictOne(Shard &sh)
{
    if (sh.map.empty()) return false;
    const std::string *cand = nullptr;
    uint64_t oldest = 0;
    auto consider = [&](Entry *e, const std::string &key) {
        uint64_t la = e->lastAccess.load(std::memory_order_relaxed);
        if (!cand || la < oldest) {
            cand = &key;
            oldest = la;
        }
    };
    if (sh.map.size() <= (size_t)kEvictSample * 4) {
        // 小 map：全量比较，确定性（随机探测在稀疏 bucket 下可能漏采）
        for (auto &kv : sh.map) {
            consider(kv.second.get(), kv.first);
        }
    } else {
        size_t bc = sh.map.bucket_count();
        auto &rng = tlsRng();
        int found = 0;
        for (int tries = 0; tries < kSampleTries && found < kEvictSample; ++tries) {
            size_t b = rng() % bc;
            if (sh.map.bucket_size(b) == 0) continue;
            auto bit = sh.map.begin(b);
            consider(bit->second.get(), bit->first);
            ++found;
        }
        if (!cand) {
            // 稀疏 map（大量删除后 bucket 数远大于条目数）随机探测可能全部落空，
            // 回退取任意一条；map 非空即有可淘汰键，"找不到"只在空 map 时成立。
            cand = &sh.map.begin()->first;
        }
    }
    auto it = sh.map.find(*cand);
    if (it == sh.map.end()) return false;
    eraseEntry(sh, it);
    return true;
}

bool ShardedStore::ensureCapacity(Shard &sh)
{
    if (sh.maxEntries == 0) return true;
    while (sh.map.size() >= sh.maxEntries) {
        if (!evictOne(sh)) return false; // OutOfMemory（防御性路径）
    }
    return true;
}

// ---------------- 读 ----------------

std::optional<std::string> ShardedStore::get(const std::string &key)
{
    Shard &sh = shards_[shardIndex(key)];
    std::shared_lock<std::shared_mutex> lock(sh.mtx);
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return std::nullopt;
    Entry &e = *it->second;
    if (isExpired(e, nowMsRealtime())) return std::nullopt; // 读锁内不删除
    e.lastAccess.store(nowMsMonotonic(), std::memory_order_relaxed);
    return e.value;
}

long long ShardedStore::exists(const std::vector<std::string> &keys) const
{
    // 读多键：逐键取单分片 shared_lock（读不保证跨片原子，天然无死锁）；
    // 与 Redis 一致，重复键重复计数。
    long long n = 0;
    uint64_t nowRt = nowMsRealtime();
    uint64_t nowAcc = nowMsMonotonic();
    for (const auto &key : keys) {
        const Shard &sh = shards_[shardIndex(key)];
        std::shared_lock<std::shared_mutex> lock(sh.mtx);
        auto it = sh.map.find(key);
        if (it == sh.map.end()) continue;
        Entry *e = it->second.get();
        if (isExpired(*e, nowRt)) continue;
        e->lastAccess.store(nowAcc, std::memory_order_relaxed);
        ++n;
    }
    return n;
}

long long ShardedStore::ttl(const std::string &key) const
{
    const Shard &sh = shards_[shardIndex(key)];
    std::shared_lock<std::shared_mutex> lock(sh.mtx);
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return -2;
    Entry *e = it->second.get();
    uint64_t nowRt = nowMsRealtime();
    if (isExpired(*e, nowRt)) return -2; // 读锁内不删除
    e->lastAccess.store(nowMsMonotonic(), std::memory_order_relaxed);
    if (e->expireMs == 0) return -1;
    return (long long)(e->expireMs - nowRt); // 剩余毫秒
}

std::vector<std::string> ShardedStore::keys(const std::string &pattern) const
{
    // global 命令：逐分片取锁、逻辑视图（跳过过期），定义为非原子快照
    std::vector<std::string> out;
    uint64_t nowRt = nowMsRealtime();
    for (const auto &sh : shards_) {
        std::shared_lock<std::shared_mutex> lock(sh.mtx);
        for (const auto &kv : sh.map) {
            if (isExpired(*kv.second, nowRt)) continue;
            if (globMatch(pattern, kv.first)) out.push_back(kv.first);
        }
    }
    return out;
}

size_t ShardedStore::dbSize() const
{
    // 原始条目数（含未清理过期条目，Redis dictSize 同语义），非原子快照
    size_t n = 0;
    for (const auto &sh : shards_) {
        std::shared_lock<std::shared_mutex> lock(sh.mtx);
        n += sh.map.size();
    }
    return n;
}

// ---------------- 写 ----------------

ShardedStore::Status ShardedStore::set(const std::string &key, std::string val,
                                       const SetOpts &o, CommitHook onCommit)
{
    if (o.nx && o.xx) return Status::ConditionNotMet; // 命令层应已拒绝，防御
    Shard &sh = shards_[shardIndex(key)];
    std::unique_lock<std::shared_mutex> lock(sh.mtx);
    uint64_t nowRt = nowMsRealtime();
    uint64_t nowAcc = nowMsMonotonic();

    auto it = sh.map.find(key);
    if (it != sh.map.end() && isExpired(*it->second, nowRt)) {
        eraseEntry(sh, it); // 写路径惰性删除
        it = sh.map.end();
    }
    bool existsNow = (it != sh.map.end());
    if (o.nx && existsNow) return Status::ConditionNotMet;
    if (o.xx && !existsNow) return Status::ConditionNotMet;

    uint64_t exp = (o.exMs > 0) ? nowRt + (uint64_t)o.exMs : 0;
    if (!existsNow) {
        if (!ensureCapacity(sh)) return Status::OutOfMemory;
        auto res = sh.map.try_emplace(key, std::make_unique<Entry>());
        it = res.first;
    }
    it->second->value = std::move(val);
    it->second->expireMs = exp;
    it->second->lastAccess.store(nowAcc, std::memory_order_relaxed);
    syncExpires(sh, it);

    if (!hookOk(onCommit)) return Status::PersistenceError;
    return Status::Ok;
}

long long ShardedStore::del(const std::vector<std::string> &keys, CommitHook onCommit, Status *err)
{
    if (err) *err = Status::Ok;
    // 涉及分片去重、升序加锁（全局锁序，CALL-CONVENTIONS §11.1），
    // 全部删除完成后在锁内调用一次 hook（多键命令只追加一条 AOF）。
    std::vector<size_t> shardIds;
    shardIds.reserve(keys.size());
    for (const auto &key : keys) shardIds.push_back(shardIndex(key));
    std::sort(shardIds.begin(), shardIds.end());
    shardIds.erase(std::unique(shardIds.begin(), shardIds.end()), shardIds.end());

    std::vector<std::unique_lock<std::shared_mutex>> locks;
    locks.reserve(shardIds.size());
    for (size_t id : shardIds) locks.emplace_back(shards_[id].mtx);

    long long removed = 0;
    uint64_t nowRt = nowMsRealtime();
    for (const auto &key : keys) {
        Shard &sh = shards_[shardIndex(key)];
        auto it = sh.map.find(key);
        if (it == sh.map.end()) continue;
        bool dead = isExpired(*it->second, nowRt);
        eraseEntry(sh, it); // 过期条目按不存在计数，只物理删除
        if (!dead) ++removed;
    }
    if (removed > 0 && !hookOk(onCommit)) {
        if (err) *err = Status::PersistenceError;
    }
    return removed;
}

bool ShardedStore::expire(const std::string &key, long long ms, CommitHook onCommit, Status *err)
{
    if (err) *err = Status::Ok;
    Shard &sh = shards_[shardIndex(key)];
    std::unique_lock<std::shared_mutex> lock(sh.mtx);
    uint64_t nowRt = nowMsRealtime();
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return false;
    if (isExpired(*it->second, nowRt)) {
        eraseEntry(sh, it); // 惰性删除；逻辑上键不存在
        return false;
    }
    if (ms <= 0) { // Redis：非正 TTL 立即删除
        eraseEntry(sh, it);
        if (!hookOk(onCommit)) {
            if (err) *err = Status::PersistenceError;
        }
        return true;
    }
    it->second->expireMs = nowRt + (uint64_t)ms;
    it->second->lastAccess.store(nowMsMonotonic(), std::memory_order_relaxed);
    syncExpires(sh, it);
    if (!hookOk(onCommit)) {
        if (err) *err = Status::PersistenceError;
    }
    return true;
}

bool ShardedStore::persist(const std::string &key, CommitHook onCommit, Status *err)
{
    if (err) *err = Status::Ok;
    Shard &sh = shards_[shardIndex(key)];
    std::unique_lock<std::shared_mutex> lock(sh.mtx);
    uint64_t nowRt = nowMsRealtime();
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return false;
    if (isExpired(*it->second, nowRt)) {
        eraseEntry(sh, it);
        return false;
    }
    if (it->second->expireMs == 0) return false;
    it->second->expireMs = 0;
    sh.expires.erase(it->first);
    it->second->lastAccess.store(nowMsMonotonic(), std::memory_order_relaxed);
    if (!hookOk(onCommit)) {
        if (err) *err = Status::PersistenceError;
    }
    return true;
}

ShardedStore::Status ShardedStore::incrBy(const std::string &key, long long delta,
                                          long long &result, CommitHook onCommit)
{
    Shard &sh = shards_[shardIndex(key)];
    std::unique_lock<std::shared_mutex> lock(sh.mtx);
    uint64_t nowRt = nowMsRealtime();
    uint64_t nowAcc = nowMsMonotonic();

    auto it = sh.map.find(key);
    if (it != sh.map.end() && isExpired(*it->second, nowRt)) {
        eraseEntry(sh, it);
        it = sh.map.end();
    }
    long long cur = 0;
    if (it != sh.map.end()) {
        if (!parseLL(it->second->value, cur)) return Status::NotInt;
    }
    if ((delta > 0 && cur > std::numeric_limits<long long>::max() - delta) ||
        (delta < 0 && cur < std::numeric_limits<long long>::min() - delta)) {
        return Status::OutOfRange; // 溢出不改内存
    }
    result = cur + delta;
    if (it == sh.map.end()) {
        if (!ensureCapacity(sh)) return Status::OutOfMemory;
        it = sh.map.try_emplace(key, std::make_unique<Entry>()).first;
    }
    it->second->value = std::to_string(result);
    // INCR 保留已有 TTL（Redis 语义）；新建键无 TTL
    it->second->lastAccess.store(nowAcc, std::memory_order_relaxed);
    if (!hookOk(onCommit)) return Status::PersistenceError;
    return Status::Ok;
}

long long ShardedStore::appendValue(const std::string &key, const std::string &val,
                                    CommitHook onCommit, Status *err)
{
    if (err) *err = Status::Ok;
    Shard &sh = shards_[shardIndex(key)];
    std::unique_lock<std::shared_mutex> lock(sh.mtx);
    uint64_t nowRt = nowMsRealtime();

    auto it = sh.map.find(key);
    if (it != sh.map.end() && isExpired(*it->second, nowRt)) {
        eraseEntry(sh, it);
        it = sh.map.end();
    }
    if (it == sh.map.end()) {
        if (!ensureCapacity(sh)) {
            if (err) *err = Status::OutOfMemory;
            return 0;
        }
        it = sh.map.try_emplace(key, std::make_unique<Entry>()).first;
        it->second->value = val;
        it->second->lastAccess.store(nowMsMonotonic(), std::memory_order_relaxed);
        if (!hookOk(onCommit)) {
            if (err) *err = Status::PersistenceError;
        }
        return (long long)it->second->value.size();
    }
    if (val.empty()) return (long long)it->second->value.size(); // 无变化，不回调
    it->second->value.append(val); // 二进制安全，按长度追加
    it->second->lastAccess.store(nowMsMonotonic(), std::memory_order_relaxed);
    if (!hookOk(onCommit)) {
        if (err) *err = Status::PersistenceError;
    }
    return (long long)it->second->value.size();
}

bool ShardedStore::flushDb(CommitHook onCommit, Status *err)
{
    if (err) *err = Status::Ok;
    std::vector<std::unique_lock<std::shared_mutex>> locks;
    locks.reserve(shards_.size());
    for (auto &sh : shards_) locks.emplace_back(sh.mtx); // 分片号升序

    bool any = false;
    for (auto &sh : shards_) {
        if (!sh.map.empty()) {
            sh.expires.clear();
            sh.map.clear();
            any = true;
        }
    }
    if (any && !hookOk(onCommit)) {
        if (err) *err = Status::PersistenceError;
        return false;
    }
    return true;
}

void ShardedStore::sampleExpire(size_t shardIndex)
{
    if (shardIndex >= shards_.size()) return;
    Shard &sh = shards_[shardIndex];
    std::unique_lock<std::shared_mutex> lock(sh.mtx);
    auto &rng = tlsRng();

    for (int round = 0; round < kExpireRounds; ++round) {
        if (sh.expires.empty()) return;
        uint64_t nowRt = nowMsRealtime();
        struct Cand
        {
            std::string_view key;
            Entry *e;
        };
        std::vector<Cand> cands;
        cands.reserve(kExpireSample);
        if (sh.expires.size() <= (size_t)kExpireSample * 2) {
            // 小集合全量取样，保证清理干净（随机探测可能漏采/重复采）
            for (auto &kv : sh.expires) {
                cands.push_back(Cand{kv.first, kv.second});
            }
        } else {
            // 随机 bucket 采样（expires 只含带 TTL 的键，命中率高）；去重防 double-erase
            size_t bc = sh.expires.bucket_count();
            for (int tries = 0; tries < kSampleTries && (int)cands.size() < kExpireSample; ++tries) {
                size_t b = rng() % bc;
                if (sh.expires.bucket_size(b) == 0) continue;
                auto bit = sh.expires.begin(b);
                bool dup = false;
                for (const Cand &c : cands) {
                    if (c.e == bit->second) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
                cands.push_back(Cand{bit->first, bit->second});
            }
        }
        if (cands.empty()) return;

        int expiredCnt = 0;
        for (const Cand &c : cands) {
            if (isExpired(*c.e, nowRt)) {
                ++expiredCnt;
                sh.expires.erase(c.key);
                sh.map.erase(std::string(c.key)); // 顺序固定：先 expires 后 map
            }
        }
        // 过期比例 > 25% 再来一轮（DESIGN §6.3）
        if (expiredCnt * 4 <= (int)cands.size()) return;
    }
}

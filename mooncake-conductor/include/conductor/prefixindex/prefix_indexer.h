#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "conductor/prefixindex/types.h"

namespace conductor {
namespace prefixindex {

struct RegistrationResult {
    bool inserted = false;
    std::string error;
};

struct BlockPresence {
    std::set<EngineOwner> gpu_owners;
    std::set<SharedObjectOwner> cpu_owners;
    std::set<SharedObjectOwner> disk_owners;

    bool Empty() const {
        return gpu_owners.empty() && cpu_owners.empty() && disk_owners.empty();
    }
};

// blocks 的条数上限, 超出后按写入顺序淘汰最旧的。
//
// 索引是 vLLM HBM 状态的**镜像**而不是缓存, 所以这个上限只是兜底:
// 单实例 HBM 容量 9127 block, 8P 合计约 73K, 索引全局去重后同量级 ——
// 正常情况永不触发。它防的是"stored 进了、removed 丢了"造成的单调膨胀:
// 一旦失配, unordered_map 反复 rehash、内存暴涨、插入与查询同步变慢,
// 消费跟不上又丢更多 removed, 正反馈直到完全失效。
constexpr size_t kDefaultMaxBlocks = 200000;
// 触发淘汰时一次降到上限的这个比例, 避免每插一条都淘汰一条。
constexpr double kEvictTargetRatio = 0.9;

struct ContextState {
    explicit ContextState(HashProfile registered_profile,
                          size_t block_limit = kDefaultMaxBlocks)
        : profile(std::move(registered_profile)), max_blocks(block_limit) {}

    // Lock order is global context-map mutex, then this mutex. Code holding
    // this mutex must never reacquire the global mutex.
    mutable std::shared_mutex mutex;
    const HashProfile profile;
    std::map<std::string, std::set<int64_t>> instance_ranks;
    std::unordered_map<ProjectedPrefix, BlockPresence> blocks;

    // 写入顺序链表, 表头最新、表尾最旧。只在 Store 时更新位置, Query 不更新 ——
    // 查询若也算"访问"就得把 shared_lock 升级成 unique_lock, 读并发在调度热路径上
    // 直接消失。退化成 FIFO 反而更贴合镜像语义: vLLM 侧本就按 LRU 驱逐,
    // 越早写入的 block 越可能已经不在 HBM 里。
    std::list<ProjectedPrefix> write_order;
    std::unordered_map<ProjectedPrefix, std::list<ProjectedPrefix>::iterator>
        order_pos;
    const size_t max_blocks;
    // 因容量上限被淘汰的累计条数。非零说明 stored/removed 已失配, 需要排查事件丢失。
    int64_t evicted_by_capacity = 0;
};

struct RankCacheHitResult {
    int64_t gpu = 0;
    int64_t cpu = 0;
    int64_t disk = 0;

    bool operator==(const RankCacheHitResult&) const = default;
};

struct CacheHitResult {
    int64_t longest_match_tokens = 0;
    std::map<int64_t, int64_t> dp;
    std::map<int64_t, RankCacheHitResult> rank_matches;
    int64_t gpu = 0;
    int64_t cpu = 0;
    int64_t disk = 0;
};

struct ContextView {
    ContextKey context;
    HashProfile profile;
    std::map<std::string, std::set<int64_t>> instance_ranks;
    size_t prefix_count = 0;
};

struct GlobalView {
    int32_t context_count = 0;
    std::vector<ContextView> contexts;
};

class PrefixCacheTable {
   public:
    PrefixCacheTable() = default;
    // block_limit 是每个 ContextKey 的 blocks 条数上限(0 表示不限)。
    // 可注入是为了单测能用小上限验证淘汰, 也便于按机群实际 HBM 容量调整
    // 而不必重编译。
    explicit PrefixCacheTable(size_t block_limit)
        : block_limit_(block_limit) {}
    PrefixCacheTable(const PrefixCacheTable&) = delete;
    PrefixCacheTable& operator=(const PrefixCacheTable&) = delete;

    static RegistrationResult ValidateRegistration(
        const EngineRegistration& registration);

    RegistrationResult Register(const EngineRegistration& registration);
    std::string ValidateProfileBinding(const ContextKey& context,
                                       const HashProfile& profile) const;
    std::string Unregister(const ContextKey& context,
                           const std::string& instance_id, int64_t dp_rank);

    std::string StoreGpu(const GpuMutation& mutation);
    std::string RemoveGpu(const GpuMutation& mutation);
    std::string ClearGpu(const GpuClear& clear);

    std::string StoreShared(const SharedMutation& mutation);
    std::string RemoveShared(const SharedMutation& mutation);
    std::string ClearShared(const SharedClear& clear);

    std::map<std::string, CacheHitResult> Query(
        const ContextKey& context, std::span<const int32_t> token_ids,
        std::optional<std::string> cache_salt = std::nullopt,
        std::optional<std::string> instance_filter = std::nullopt) const;

    GlobalView GetGlobalView() const;

   private:
    friend class PrefixCacheTableTestPeer;

    std::shared_ptr<ContextState> LoadContextState(
        const ContextKey& context) const;

    mutable std::shared_mutex context_map_mutex_;
    std::unordered_map<ContextKey, std::shared_ptr<ContextState>> contexts_;
    const size_t block_limit_ = kDefaultMaxBlocks;
};

}  // namespace prefixindex
}  // namespace conductor

#include "conductor/prefixindex/prefix_indexer.h"

#include <glog/logging.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

#include "conductor/prefixindex/hash_strategy.h"

namespace conductor {
namespace prefixindex {

namespace {

std::string ValidateContext(const ContextKey& context) {
    if (context.tenant_id.empty()) {
        return "tenant_id is required";
    }
    if (context.model_name.empty()) {
        return "model_name is required";
    }
    if (context.block_size <= 0) {
        return "block_size must be positive";
    }
    return "";
}

std::string ValidateLayout(const ContextKey& context,
                           int64_t effective_block_size,
                           std::optional<int64_t> cache_group) {
    if (auto error = ValidateContext(context); !error.empty()) {
        return error;
    }
    if (effective_block_size <= 0) {
        return "effective_block_size must be positive";
    }
    if (effective_block_size != context.block_size) {
        return "effective_block_size must equal ContextKey block_size";
    }
    if (cache_group.has_value() && *cache_group != 0) {
        return "only cache group 0 is supported";
    }
    return "";
}

std::string ValidateEngineOwner(const EngineOwner& owner) {
    if (owner.source_stream.empty()) {
        return "engine owner source_stream is required";
    }
    if (owner.instance_id.empty()) {
        return "engine owner instance_id is required";
    }
    if (owner.dp_rank < 0) {
        return "engine owner dp_rank must be non-negative";
    }
    return "";
}

std::string ValidateSharedOwner(const SharedObjectOwner& owner) {
    if (owner.source_stream.empty()) {
        return "shared owner source_stream is required";
    }
    if (owner.backend_id.empty()) {
        return "shared owner backend_id is required";
    }
    if (owner.object_id.empty()) {
        return "shared owner object_id is required";
    }
    return "";
}

std::string ValidateGpuMutation(const GpuMutation& mutation) {
    if (auto error =
            ValidateLayout(mutation.context, mutation.effective_block_size,
                           mutation.cache_group);
        !error.empty()) {
        return error;
    }
    return ValidateEngineOwner(mutation.owner);
}

std::string ValidateGpuClear(const GpuClear& clear) {
    if (auto error = ValidateLayout(clear.context, clear.effective_block_size,
                                    clear.cache_group);
        !error.empty()) {
        return error;
    }
    return ValidateEngineOwner(clear.owner);
}

bool IsSharedTier(StorageTier tier) {
    return tier == StorageTier::kCpu || tier == StorageTier::kDisk;
}

std::string ValidateSharedMutation(const SharedMutation& mutation) {
    if (auto error =
            ValidateLayout(mutation.context, mutation.effective_block_size,
                           mutation.cache_group);
        !error.empty()) {
        return error;
    }
    if (!IsSharedTier(mutation.tier)) {
        return "shared mutation tier must be CPU or DISK";
    }
    return ValidateSharedOwner(mutation.owner);
}

std::string ValidateSharedClear(const SharedClear& clear) {
    if (auto error = ValidateLayout(clear.context, clear.effective_block_size,
                                    clear.cache_group);
        !error.empty()) {
        return error;
    }
    if (clear.tier.has_value() && !IsSharedTier(*clear.tier)) {
        return "shared clear tier must be CPU, DISK, or omitted";
    }
    return ValidateSharedOwner(clear.owner);
}

std::set<SharedObjectOwner>& SharedOwners(BlockPresence& presence,
                                          StorageTier tier) {
    return tier == StorageTier::kCpu ? presence.cpu_owners
                                     : presence.disk_owners;
}

// 全表扫描清空块。只给 Clear*/Unregister 这类本身就是全表语义的低频操作用;
// Remove* 走的是按 prefix 就地清理(见 RemoveGpu 的注释), 不能再调这个。
void EraseEmptyBlocks(ContextState& state) {
    std::erase_if(state.blocks, [&state](const auto& item) {
        if (!item.second.Empty()) {
            return false;
        }
        auto pos = state.order_pos.find(item.first);
        if (pos != state.order_pos.end()) {
            state.write_order.erase(pos->second);
            state.order_pos.erase(pos);
        }
        return true;
    });
}

// 从写入顺序链表里摘掉一个 prefix。blocks 与 write_order 必须同步增删,
// 否则链表会攒下悬空项, 淘汰时按它们去删已不存在的 block, 白跑一轮还删不掉东西。
void ForgetOrder(ContextState& state, ProjectedPrefix prefix) {
    auto pos = state.order_pos.find(prefix);
    if (pos != state.order_pos.end()) {
        state.write_order.erase(pos->second);
        state.order_pos.erase(pos);
    }
}

// 记录/刷新一个 prefix 的写入位置, 表头最新。要求已持 state.mutex 写锁。
void TouchOrder(ContextState& state, ProjectedPrefix prefix) {
    auto pos = state.order_pos.find(prefix);
    if (pos != state.order_pos.end()) {
        state.write_order.splice(state.write_order.begin(), state.write_order,
                                 pos->second);
        return;
    }
    state.write_order.push_front(prefix);
    state.order_pos.emplace(prefix, state.write_order.begin());
}

// 超过容量上限时从表尾(最旧)批量淘汰到 max_blocks × kEvictTargetRatio。
// 批量而非逐条, 是为了不让每次插入都付一次淘汰成本。要求已持写锁。
void EvictIfOverCapacity(ContextState& state) {
    if (state.max_blocks == 0 || state.blocks.size() <= state.max_blocks) {
        return;
    }
    const size_t target =
        static_cast<size_t>(state.max_blocks * kEvictTargetRatio);
    while (state.blocks.size() > target && !state.write_order.empty()) {
        const ProjectedPrefix oldest = state.write_order.back();
        state.write_order.pop_back();
        state.order_pos.erase(oldest);
        state.blocks.erase(oldest);
        ++state.evicted_by_capacity;
    }
    LOG_EVERY_N(WARNING, 100)
        << "Prefix index hit the capacity limit; oldest entries dropped."
        << " limit=" << state.max_blocks << " now=" << state.blocks.size()
        << " cumulative_evicted=" << state.evicted_by_capacity
        << " (non-zero means stored/removed events are out of sync)";
}

int64_t TokensForBlocks(size_t block_count, int64_t block_size) {
    const uint64_t max_blocks =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / block_size);
    if (block_count > max_blocks) {
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(block_count) * block_size;
}

}  // namespace

RegistrationResult PrefixCacheTable::ValidateRegistration(
    const EngineRegistration& registration) {
    if (auto error = ValidateLayout(registration.context,
                                    registration.effective_block_size,
                                    registration.cache_group);
        !error.empty()) {
        return {.error = std::move(error)};
    }
    if (registration.instance_id.empty()) {
        return {.error = "instance_id is required"};
    }
    if (registration.dp_rank < 0) {
        return {.error = "dp_rank must be non-negative"};
    }
    if (auto error = ValidateHashProfile(registration.profile);
        !error.empty()) {
        return {.error = std::move(error)};
    }
    return {};
}

RegistrationResult PrefixCacheTable::Register(
    const EngineRegistration& registration) {
    if (auto validation = ValidateRegistration(registration);
        !validation.error.empty()) {
        return validation;
    }

    auto candidate =
        std::make_shared<ContextState>(registration.profile, block_limit_);
    candidate->instance_ranks[registration.instance_id].insert(
        registration.dp_rank);

    std::shared_ptr<ContextState> state;
    {
        std::unique_lock map_lock(context_map_mutex_);
        auto [it, inserted] =
            contexts_.try_emplace(registration.context, std::move(candidate));
        if (inserted) {
            return {.inserted = true, .error = ""};
        }
        state = it->second;
    }

    std::unique_lock state_lock(state->mutex);
    if (state->profile != registration.profile) {
        return {.error =
                    "registration conflicts with the ContextKey hash profile"};
    }
    const bool inserted = state->instance_ranks[registration.instance_id]
                              .insert(registration.dp_rank)
                              .second;
    return {.inserted = inserted, .error = ""};
}

std::shared_ptr<ContextState> PrefixCacheTable::LoadContextState(
    const ContextKey& context) const {
    std::shared_lock map_lock(context_map_mutex_);
    auto it = contexts_.find(context);
    return it == contexts_.end() ? nullptr : it->second;
}

std::string PrefixCacheTable::ValidateProfileBinding(
    const ContextKey& context, const HashProfile& profile) const {
    if (auto error = ValidateContext(context); !error.empty()) {
        return error;
    }
    if (auto error = ValidateHashProfile(profile); !error.empty()) {
        return error;
    }
    auto state = LoadContextState(context);
    if (!state) {
        return "ContextKey is not registered";
    }

    std::shared_lock state_lock(state->mutex);
    if (state->profile != profile) {
        return "hash profile conflicts with the registered ContextKey profile";
    }
    return "";
}

std::string PrefixCacheTable::Unregister(const ContextKey& context,
                                         const std::string& instance_id,
                                         int64_t dp_rank) {
    if (auto error = ValidateContext(context); !error.empty()) {
        return error;
    }
    if (instance_id.empty()) {
        return "instance_id is required";
    }
    if (dp_rank < 0) {
        return "dp_rank must be non-negative";
    }

    auto state = LoadContextState(context);
    if (!state) {
        return "";
    }

    std::unique_lock state_lock(state->mutex);
    auto instance = state->instance_ranks.find(instance_id);
    if (instance != state->instance_ranks.end()) {
        instance->second.erase(dp_rank);
        if (instance->second.empty()) {
            state->instance_ranks.erase(instance);
        }
    }

    for (auto& [unused_prefix, presence] : state->blocks) {
        (void)unused_prefix;
        std::erase_if(presence.gpu_owners, [&](const EngineOwner& owner) {
            return owner.instance_id == instance_id && owner.dp_rank == dp_rank;
        });
    }
    EraseEmptyBlocks(*state);
    return "";
}

std::string PrefixCacheTable::StoreGpu(const GpuMutation& mutation) {
    if (auto error = ValidateGpuMutation(mutation); !error.empty()) {
        return error;
    }
    auto state = LoadContextState(mutation.context);
    if (!state) {
        return "ContextKey is not registered";
    }

    std::unique_lock state_lock(state->mutex);
    auto instance = state->instance_ranks.find(mutation.owner.instance_id);
    if (instance == state->instance_ranks.end() ||
        !instance->second.contains(mutation.owner.dp_rank)) {
        return "engine owner instance/rank is not registered";
    }
    for (ProjectedPrefix prefix : mutation.prefixes) {
        state->blocks[prefix].gpu_owners.insert(mutation.owner);
        TouchOrder(*state, prefix);
    }
    EvictIfOverCapacity(*state);
    return "";
}

std::string PrefixCacheTable::RemoveGpu(const GpuMutation& mutation) {
    if (auto error = ValidateGpuMutation(mutation); !error.empty()) {
        return error;
    }
    auto state = LoadContextState(mutation.context);
    if (!state) {
        return "";
    }

    std::unique_lock state_lock(state->mutex);
    // 只清理本次动到的 prefix。原先在末尾调 EraseEmptyBlocks 全表扫描,
    // 而 removed 事件在驱逐期极密集 —— 每条事件扫一遍百万级 blocks,
    // 且全程持写锁, 消费线程被 O(N) 吃满 -> SUB 缓冲堆积 -> HWM 溢出丢消息
    // -> 丢掉的 removed 让索引更删不掉 -> 索引继续膨胀。正反馈到完全失效。
    // 这里改成 O(涉及的 prefix 数)。
    for (ProjectedPrefix prefix : mutation.prefixes) {
        auto block = state->blocks.find(prefix);
        if (block != state->blocks.end()) {
            block->second.gpu_owners.erase(mutation.owner);
            if (block->second.Empty()) {
                state->blocks.erase(block);
                ForgetOrder(*state, prefix);
            }
        }
    }
    return "";
}

std::string PrefixCacheTable::ClearGpu(const GpuClear& clear) {
    if (auto error = ValidateGpuClear(clear); !error.empty()) {
        return error;
    }
    auto state = LoadContextState(clear.context);
    if (!state) {
        return "";
    }

    std::unique_lock state_lock(state->mutex);
    for (auto& [unused_prefix, presence] : state->blocks) {
        (void)unused_prefix;
        presence.gpu_owners.erase(clear.owner);
    }
    EraseEmptyBlocks(*state);
    return "";
}

std::string PrefixCacheTable::StoreShared(const SharedMutation& mutation) {
    if (auto error = ValidateSharedMutation(mutation); !error.empty()) {
        return error;
    }
    auto state = LoadContextState(mutation.context);
    if (!state) {
        return "ContextKey is not registered";
    }

    std::unique_lock state_lock(state->mutex);
    for (ProjectedPrefix prefix : mutation.prefixes) {
        SharedOwners(state->blocks[prefix], mutation.tier)
            .insert(mutation.owner);
        TouchOrder(*state, prefix);
    }
    EvictIfOverCapacity(*state);
    return "";
}

std::string PrefixCacheTable::RemoveShared(const SharedMutation& mutation) {
    if (auto error = ValidateSharedMutation(mutation); !error.empty()) {
        return error;
    }
    auto state = LoadContextState(mutation.context);
    if (!state) {
        return "";
    }

    std::unique_lock state_lock(state->mutex);
    // 同 RemoveGpu: 只清理本次动到的 prefix, 不再全表扫描。
    for (ProjectedPrefix prefix : mutation.prefixes) {
        auto block = state->blocks.find(prefix);
        if (block != state->blocks.end()) {
            SharedOwners(block->second, mutation.tier).erase(mutation.owner);
            if (block->second.Empty()) {
                state->blocks.erase(block);
                ForgetOrder(*state, prefix);
            }
        }
    }
    return "";
}

std::string PrefixCacheTable::ClearShared(const SharedClear& clear) {
    if (auto error = ValidateSharedClear(clear); !error.empty()) {
        return error;
    }
    auto state = LoadContextState(clear.context);
    if (!state) {
        return "";
    }

    std::unique_lock state_lock(state->mutex);
    for (auto& [unused_prefix, presence] : state->blocks) {
        (void)unused_prefix;
        if (!clear.tier.has_value() || *clear.tier == StorageTier::kCpu) {
            presence.cpu_owners.erase(clear.owner);
        }
        if (!clear.tier.has_value() || *clear.tier == StorageTier::kDisk) {
            presence.disk_owners.erase(clear.owner);
        }
    }
    EraseEmptyBlocks(*state);
    return "";
}

std::map<std::string, CacheHitResult> PrefixCacheTable::Query(
    const ContextKey& context, std::span<const int32_t> token_ids,
    std::optional<std::string> cache_salt,
    std::optional<std::string> instance_filter) const {
    std::map<std::string, CacheHitResult> results;
    auto state = LoadContextState(context);
    if (!state) {
        return results;
    }

    // profile 是 const 且构造后不再变, 而我们已持 shared_ptr 保活 state,
    // 所以建 strategy/chain 不需要索引锁。哈希只依赖 profile 与 token_ids,
    // 把它留在锁内会让每次查询持读锁做上千次 SHA256 —— glibc 的
    // pthread_rwlock_t 默认读者优先, 连续重叠的读者会把等待中的写者饿死,
    // 写者一被推迟 ZMQ 缓冲就堆积, 堆到 HWM 就丢事件。
    std::string strategy_error;
    auto strategy = CreateHashStrategy(state->profile, &strategy_error);
    if (!strategy) {
        LOG(ERROR) << "Registered hash profile became invalid: "
                   << strategy_error;
        return results;
    }

    std::string chain_error;
    auto chain = strategy->CreateChain(context, token_ids,
                                       std::move(cache_salt), &chain_error);
    if (!chain) {
        LOG(ERROR) << "Query hash chain setup failed: " << chain_error;
        return results;
    }
    const size_t block_count = chain->BlockCount();

    // 先选实例再探深度: filter 指向未知实例时要立刻返回, 不能先白算一轮哈希。
    // 这里**拷贝** ranks 而不是像原先那样存 &instance->second —— 原实现全程
    // 单锁所以指针安全, 现在锁分成了两段, 并发 Unregister 会 erase 掉那项,
    // 跨锁持指针就悬空了。8 实例各 1 rank, 拷贝成本可忽略。
    std::map<std::string, std::set<int64_t>> selected_instances;
    {
        std::shared_lock select_lock(state->mutex);
        if (instance_filter.has_value()) {
            auto instance = state->instance_ranks.find(*instance_filter);
            if (instance == state->instance_ranks.end()) {
                return results;
            }
            selected_instances.emplace(instance->first, instance->second);
        } else {
            selected_instances = state->instance_ranks;
        }
    }
    if (selected_instances.empty()) {
        return results;
    }

    // 粗探匹配深度: 判据取"block 在索引里存在", 它是下面每条 per-instance/tier
    // 判据的**超集**, 所以探出的深度是所有游标终值的上界。
    //
    // 分块进行: 每块先在锁外把哈希算出来(chain 内部 memoize), 再持读锁只做
    // find() 走完这一块。这样既保住了原有的惰性 —— 早早失配的查询不会为
    // 未触及的尾部付哈希成本 —— 又把锁内的持有时间压到"一块的查表"。
    // 块大小从小起步再倍增: 固定大块会让"本该在 block 0 就失配"的冷查询
    // 白算一整块, 而缓存真冷时冷查询正是常态; 固定小块又会让深匹配反复取锁。
    // 倍增两头都照顾到 —— 冷查询只多算几块, 深匹配的取锁次数是对数级。
    constexpr size_t kProbeChunkMin = 8;
    constexpr size_t kProbeChunkMax = 512;
    size_t chunk = kProbeChunkMin;
    size_t probe_depth = 0;
    bool probe_stalled = false;
    while (!probe_stalled && probe_depth < block_count) {
        const size_t chunk_end = std::min(probe_depth + chunk, block_count);
        chunk = std::min(chunk * 2, kProbeChunkMax);
        // 锁外哈希。失败时 chain_error 是 sticky 的, 留给下面统一处理。
        for (size_t i = probe_depth; i < chunk_end; ++i) {
            if (chain->At(i, &chain_error) == nullptr) {
                probe_stalled = true;
                break;
            }
        }
        std::shared_lock probe_lock(state->mutex);
        while (probe_depth < chunk_end) {
            const HashBlock* hashed = chain->At(probe_depth, &chain_error);
            if (hashed == nullptr ||
                !state->blocks.contains(hashed->projected)) {
                probe_stalled = true;
                break;
            }
            ++probe_depth;
        }
    }

    std::shared_lock state_lock(state->mutex);

    // 到这里 [0, probe_depth] 的哈希已全部算好并 memoize, 所以下面的 At()
    // 都是纯向量下标访问 —— 这段读锁内不再有哈希计算。
    auto advance_cursor = [&](size_t& cursor, const auto& present) {
        while (cursor < block_count) {
            const HashBlock* hashed = chain->At(cursor, &chain_error);
            if (hashed == nullptr) {
                cursor = block_count;  // stall every remaining walk
                return;
            }
            auto block = state->blocks.find(hashed->projected);
            if (block == state->blocks.end() || !present(block->second)) {
                break;
            }
            ++cursor;
        }
    };

    for (const auto& [instance_id, ranks] : selected_instances) {
        CacheHitResult result;

        for (int64_t rank : ranks) {
            auto gpu_present = [&](const BlockPresence& block) {
                return std::any_of(
                    block.gpu_owners.begin(), block.gpu_owners.end(),
                    [&](const EngineOwner& owner) {
                        return owner.instance_id == instance_id &&
                               owner.dp_rank == rank;
                    });
            };

            size_t cursor = 0;
            advance_cursor(cursor, gpu_present);

            RankCacheHitResult rank_match;
            rank_match.gpu = TokensForBlocks(cursor, context.block_size);

            advance_cursor(cursor, [](const BlockPresence& block) {
                return !block.cpu_owners.empty();
            });
            rank_match.cpu = TokensForBlocks(cursor, context.block_size);

            advance_cursor(cursor, [](const BlockPresence& block) {
                return !block.disk_owners.empty();
            });
            rank_match.disk = TokensForBlocks(cursor, context.block_size);

            result.dp.emplace(rank, rank_match.gpu);
            result.rank_matches.emplace(rank, rank_match);
            result.gpu = std::max(result.gpu, rank_match.gpu);
            result.cpu = std::max(result.cpu, rank_match.cpu);
            result.disk = std::max(result.disk, rank_match.disk);
        }
        result.longest_match_tokens = result.disk;
        results.emplace(instance_id, std::move(result));
    }
    if (!chain_error.empty()) {
        LOG(ERROR) << "Query hash computation failed: " << chain_error;
        return {};
    }
    return results;
}

GlobalView PrefixCacheTable::GetGlobalView() const {
    GlobalView view;
    std::vector<std::pair<ContextKey, std::shared_ptr<ContextState>>> contexts;
    {
        std::shared_lock map_lock(context_map_mutex_);
        contexts.reserve(contexts_.size());
        for (const auto& item : contexts_) {
            contexts.push_back(item);
        }
    }

    view.context_count = static_cast<int32_t>(contexts.size());
    view.contexts.reserve(contexts.size());
    for (const auto& [context, state] : contexts) {
        std::shared_lock state_lock(state->mutex);
        ContextView context_view;
        context_view.context = context;
        context_view.profile = state->profile;
        context_view.instance_ranks = state->instance_ranks;
        context_view.prefix_count = state->blocks.size();
        view.contexts.push_back(std::move(context_view));
    }
    return view;
}

}  // namespace prefixindex
}  // namespace conductor

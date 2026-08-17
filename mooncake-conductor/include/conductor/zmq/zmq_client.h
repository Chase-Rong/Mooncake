#pragma once

#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

#include "conductor/zmq/event_type.h"

namespace conductor {
namespace zmq {

class ZMQClientTestPeer;

// EventHandler processes received KV events.
class EventHandler {
   public:
    virtual ~EventHandler() = default;
    // Returns empty string on success, error message otherwise.
    virtual std::string HandleBatch(const DecodedBatch& batch,
                                    const MessageMetadata& metadata) = 0;
};

struct ZMQClientConfig {
    std::string cache_pool_key;
    std::string endpoint;
    std::string replay_endpoint;
    std::string model_name;
    common::PublisherKind publisher_kind = common::PublisherKind::kVllm;
    std::chrono::milliseconds poll_timeout{100};
    std::chrono::milliseconds replay_timeout{5000};
    std::chrono::milliseconds reconnect_delay{1000};
    // SUB 接收高水位(条)。ZMQ 默认 1000 —— 消费一慢, PUB 端(vLLM)直接丢弃,
    // 而 seq gap 检测到也不补发, 丢掉的 block 永久不进索引。
    // 8P 机群实测: 瞬时上百条消息即溢出。放大到 20 万条换约 200MB 上限,
    // 给消费侧留出吸收突发的余量。
    int rcv_hwm = 200000;
};

// Returns empty string when valid, error message otherwise.
std::string ValidateConfig(const ZMQClientConfig& config);

class ZMQClient {
   public:
    ZMQClient(ZMQClientConfig config, std::shared_ptr<EventHandler> handler);
    ~ZMQClient();
    ZMQClient(const ZMQClient&) = delete;
    ZMQClient& operator=(const ZMQClient&) = delete;

    // Establishes the SUB and DEALER sockets. Returns empty string on
    // success. Safe to call when already connected (no-op).
    std::string Connect();

    // Connects and starts the background event loop thread. Returns
    // empty string on success.
    std::string Start();

    // Stops the event loop (stop flag + join) and closes all sockets.
    // Idempotent — stop, wait for the loop to join, then clean up; safe
    // to invoke repeatedly.
    void Stop();

    int64_t GetLastSequence() const;

    // 累计丢失事件数 / gap 次数。用于判断"索引里没有"是真没缓存还是事件丢了。
    int64_t GetDroppedEvents() const { return dropped_events_.load(); }
    int64_t GetGapCount() const { return gap_count_.load(); }

   private:
    friend class ZMQClientTestPeer;

    void Loop();
    void HandleReconnect();
    bool IsConnected() const;
    void MarkDisconnected();
    // The following require holding mu_ (exclusive):
    void CleanupSocketsLocked();

    std::string Consume();
    std::string ProcessMessage();
    std::string RequestReplay(int64_t from_seq);

    ZMQClientConfig config_;
    std::shared_ptr<EventHandler> event_handler_;

    ::zmq::context_t zmq_context_{1};
    std::unique_ptr<::zmq::socket_t> sub_socket_;
    std::unique_ptr<::zmq::socket_t> replay_socket_;

    // State management.
    mutable std::shared_mutex mu_;
    bool connected_ = false;
    int64_t last_seq_ = -1;
    std::chrono::milliseconds reconnect_delay_;
    // 累计丢失的事件条数(由 seq gap 推算)。丢事件原先完全静默 ——
    // 索引里少了 block 与"缓存真的没有"在 /query 上无法区分, 必须能观测。
    std::atomic<int64_t> dropped_events_{0};
    std::atomic<int64_t> gap_count_{0};

    // Lifecycle.
    std::atomic<bool> stop_requested_{false};
    std::thread loop_thread_;
    std::mutex stop_mu_;  // serialises concurrent Stop() calls
};

}  // namespace zmq
}  // namespace conductor

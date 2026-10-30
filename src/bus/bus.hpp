#pragma once
/**
 * @file bus.hpp
 * @brief In-process publish/subscribe bus with MQTT-style topics.
 *
 * Every part of the system talks through this bus instead of calling each
 * other or polling the database:
 *
 * @verbatim
   sampler threads ──publish "samples/<provider>"──▶ bus ──▶ DB writer thread
                                                        ├──▶ alarm engine thread
                                                        ├──▶ live dashboard streams (SSE)
                                                        └──▶ MQTT bridge (optional)
   @endverbatim
 *
 * Topics are '/'-separated levels. Subscription filters follow MQTT rules:
 * '+' matches exactly one level, '#' (last level only) matches any number of
 * remaining levels, e.g. "samples/#", "alarms/+/state".
 *
 * Each subscriber owns a bounded queue that it drains on its own thread, so
 * a slow subscriber (a stalled browser, a busy disk) never blocks publishers
 * or other subscribers. When a queue is full the oldest message is dropped
 * and counted; capacities are sized so this only happens if a consumer is
 * stuck for minutes.
 *
 * Messages are immutable and shared (one allocation per publish, no copies
 * per subscriber).
 */

#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

/// @brief In-process publish/subscribe messaging (library @c sd_bus).
namespace sd::bus {

/// @brief Monotonic clock used for queue wait deadlines.
using Clock = std::chrono::steady_clock;

/// @brief One published message: a topic plus a type-erased payload.
struct Message {
    std::string topic;  ///< Full topic the message was published on, e.g. "samples/cpu_temp".
    std::any payload;   ///< Payload; read it with payload_as<T>().
};
/// @brief Shared, immutable handle to a message (one allocation per publish).
using MessagePtr = std::shared_ptr<const Message>;

/**
 * @brief Typed access to a message payload.
 * @tparam T Expected payload type.
 * @param m  Message to inspect.
 * @return Pointer to the payload, or nullptr if the message carries another type.
 */
template <typename T>
const T* payload_as(const Message& m) {
    return std::any_cast<T>(&m.payload);
}

/**
 * @brief Checks whether a string is a valid MQTT subscription filter.
 * @param filter Filter such as "samples/#" or "alarms/+/state".
 * @return True if `filter` is a valid MQTT subscription filter.
 */
bool valid_filter(std::string_view filter);
/**
 * @brief Matches a topic against a subscription filter (MQTT semantics).
 * @param filter Subscription filter ('+' = one level, '#' = remaining levels).
 * @param topic  Concrete topic name.
 * @return True if `topic` matches `filter`.
 */
bool topic_matches(std::string_view filter, std::string_view topic);

/**
 * @brief One subscriber's bounded message queue.
 *
 * Created by Bus::subscribe(). The owning thread drains it with drain() or
 * drain_until(); the bus pushes matching messages into it. When full, the
 * oldest message is dropped and counted in dropped().
 */
class Subscription {
public:
    /**
     * @brief Creates a subscription (normally called by Bus::subscribe()).
     * @param name     Human-readable name, shown in /api/stats.
     * @param filters  MQTT-style topic filters.
     * @param capacity Maximum number of queued messages.
     */
    Subscription(std::string name, std::vector<std::string> filters, size_t capacity);

    /// @brief Name given at subscription time.
    const std::string& name() const { return name_; }
    /// @brief True if `topic` matches any of this subscription's filters.
    bool matches(std::string_view topic) const;

    /**
     * @brief Waits for messages and returns everything queued, oldest first.
     *
     * Waits until at least one message is queued (or the deadline passes, or
     * the subscription is closed).
     * @param deadline Absolute time to stop waiting.
     * @return All queued messages (possibly empty).
     */
    std::vector<MessagePtr> drain_until(Clock::time_point deadline);
    /**
     * @brief Same as drain_until() with a relative timeout.
     * @param timeout Maximum time to wait.
     * @return All queued messages (possibly empty).
     */
    std::vector<MessagePtr> drain(std::chrono::milliseconds timeout) {
        return drain_until(Clock::now() + timeout);
    }

    /// @brief Wakes any waiter; later publishes are ignored.
    void close();
    /// @brief True once close() has been called.
    bool closed() const { return closed_.load(); }

    /// @brief Total messages delivered to this subscription.
    uint64_t received() const { return received_.load(); }
    /// @brief Messages discarded because the queue was full (oldest dropped).
    uint64_t dropped() const { return dropped_.load(); }
    /// @brief Messages currently waiting in the queue.
    size_t depth() const;
    /// @brief Maximum queue length.
    size_t capacity() const { return capacity_; }

private:
    friend class Bus;
    /// @brief Enqueues a message, dropping the oldest when full (called by Bus).
    void push(const MessagePtr& m);

    std::string name_;                        ///< Subscriber name.
    std::vector<std::string> filters_;        ///< Topic filters.
    size_t capacity_;                         ///< Queue capacity.
    mutable std::mutex mu_;                   ///< Guards queue_.
    std::condition_variable cv_;              ///< Signalled on push/close.
    std::deque<MessagePtr> queue_;            ///< Pending messages, oldest first.
    std::atomic<bool> closed_{false};         ///< Set by close().
    ///@{
    std::atomic<uint64_t> received_{0}, dropped_{0};  ///< Delivered / dropped message counts.
    ///@}
};

/**
 * @brief The message bus: routes published messages to matching subscriptions.
 *
 * Thread-safe. Publishing never blocks on a subscriber.
 */
class Bus {
public:
    /**
     * @brief Registers a new subscriber.
     * @param name     Subscriber name (for statistics).
     * @param filters  MQTT-style topic filters.
     * @param capacity Queue capacity (default 4096).
     * @return The new subscription; the caller drains it on its own thread.
     * @throws std::invalid_argument for an invalid filter.
     */
    std::shared_ptr<Subscription> subscribe(std::string name, std::vector<std::string> filters,
                                            size_t capacity = 4096);
    /// @brief Removes a subscription and closes it.
    void unsubscribe(const std::shared_ptr<Subscription>& sub);

    /**
     * @brief Delivers a message to every matching subscription.
     * @param topic   Topic to publish on.
     * @param payload Payload (shared, not copied per subscriber).
     * @return Number of subscriptions that received it.
     */
    size_t publish(std::string topic, std::any payload);

    /// @brief Per-subscriber statistics snapshot (for /api/stats).
    struct SubscriberStats {
        std::string name;    ///< Subscriber name.
        ///@{
        size_t depth, capacity;  ///< Messages currently queued / queue capacity.
        ///@}
        ///@{
        uint64_t received, dropped;  ///< Messages delivered / dropped on overflow.
        ///@}
    };
    /// @brief Returns statistics for every current subscriber.
    std::vector<SubscriberStats> stats() const;
    /// @brief Total number of publish() calls.
    uint64_t published() const { return published_.load(); }

private:
    mutable std::shared_mutex mu_;                      ///< Guards subs_.
    std::vector<std::shared_ptr<Subscription>> subs_;   ///< Current subscribers.
    std::atomic<uint64_t> published_{0};                ///< Publish counter.
};

}  // namespace sd::bus

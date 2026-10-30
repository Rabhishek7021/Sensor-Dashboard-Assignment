/**
 * @file bus.cpp
 * @brief Implementation of the in-process publish/subscribe bus (topic matching,
 *        bounded subscriber queues, fan-out on publish).
 */
#include "bus.hpp"

#include <algorithm>
#include <stdexcept>

namespace sd::bus {

namespace {

/// @brief Splits a topic or filter on '/' without allocating.
/// @param s Topic or filter string.
/// @return Views into the original string, one per level.
std::vector<std::string_view> levels(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find('/', start);
        if (pos == std::string_view::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

}  // namespace

bool valid_filter(std::string_view filter) {
    if (filter.empty()) return false;
    auto lv = levels(filter);
    for (size_t i = 0; i < lv.size(); ++i) {
        const auto& l = lv[i];
        if (l.find('#') != std::string_view::npos && (l != "#" || i + 1 != lv.size())) return false;
        if (l.find('+') != std::string_view::npos && l != "+") return false;
    }
    return true;
}

bool topic_matches(std::string_view filter, std::string_view topic) {
    auto f = levels(filter);
    auto t = levels(topic);
    size_t i = 0;
    for (; i < f.size(); ++i) {
        if (f[i] == "#") return true;  // matches the rest, including the parent level
        if (i >= t.size()) return false;
        if (f[i] != "+" && f[i] != t[i]) return false;
    }
    return i == t.size();
}

/// Capacity is clamped to at least 1.
Subscription::Subscription(std::string name, std::vector<std::string> filters, size_t capacity)
    : name_(std::move(name)), filters_(std::move(filters)), capacity_(std::max<size_t>(capacity, 1)) {}

bool Subscription::matches(std::string_view topic) const {
    return std::any_of(filters_.begin(), filters_.end(),
                       [&](const std::string& f) { return topic_matches(f, topic); });
}

/// Drops the oldest queued message when the queue is full; ignored after close().
void Subscription::push(const MessagePtr& m) {
    {
        std::lock_guard lock(mu_);
        if (closed_) return;
        if (queue_.size() >= capacity_) {
            queue_.pop_front();
            ++dropped_;
        }
        queue_.push_back(m);
        ++received_;
    }
    cv_.notify_one();
}

/// Moves the whole queue out under the lock, so the caller processes it without blocking publishers.
std::vector<MessagePtr> Subscription::drain_until(Clock::time_point deadline) {
    std::unique_lock lock(mu_);
    cv_.wait_until(lock, deadline, [&] { return !queue_.empty() || closed_; });
    std::vector<MessagePtr> out(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
    queue_.clear();
    return out;
}

void Subscription::close() {
    {
        std::lock_guard lock(mu_);
        closed_ = true;
    }
    cv_.notify_all();
}

size_t Subscription::depth() const {
    std::lock_guard lock(mu_);
    return queue_.size();
}

/// Validates every filter before registering, so a bad filter never leaves a half-registered subscriber.
std::shared_ptr<Subscription> Bus::subscribe(std::string name, std::vector<std::string> filters,
                                             size_t capacity) {
    for (const auto& f : filters)
        if (!valid_filter(f)) throw std::invalid_argument("invalid topic filter: '" + f + "'");
    auto sub = std::make_shared<Subscription>(std::move(name), std::move(filters), capacity);
    std::unique_lock lock(mu_);
    subs_.push_back(sub);
    return sub;
}

void Bus::unsubscribe(const std::shared_ptr<Subscription>& sub) {
    if (!sub) return;
    sub->close();
    std::unique_lock lock(mu_);
    subs_.erase(std::remove(subs_.begin(), subs_.end(), sub), subs_.end());
}

/// One shared allocation per publish; subscribers receive the same immutable message.
size_t Bus::publish(std::string topic, std::any payload) {
    auto msg = std::make_shared<const Message>(Message{std::move(topic), std::move(payload)});
    ++published_;
    size_t n = 0;
    std::shared_lock lock(mu_);
    for (const auto& s : subs_) {
        if (s->matches(msg->topic)) {
            s->push(msg);
            ++n;
        }
    }
    return n;
}

std::vector<Bus::SubscriberStats> Bus::stats() const {
    std::shared_lock lock(mu_);
    std::vector<SubscriberStats> out;
    for (const auto& s : subs_) out.push_back({s->name(), s->depth(), s->capacity(), s->received(), s->dropped()});
    return out;
}

}  // namespace sd::bus

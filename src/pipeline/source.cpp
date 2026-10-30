/**
 * @file source.cpp
 * @brief SamplerLock (flock-based single-sampler lock) and DbFollower implementation.
 */
#include "source.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "../common/log.hpp"
#include "../common/time_util.hpp"
#include "messages.hpp"

namespace sd::pipeline {

namespace {
/// @brief Path of the lock file next to the database.
std::string lock_path(const std::string& db_path) { return db_path + ".sampler.lock"; }
}  // namespace

std::unique_ptr<SamplerLock> SamplerLock::try_acquire(const std::string& db_path) {
    const int fd = ::open(lock_path(db_path).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return nullptr;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return nullptr;
    }
    const std::string pid = std::to_string(::getpid()) + "\n";
    if (::ftruncate(fd, 0) == 0) {
        ssize_t w = ::pwrite(fd, pid.data(), pid.size(), 0);
        (void)w;
    }
    return std::unique_ptr<SamplerLock>(new SamplerLock(fd));
}

std::optional<long> SamplerLock::holder(const std::string& db_path) {
    const int fd = ::open(lock_path(db_path).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    char buf[32] = {};
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return std::nullopt;
    const long pid = std::strtol(buf, nullptr, 10);
    return pid > 0 ? std::optional<long>(pid) : std::nullopt;
}

SamplerLock::~SamplerLock() {
    if (fd_ >= 0) ::close(fd_);  // closing releases the flock
}

DbFollower::DbFollower(db::Database& db, bus::Bus& bus, std::chrono::milliseconds poll)
    : db_(db), bus_(bus), poll_(poll) {}

DbFollower::~DbFollower() { stop(); }

void DbFollower::start() {
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void DbFollower::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void DbFollower::run() {
    std::map<int64_t, int64_t> last;  // sensor id -> newest ts already published
    std::vector<int64_t> ids;
    int64_t next_sensor_check = 0;
    const int64_t start = time::now_ms() - 2000;  // do not replay history
    while (!stop_) {
        try {
            const int64_t now = time::now_ms();
            if (now >= next_sensor_check) {
                auto fresh = db_.sensor_ids();
                if (fresh != ids) {
                    const bool first = ids.empty();
                    ids = std::move(fresh);
                    if (!first) bus_.publish(msg::topic::kSensorsChanged, msg::SensorsChanged{ids.size()});
                }
                next_sensor_check = now + 5000;
            }
            // Group the new rows by timestamp: one batch per tick, like the sampler.
            std::map<int64_t, msg::SampleBatch> ticks;
            for (auto id : ids) {
                auto it = last.find(id);
                const int64_t after = it == last.end() ? start : it->second;
                for (const auto& r : db_.readings_after(id, after, 10000)) {
                    auto& b = ticks[r.ts];
                    b.ts = r.ts;
                    b.source = "db";
                    b.samples.push_back({id, r.value});
                    last[id] = r.ts;
                }
            }
            for (auto& [ts, b] : ticks) {
                published_ += b.samples.size();
                bus_.publish(msg::topic::samples("db"), std::move(b));
            }
        } catch (const std::exception& e) {
            log::warn("db follower: ", e.what());
        }
        const auto until = std::chrono::steady_clock::now() + poll_;
        while (!stop_ && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

}  // namespace sd::pipeline

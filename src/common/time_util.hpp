#pragma once
/**
 * @file time_util.hpp
 * @brief Time helpers.
 *
 * All timestamps in the system are Unix epoch milliseconds, UTC.
 */

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

/// @brief Time helpers (epoch milliseconds, UTC).
namespace sd::time {

/// @brief Current time in epoch milliseconds (UTC).
inline int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// @brief Formats epoch ms as "YYYY-MM-DDTHH:MM:SS.mmmZ".
inline std::string to_iso8601(int64_t ms) {
    std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03dZ", buf, static_cast<int>(ms % 1000));
    return out;
}

}  // namespace sd::time

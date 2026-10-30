#pragma once
/**
 * @file log.hpp
 * @brief Minimal thread-safe logger writing to stderr with a UTC timestamp.
 */

#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include "time_util.hpp"

/// @brief Logging to stderr.
namespace sd::log {

/// @brief Message severity.
enum class Level { Debug, Info, Warn, Error };

/// @brief Minimum level that is written (default Info; --verbose lowers it to Debug).
inline Level& threshold() {
    static Level lvl = Level::Info;
    return lvl;
}

/// @brief Writes one line "<timestamp> [LEVEL] msg" if `lvl` passes the threshold.
inline void write(Level lvl, const std::string& msg) {
    if (lvl < threshold()) return;
    static std::mutex mu;
    static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    std::lock_guard<std::mutex> lock(mu);
    std::cerr << sd::time::to_iso8601(sd::time::now_ms()) << " ["
              << names[static_cast<int>(lvl)] << "] " << msg << '\n';
}

template <typename... Args>
/// @brief Streams all arguments into one string.
std::string concat(Args&&... args) {
    std::ostringstream os;
    (os << ... << std::forward<Args>(args));
    return os.str();
}

/// @brief Logs a Debug message; arguments are concatenated with operator<<.
template <typename... Args> void debug(Args&&... a) { write(Level::Debug, concat(std::forward<Args>(a)...)); }
/// @brief Logs an Info message.
template <typename... Args> void info(Args&&... a)  { write(Level::Info,  concat(std::forward<Args>(a)...)); }
/// @brief Logs a Warn message.
template <typename... Args> void warn(Args&&... a)  { write(Level::Warn,  concat(std::forward<Args>(a)...)); }
/// @brief Logs an Error message.
template <typename... Args> void error(Args&&... a) { write(Level::Error, concat(std::forward<Args>(a)...)); }

}  // namespace sd::log

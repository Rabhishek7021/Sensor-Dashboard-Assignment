/**
 * @file sysfs.cpp
 * @brief Implementation of the sysfs file helpers and SysfsValueSensor::read().
 */
#include "sysfs.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include "sensor.hpp"

namespace fs = std::filesystem;

namespace sd::sysfs {

/// @brief First line of a file with trailing whitespace removed; nullopt if unreadable.
std::optional<std::string> read_line(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return std::nullopt;
    std::string s;
    std::getline(in, s);
    if (in.bad()) return std::nullopt;
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r' || s.back() == '\t'))
        s.pop_back();
    return s;
}

/// Hot path: called for every sensor on every tick (2x per second by
/// default). Plain open/read/close into a stack buffer avoids the stream,
/// locale and heap work of std::ifstream.
std::optional<long long> read_int(const fs::path& p) {
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    char buf[64];
    ssize_t n;
    do {
        n = ::read(fd, buf, sizeof(buf) - 1);
    } while (n < 0 && errno == EINTR);
    ::close(fd);
    if (n <= 0) return std::nullopt;
    buf[n] = '\0';
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(buf, &end, 10);
    if (errno != 0 || end == buf) return std::nullopt;
    return v;
}

/// @brief Entries of a directory, sorted by name; empty if missing or unreadable.
std::vector<fs::path> list_dir(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.push_back(it->path());
    std::sort(out.begin(), out.end());
    return out;
}

int trailing_number(const std::string& s, int fallback) {
    static const std::regex re(R"((\d+)\s*$)");
    std::smatch m;
    if (std::regex_search(s, m, re)) return std::atoi(m[1].str().c_str());
    return fallback;
}

}  // namespace sd::sysfs

namespace sd::sensors {

std::optional<double> SysfsValueSensor::read() const {
    auto v = sysfs::read_int(path_);
    if (!v) return std::nullopt;
    return static_cast<double>(*v) * scale_;
}

}  // namespace sd::sensors

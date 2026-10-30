#pragma once
/**
 * @file sysfs.hpp
 * @brief Small, exception-free helpers for reading Linux sysfs files.
 *
 * Shared by all providers so each one only contains its own discovery rules.
 */

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/// @brief Exception-free helpers for reading Linux sysfs files.
namespace sd::sysfs {

/// First line of a file with trailing whitespace removed; nullopt if unreadable.
std::optional<std::string> read_line(const std::filesystem::path& p);

/// Integer content of a file; nullopt if unreadable or not a number.
std::optional<long long> read_int(const std::filesystem::path& p);

/// Entries of a directory, sorted by name (deterministic discovery order).
/// Returns an empty list if the directory does not exist or is unreadable.
std::vector<std::filesystem::path> list_dir(const std::filesystem::path& dir);

/// Trailing number of a string ("Core 12" -> 12), or `fallback`.
int trailing_number(const std::string& s, int fallback);

}  // namespace sd::sysfs

#pragma once

#include <cstddef>
#include <string>

namespace tt::app::tagquest {

constexpr size_t USERNAME_MAX_LENGTH = 10;

struct Settings {
    std::string badgeId;
    std::string username;
};

/** Loads the stored settings, deriving and persisting a badge id when there is none yet. */
Settings loadOrCreate();

bool save(const Settings& settings);

/**
 * Whether a username is usable as-is.
 * Restricted to ASCII alphanumerics so it can go into a query string without an encoder.
 */
bool isValidUsername(const std::string& username);

} // namespace tt::app::tagquest

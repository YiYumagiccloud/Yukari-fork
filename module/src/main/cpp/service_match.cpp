#include "service_match.h"

#include <cctype>
#include <string>

namespace {
constexpr const char *kKeywords[] = {
    "lineage",
    "crdroid",
    "aospa",
    "pixelexperience",
    "omnirom",
    "protonaosp",
};

constexpr const char *kExactServices[] = {
    "profile",
};

std::string to_lower(const std::string &input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}
} // namespace

bool hide_service(const std::string &service_name) {
    const std::string lower = to_lower(service_name);

    for (const char *service : kExactServices) {
        if (lower == service) return true;
    }

    for (const char *keyword : kKeywords) {
        if (lower.find(keyword) != std::string::npos) return true;
    }
    return false;
}

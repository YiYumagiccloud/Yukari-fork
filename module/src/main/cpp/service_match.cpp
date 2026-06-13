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

std::string to_lower_ascii(const std::string &input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}
} // namespace

bool should_hide_service(const std::string &service_name) {
    const std::string lower = to_lower_ascii(service_name);
    for (const char *keyword : kKeywords) {
        if (lower.find(keyword) != std::string::npos) return true;
    }
    return false;
}

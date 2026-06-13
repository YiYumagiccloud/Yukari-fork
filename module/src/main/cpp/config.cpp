#include "config.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {
constexpr const char *kConfigPath = "/data/adb/modules/Yukari/config.json";

std::string read_file(const char *path) {
    FILE *fp = std::fopen(path, "rb");
    if (!fp) return {};
    std::string out;
    char buffer[4096];
    while (true) {
        size_t n = std::fread(buffer, 1, sizeof(buffer), fp);
        if (n > 0) out.append(buffer, n);
        if (n < sizeof(buffer)) break;
    }
    std::fclose(fp);
    return out;
}

std::vector<std::string> parse_targets_lenient(const std::string &text) {
    std::vector<std::string> targets;
    const auto key = text.find("\"targets\"");
    if (key == std::string::npos) return targets;
    const auto begin = text.find('[', key);
    const auto end = text.find(']', begin);
    if (begin == std::string::npos || end == std::string::npos || end <= begin) return targets;

    size_t pos = begin;
    while ((pos = text.find('"', pos + 1)) != std::string::npos && pos < end) {
        const auto close = text.find('"', pos + 1);
        if (close == std::string::npos || close > end) break;
        auto value = text.substr(pos + 1, close - pos - 1);
        if (!value.empty()) targets.push_back(value);
        pos = close;
    }
    return targets;
}
} // namespace

bool load_config(YukariConfig &out) {
    const std::string text = read_file(kConfigPath);
    if (text.empty()) {
        out = {};
        return false;
    }
    const auto enabled_key = text.find("\"enabled\"");
    const auto false_value = enabled_key == std::string::npos
        ? std::string::npos
        : text.find("false", enabled_key);
    const auto targets_key = text.find("\"targets\"");
    out.enabled = false_value == std::string::npos ||
                  (targets_key != std::string::npos && false_value > targets_key);
    out.targets = parse_targets_lenient(text);
    return true;
}

bool is_target_package(const YukariConfig &config, const std::string &package_name) {
    if (!config.enabled || package_name.empty()) return false;
    static const std::vector<std::string> protected_packages = {
        "android",
        "system",
        "system_server",
        "com.android.systemui",
        "com.android.settings",
    };
    if (std::find(protected_packages.begin(), protected_packages.end(), package_name) != protected_packages.end()) {
        return false;
    }
    return std::find(config.targets.begin(), config.targets.end(), package_name) != config.targets.end();
}

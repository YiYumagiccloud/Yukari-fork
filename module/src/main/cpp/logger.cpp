#include "logger.h"

#include <android/log.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <unistd.h>

namespace {
constexpr const char *kTag = "Yukari";
constexpr const char *kLogPath = "/data/adb/modules/Yukari/logs/yukari.log";

void write_file_log(const char *level, const char *message) {
#if YUKARI_DEBUG_LOG
    FILE *fp = std::fopen(kLogPath, "a");
    if (!fp) return;

    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);

    char time_buf[32]{};
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    std::fprintf(fp, "%s [%s] pid=%d %s\n", time_buf, level, getpid(), message);
    std::fclose(fp);
#else
    (void)level;
    (void)message;
#endif
}

void vlog(int prio, const char *level, const char *fmt, va_list ap) {
    char buffer[1024]{};
    std::vsnprintf(buffer, sizeof(buffer), fmt, ap);
    __android_log_print(prio, kTag, "%s", buffer);
    write_file_log(level, buffer);
}
} // namespace

void yukari_log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(ANDROID_LOG_INFO, "I", fmt, ap);
    va_end(ap);
}

void yukari_log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(ANDROID_LOG_ERROR, "E", fmt, ap);
    va_end(ap);
}

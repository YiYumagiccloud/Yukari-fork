#include "binder_hook.h"

#include <android/log.h>

#define LOG_TAG "Yukari"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

void install_binder_hooks() {
    // TODO: Install ioctl / libbinder_ndk hooks and filter service-manager replies.
    // The service predicate is fixed in service_match.cpp.
    LOGI("binder hook skeleton installed");
}

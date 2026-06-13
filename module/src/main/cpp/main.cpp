#include "binder_hook.h"
#include "config.h"
#include "service_cache.h"

#include <android/log.h>
#include <jni.h>
#include <string>

#define LOG_TAG "Yukari"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// This file is a Zygisk entry skeleton. Wire it to the upstream zygisk.hpp API
// when adding the final module bootstrap.
namespace {
YukariConfig g_config;
std::string g_package_name;
bool g_enabled_for_process = false;
}

extern "C" JNIEXPORT void JNICALL
Java_com_yukari_module_stub_YukariNative_nativeInit(JNIEnv *env, jclass, jstring package_name) {
    load_config(g_config);
    const char *raw = env->GetStringUTFChars(package_name, nullptr);
    if (raw) {
        g_package_name = raw;
        env->ReleaseStringUTFChars(package_name, raw);
    }
    g_enabled_for_process = is_target_package(g_config, g_package_name);
    if (!g_enabled_for_process) return;

    clear_service_manager_cache(env);
    install_binder_hooks();
    LOGI("enabled for %s", g_package_name.c_str());
}

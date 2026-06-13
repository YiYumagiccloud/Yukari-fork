#pragma once

#include <jni.h>

namespace zygisk {

enum Option : int {
    FORCE_DENYLIST_UNMOUNT = 0,
    DLCLOSE_MODULE_LIBRARY = 1,
};

struct AppSpecializeArgs {
    jint *uid = nullptr;
    jint *gid = nullptr;
    jintArray *gids = nullptr;
    jint *runtime_flags = nullptr;
    jobjectArray *rlimits = nullptr;
    jint *mount_external = nullptr;
    jstring *se_info = nullptr;
    jstring *nice_name = nullptr;
    jstring *instruction_set = nullptr;
    jstring *app_data_dir = nullptr;
};

class Api {
public:
    void setOption(Option) {}
};

class ModuleBase {
public:
    virtual ~ModuleBase() = default;
    virtual void onLoad(Api *api, JNIEnv *env) {}
    virtual void preAppSpecialize(AppSpecializeArgs *args) {}
    virtual void postAppSpecialize(const AppSpecializeArgs *args) {}
};

} // namespace zygisk

#define REGISTER_ZYGISK_MODULE(clazz) \
extern "C" __attribute__((visibility("default"))) zygisk::ModuleBase *zygisk_module_create() { \
    return new clazz(); \
}

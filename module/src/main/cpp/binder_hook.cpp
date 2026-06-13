#include "binder_hook.h"
#include "service_match.h"

#include <android/log.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <linux/android/binder.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_TAG "Yukari"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef BC_TRANSACTION_SG
#define BC_TRANSACTION_SG _IOW('c', 17, struct binder_transaction_data_sg)
#endif

namespace {
using IoctlFn = int (*)(int, unsigned long, void *);
IoctlFn g_original_ioctl = nullptr;
bool g_hook_installed = false;

constexpr size_t kPatchSize = 16;
uint8_t g_original_bytes[kPatchSize]{};
void *g_ioctl_symbol = nullptr;

struct binder_transaction_data_sg_local {
    binder_transaction_data transaction_data;
    binder_size_t buffers_size;
};

bool make_writable(void *addr) {
    const long page_size = sysconf(_SC_PAGESIZE);
    const uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~(static_cast<uintptr_t>(page_size) - 1);
    return mprotect(reinterpret_cast<void *>(page), static_cast<size_t>(page_size), PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

void write_abs_jump(void *target, void *replacement) {
#if defined(__aarch64__)
    // ldr x17, #8; br x17; .quad replacement
    uint32_t patch[4] = {
        0x58000051u,
        0xd61f0220u,
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement) & 0xffffffffu),
        static_cast<uint32_t>((reinterpret_cast<uintptr_t>(replacement) >> 32u) & 0xffffffffu),
    };
    std::memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache(reinterpret_cast<char *>(target), reinterpret_cast<char *>(target) + sizeof(patch));
#else
    (void)target;
    (void)replacement;
#endif
}

void *create_trampoline(void *target) {
#if defined(__aarch64__)
    void *memory = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return nullptr;
    std::memcpy(memory, target, kPatchSize);
    write_abs_jump(reinterpret_cast<uint8_t *>(memory) + kPatchSize, reinterpret_cast<uint8_t *>(target) + kPatchSize);
    __builtin___clear_cache(reinterpret_cast<char *>(memory), reinterpret_cast<char *>(memory) + kPatchSize * 2);
    return memory;
#else
    (void)target;
    return nullptr;
#endif
}

bool contains_keyword_utf16(const char16_t *chars, int32_t len) {
    if (!chars || len <= 0 || len > 512) return false;
    std::string ascii;
    ascii.reserve(static_cast<size_t>(len));
    for (int32_t i = 0; i < len; ++i) {
        const char16_t c = chars[i];
        ascii.push_back(c <= 0x7f ? static_cast<char>(c) : '?');
    }
    return should_hide_service(ascii);
}

void overwrite_utf16(char16_t *chars, int32_t len) {
    if (!chars || len <= 0) return;
    for (int32_t i = 0; i < len; ++i) chars[i] = u'_';
}

void scrub_service_strings(uint8_t *parcel, size_t size) {
    if (!parcel || size < sizeof(int32_t)) return;
    for (size_t off = 0; off + sizeof(int32_t) < size; ++off) {
        int32_t len = 0;
        std::memcpy(&len, parcel + off, sizeof(len));
        if (len <= 0 || len > 512) continue;
        const size_t str_off = off + sizeof(int32_t);
        const size_t bytes = static_cast<size_t>(len) * sizeof(char16_t);
        if (str_off + bytes > size) continue;
        auto *chars = reinterpret_cast<char16_t *>(parcel + str_off);
        if (contains_keyword_utf16(chars, len)) {
            overwrite_utf16(chars, len);
            LOGI("scrubbed service query string");
        }
    }
}

void process_transaction(const binder_transaction_data &txn) {
    if (txn.data_size == 0 || txn.data.ptr.buffer == 0) return;
    // Only scrub traffic aimed at the context manager. This is conservative and
    // avoids touching arbitrary app Binder payloads.
    if (txn.target.handle != 0) return;
    auto *parcel = reinterpret_cast<uint8_t *>(txn.data.ptr.buffer);
    scrub_service_strings(parcel, static_cast<size_t>(txn.data_size));
}

void process_binder_write_buffer(binder_write_read *bwr) {
    if (!bwr || !bwr->write_buffer || !bwr->write_size) return;
    auto *ptr = reinterpret_cast<uint8_t *>(bwr->write_buffer);
    auto *end = ptr + bwr->write_size;

    while (ptr + sizeof(uint32_t) <= end) {
        uint32_t cmd = 0;
        std::memcpy(&cmd, ptr, sizeof(cmd));
        ptr += sizeof(cmd);

        if (cmd == BC_TRANSACTION || cmd == BC_REPLY) {
            if (ptr + sizeof(binder_transaction_data) > end) return;
            auto *txn = reinterpret_cast<binder_transaction_data *>(ptr);
            if (cmd == BC_TRANSACTION) process_transaction(*txn);
            ptr += sizeof(binder_transaction_data);
        } else if (cmd == BC_TRANSACTION_SG) {
            if (ptr + sizeof(binder_transaction_data_sg_local) > end) return;
            auto *txn = reinterpret_cast<binder_transaction_data_sg_local *>(ptr);
            process_transaction(txn->transaction_data);
            ptr += sizeof(binder_transaction_data_sg_local);
        } else {
            // Unknown command sizes are version dependent. Stop rather than desync.
            return;
        }
    }
}

int hooked_ioctl(int fd, unsigned long request, void *arg) {
    if (request == BINDER_WRITE_READ && arg) {
        process_binder_write_buffer(reinterpret_cast<binder_write_read *>(arg));
    }
    return g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
}

bool install_inline_hook(void *symbol, void *replacement, void **original) {
#if defined(__aarch64__)
    if (!symbol || !replacement || !original) return false;
    void *trampoline = create_trampoline(symbol);
    if (!trampoline) return false;
    std::memcpy(g_original_bytes, symbol, kPatchSize);
    if (!make_writable(symbol)) return false;
    write_abs_jump(symbol, replacement);
    *original = trampoline;
    g_ioctl_symbol = symbol;
    return true;
#else
    (void)symbol;
    (void)replacement;
    (void)original;
    return false;
#endif
}
} // namespace

void install_binder_hooks() {
    if (g_hook_installed) return;
    void *handle = dlopen("libc.so", RTLD_NOW);
    if (!handle) {
        LOGE("dlopen libc.so failed: %s", dlerror());
        return;
    }
    void *symbol = dlsym(handle, "ioctl");
    if (!symbol) {
        LOGE("dlsym ioctl failed: %s", dlerror());
        return;
    }
    if (!install_inline_hook(symbol, reinterpret_cast<void *>(hooked_ioctl), reinterpret_cast<void **>(&g_original_ioctl))) {
        LOGE("inline ioctl hook failed errno=%d", errno);
        return;
    }
    g_hook_installed = true;
    LOGI("ioctl hook installed at %p", g_ioctl_symbol);
}

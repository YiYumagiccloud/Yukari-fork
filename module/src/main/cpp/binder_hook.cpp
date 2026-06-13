#include "binder_hook.h"
#include "logger.h"
#include "service_match.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <linux/android/binder.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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
thread_local bool g_waiting_for_service_manager_reply = false;

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

std::string utf16_to_ascii(const char16_t *chars, int32_t len) {
    std::string ascii;
    if (!chars || len <= 0 || len > 512) return ascii;
    ascii.reserve(static_cast<size_t>(len));
    for (int32_t i = 0; i < len; ++i) {
        const char16_t c = chars[i];
        ascii.push_back(c <= 0x7f ? static_cast<char>(c) : '?');
    }
    return ascii;
}

void overwrite_utf16(char16_t *chars, int32_t len) {
    if (!chars || len <= 0) return;
    for (int32_t i = 0; i < len; ++i) chars[i] = u'_';
}

int scrub_service_strings(uint8_t *parcel, size_t size, const char *source) {
    if (!parcel || size < sizeof(int32_t)) return 0;
    int hits = 0;
    for (size_t off = 0; off + sizeof(int32_t) < size; ++off) {
        int32_t len = 0;
        std::memcpy(&len, parcel + off, sizeof(len));
        if (len <= 0 || len > 512) continue;
        const size_t str_off = off + sizeof(int32_t);
        const size_t bytes = static_cast<size_t>(len) * sizeof(char16_t);
        if (str_off + bytes > size) continue;

        auto *chars = reinterpret_cast<char16_t *>(parcel + str_off);
        const std::string value = utf16_to_ascii(chars, len);
        if (should_hide_service(value)) {
            overwrite_utf16(chars, len);
            ++hits;
            yukari_log_info("scrubbed %s service string: %s", source, value.c_str());
        }
    }
    return hits;
}

void process_service_manager_transaction(const binder_transaction_data &txn) {
    if (txn.data_size == 0 || txn.data.ptr.buffer == 0) return;
    if (txn.target.handle != 0) return;
    g_waiting_for_service_manager_reply = true;
    auto *parcel = reinterpret_cast<uint8_t *>(txn.data.ptr.buffer);
    scrub_service_strings(parcel, static_cast<size_t>(txn.data_size), "request");
}

void process_reply_transaction(const binder_transaction_data &txn) {
    if (!g_waiting_for_service_manager_reply) return;
    g_waiting_for_service_manager_reply = false;
    if (txn.data_size == 0 || txn.data.ptr.buffer == 0) return;
    auto *parcel = reinterpret_cast<uint8_t *>(txn.data.ptr.buffer);
    const int hits = scrub_service_strings(parcel, static_cast<size_t>(txn.data_size), "reply");
    if (hits > 0) yukari_log_info("filtered %d service-manager reply item(s)", hits);
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
            if (cmd == BC_TRANSACTION) process_service_manager_transaction(*txn);
            ptr += sizeof(binder_transaction_data);
        } else if (cmd == BC_TRANSACTION_SG) {
            if (ptr + sizeof(binder_transaction_data_sg_local) > end) return;
            auto *txn = reinterpret_cast<binder_transaction_data_sg_local *>(ptr);
            process_service_manager_transaction(txn->transaction_data);
            ptr += sizeof(binder_transaction_data_sg_local);
        } else {
            return;
        }
    }
}

void process_binder_read_buffer(binder_write_read *bwr) {
    if (!bwr || !bwr->read_buffer || !bwr->read_consumed) return;
    auto *ptr = reinterpret_cast<uint8_t *>(bwr->read_buffer);
    auto *end = ptr + bwr->read_consumed;

    while (ptr + sizeof(uint32_t) <= end) {
        uint32_t cmd = 0;
        std::memcpy(&cmd, ptr, sizeof(cmd));
        ptr += sizeof(cmd);

        switch (cmd) {
            case BR_REPLY: {
                if (ptr + sizeof(binder_transaction_data) > end) return;
                auto *txn = reinterpret_cast<binder_transaction_data *>(ptr);
                process_reply_transaction(*txn);
                ptr += sizeof(binder_transaction_data);
                break;
            }
            case BR_TRANSACTION: {
                if (ptr + sizeof(binder_transaction_data) > end) return;
                ptr += sizeof(binder_transaction_data);
                break;
            }
            case BR_NOOP:
            case BR_TRANSACTION_COMPLETE:
            case BR_DEAD_REPLY:
            case BR_FAILED_REPLY:
            case BR_FINISHED:
                break;
            case BR_DEAD_BINDER:
            case BR_CLEAR_DEATH_NOTIFICATION_DONE:
                if (ptr + sizeof(binder_uintptr_t) > end) return;
                ptr += sizeof(binder_uintptr_t);
                break;
            default:
                return;
        }
    }
}

int hooked_ioctl(int fd, unsigned long request, void *arg) {
    if (request != BINDER_WRITE_READ || !arg) {
        return g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    }

    auto *bwr = reinterpret_cast<binder_write_read *>(arg);
    process_binder_write_buffer(bwr);
    const int ret = g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    if (ret == 0) process_binder_read_buffer(bwr);
    return ret;
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
        yukari_log_error("dlopen libc.so failed: %s", dlerror());
        return;
    }
    void *symbol = dlsym(handle, "ioctl");
    if (!symbol) {
        yukari_log_error("dlsym ioctl failed: %s", dlerror());
        return;
    }
    if (!install_inline_hook(symbol, reinterpret_cast<void *>(hooked_ioctl), reinterpret_cast<void **>(&g_original_ioctl))) {
        yukari_log_error("inline ioctl hook failed errno=%d", errno);
        return;
    }
    g_hook_installed = true;
    yukari_log_info("ioctl hook installed at %p", g_ioctl_symbol);
}

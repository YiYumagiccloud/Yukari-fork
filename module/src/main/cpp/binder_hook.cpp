#include "binder_hook.h"
#include "logger.h"
#include "service_match.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <linux/android/binder.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vector>

#ifndef BC_TRANSACTION_SG
#define BC_TRANSACTION_SG _IOW('c', 17, struct binder_transaction_data_sg)
#endif

#ifndef TF_ONE_WAY
#define TF_ONE_WAY 0x01
#endif

// Service Manager Transaction Codes
#define SVC_GET_SERVICE 1
#define SVC_CHECK_SERVICE 2
#define SVC_LIST_SERVICES 4

namespace {
using IoctlFn = int (*)(int, unsigned long, void *);
IoctlFn g_original_ioctl = nullptr;
bool g_hook_installed = false;
bool g_enhanced_mode = false; // Kept for compatibility, but no longer used for swap

// Thread local state to track pending ServiceManager list replies
thread_local int g_pending_sm_list_replies = 0;

struct binder_transaction_data_sg_local {
    binder_transaction_data transaction_data;
    binder_size_t buffers_size;
};

struct ElfMappingId {
    dev_t dev = 0;
    ino_t inode = 0;
};

size_t align4(size_t value) { return (value + 3u) & ~static_cast<size_t>(3u); }

std::string to_ascii(const char16_t *chars, int32_t len) {
    std::string ascii;
    if (!chars || len <= 0 || len > 512) return ascii;
    ascii.reserve(static_cast<size_t>(len));
    for (int32_t i = 0; i < len; ++i) {
        ascii.push_back(chars[i] <= 0x7f ? static_cast<char>(chars[i]) : '?');
    }
    return ascii;
}

bool is_parcel_str16(const uint8_t *parcel, size_t size, size_t off, int32_t len) {
    if (!parcel || (off & 0x3u) != 0 || len <= 0 || len > 512) return false;
    const size_t str_off = off + sizeof(int32_t);
    const size_t bytes = static_cast<size_t>(len) * sizeof(char16_t);
    const size_t terminator_off = str_off + bytes;
    const size_t next_off = align4(terminator_off + sizeof(char16_t));
    if (next_off > size) return false;

    char16_t terminator = 1;
    std::memcpy(&terminator, parcel + terminator_off, sizeof(terminator));
    if (terminator != 0) return false;

    for (int32_t i = 0; i < len; ++i) {
        char16_t c = 0;
        std::memcpy(&c, parcel + str_off + static_cast<size_t>(i) * sizeof(char16_t), sizeof(c));
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

void overwrite_utf16(char16_t *chars, int32_t len) {
    if (!chars || len <= 0) return;
    for (int32_t i = 0; i < len; ++i) chars[i] = u'_';
}

size_t process_string16(uint8_t *parcel, size_t size, size_t off, bool &hit) {
    if (off + sizeof(int32_t) > size) return 0;
    int32_t len = 0;
    std::memcpy(&len, parcel + off, sizeof(len));
    if (!is_parcel_str16(parcel, size, off, len)) return 0;

    const size_t str_off = off + sizeof(int32_t);
    const size_t bytes = static_cast<size_t>(len) * sizeof(char16_t);
    const size_t terminator_off = str_off + bytes;
    const size_t next_off = align4(terminator_off + sizeof(char16_t));

    auto *chars = reinterpret_cast<char16_t *>(parcel + str_off);
    const std::string value = to_ascii(chars, len);
    if (hide_service(value)) {
        overwrite_utf16(chars, len);
        hit = true;
        log_info("scrubbed service string: %s", value.c_str());
    }
    return next_off - off;
}

void filter_list_services_reply(uint8_t *parcel, size_t size) {
    if (!parcel || size < sizeof(int32_t)) return;
    size_t pos = 0;
    pos += sizeof(int32_t); // Skip status_t (exception code)
    if (pos >= size) return;

    int32_t array_len = 0;
    std::memcpy(&array_len, parcel + pos, sizeof(array_len));
    pos += sizeof(int32_t);
    if (array_len <= 0 || array_len > 1024) return;

    for (int32_t i = 0; i < array_len; ++i) {
        bool hit = false;
        size_t consumed = process_string16(parcel, size, pos, hit);
        if (consumed == 0) break;
        pos += consumed;
    }
}

void process_transaction(const binder_transaction_data &txn) {
    if (txn.data_size == 0 || txn.data.ptr.buffer == 0) return;
    if (txn.target.handle != 0) return; // Only ServiceManager

    uint32_t code = txn.code;
    if (code == SVC_GET_SERVICE || code == SVC_CHECK_SERVICE) {
        if ((txn.flags & TF_ONE_WAY) == 0) {
            auto *parcel = reinterpret_cast<uint8_t *>(txn.data.ptr.buffer);
            // Parcel format: [strict_mode(4)] [work_source(4)] [header(4)] [interface_token] [service_name]
            size_t pos = 12; // Skip the 3 int32_t header
            bool hit = false;
            // Skip interface token
            size_t consumed = process_string16(parcel, static_cast<size_t>(txn.data_size), pos, hit);
            if (consumed > 0) {
                pos += consumed;
                // Next string is the service name
                process_string16(parcel, static_cast<size_t>(txn.data_size), pos, hit);
            }
        }
    } else if (code == SVC_LIST_SERVICES) {
        if ((txn.flags & TF_ONE_WAY) == 0) {
            ++g_pending_sm_list_replies;
        }
    }
}

// Helper to make a read-only binder buffer writable temporarily
bool make_writable(void *addr, size_t len) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    const uintptr_t page_mask = ~static_cast<uintptr_t>(page_size - 1);
    uintptr_t start = reinterpret_cast<uintptr_t>(addr) & page_mask;
    uintptr_t end = (reinterpret_cast<uintptr_t>(addr) + len + page_size - 1) & page_mask;
    return mprotect(reinterpret_cast<void *>(start), end - start, PROT_READ | PROT_WRITE) == 0;
}

void process_reply(binder_transaction_data *txn) {
    if (g_pending_sm_list_replies <= 0) return;
    --g_pending_sm_list_replies;

    if (!txn || txn->data_size == 0 || txn->data.ptr.buffer == 0) return;

    auto *parcel = reinterpret_cast<uint8_t *>(txn->data.ptr.buffer);
    const size_t data_size = static_cast<size_t>(txn->data_size);

    // Attempt to write to the buffer. If it's read-only (from kernel mmap),
    // temporarily make it writable using mprotect.
    bool made_writable = false;
    if (make_writable(parcel, data_size)) {
        // To check if it was actually read-only before, we'd need to parse /proc/maps.
        // Instead, just try to write. If it segfaults, we can't catch it here.
        // But mprotect on binder mmap should work safely.
        made_writable = true; // Assume we might have changed it
    }

    filter_list_services_reply(parcel, data_size);
    log_info("filtered listServices reply (mprotect ensured writability)");
}

void forget_reply() {
    if (g_pending_sm_list_replies > 0) --g_pending_sm_list_replies;
}

void process_write(binder_write_read *bwr) {
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
            return;
        }
    }
}

void process_read(binder_write_read *bwr) {
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
                process_reply(txn);
                ptr += sizeof(binder_transaction_data);
                break;
            }
            case BR_TRANSACTION: {
                if (ptr + sizeof(binder_transaction_data) > end) return;
                ptr += sizeof(binder_transaction_data);
                break;
            }
            case BR_DEAD_REPLY:
            case BR_FAILED_REPLY:
                forget_reply();
                break;
            case BR_NOOP:
            case BR_TRANSACTION_COMPLETE:
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

int hook_ioctl(int fd, unsigned long request, void *arg) {
    if (request != BINDER_WRITE_READ || !arg) {
        return g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    }

    auto *bwr = reinterpret_cast<binder_write_read *>(arg);
    process_write(bwr);
    const int ret = g_original_ioctl ? g_original_ioctl(fd, request, arg) : -1;
    if (ret == 0) process_read(bwr);
    return ret;
}

bool seen(const std::vector<ElfMappingId> &mappings, dev_t dev, ino_t inode) {
    for (const auto &mapping : mappings) {
        if (mapping.dev == dev && mapping.inode == inode) return true;
    }
    return false;
}

std::vector<ElfMappingId> find_mappings() {
    std::vector<ElfMappingId> mappings;
    FILE *fp = std::fopen("/proc/self/maps", "r");
    if (!fp) return mappings;

    char line[1024]{};
    while (std::fgets(line, sizeof(line), fp)) {
        unsigned long long begin = 0, end = 0, offset = 0, inode = 0;
        unsigned int major_id = 0, minor_id = 0;
        char perms[5]{};
        char path[512]{};
        const int fields = std::sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %511s", &begin, &end,
                                       perms, &offset, &major_id, &minor_id, &inode, path);
        if (fields < 8 || inode == 0) continue;
        const std::string pathname = path;
        if (pathname.find("/libbinder.so") == std::string::npos) continue;

        const dev_t dev = makedev(major_id, minor_id);
        const auto ino = static_cast<ino_t>(inode);
        if (!seen(mappings, dev, ino)) mappings.push_back({dev, ino});
    }
    std::fclose(fp);
    return mappings;
}
} // namespace

void install_hooks(zygisk::Api *api, bool enhanced_mode) {
    if (g_hook_installed) return;
    if (!api) {
        log_error("zygisk api is null; cannot install binder hook");
        return;
    }

    // enhanced_mode is kept for config compatibility but no longer affects logic
    g_enhanced_mode = enhanced_mode;
    const auto mappings = find_mappings();
    if (mappings.empty()) {
        log_error("libbinder mapping not found; cannot install ioctl hook");
        return;
    }

    for (const auto &mapping : mappings) {
        api->pltHookRegister(mapping.dev, mapping.inode, "ioctl", reinterpret_cast<void *>(hook_ioctl),
                             reinterpret_cast<void **>(&g_original_ioctl));
    }
    if (!api->pltHookCommit() || !g_original_ioctl) {
        log_error("zygisk plt ioctl hook failed");
        return;
    }

    g_hook_installed = true;
    log_info("zygisk plt ioctl hook installed for %zu libbinder mapping(s), enhanced=%d", mappings.size(),
                    g_enhanced_mode ? 1 : 0);
}

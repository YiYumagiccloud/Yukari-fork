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
bool g_enhanced_mode = false;

// Thread local: whether the current thread has a pending synchronous
// transaction to handle 0 (servicemanager). If so, the next BR_REPLY
// will be scanned for service-name strings.
thread_local bool g_pending_sm_reply = false;

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

// Reads a parcel string16 at a given offset, checks for keyword, and overwrites if matched.
// Returns bytes consumed (including padding) if string exists, 0 otherwise.
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
        log_info("scrubbed service string in SM reply: %s", value.c_str());
    }
    return next_off - off;
}

// Safely scan a ServiceManager reply buffer for String16 values containing
// ROM keywords. This works for listServices (String[]), getServiceDebugInfo
// (ServiceDebugInfo[] which contains String16 name), and any other SM reply
// that embeds service names as String16.
//
// The is_parcel_str16 validator is extremely strict (alignment, length,
// null-terminator, printable-ASCII-only), so false positives on binary
// data are virtually impossible.
void scan_sm_reply_for_strings(uint8_t *parcel, size_t size) {
    if (!parcel || size < sizeof(int32_t)) return;
    int total_hits = 0;

    // Walk the buffer 4 bytes at a time, looking for valid String16 structures.
    for (size_t off = 0; off + sizeof(int32_t) <= size; off += sizeof(uint32_t)) {
        bool hit = false;
        size_t consumed = process_string16(parcel, size, off, hit);
        if (hit) ++total_hits;
        // consumed > 0 means we found a valid String16 (hit or not).
        // We can optionally skip past it, but scanning every 4 bytes is
        // safer and the overhead is negligible for typical SM replies.
    }

    if (total_hits > 0) {
        log_info("scan_sm_reply: filtered %d service string(s)", total_hits);
    }
}

void process_transaction(const binder_transaction_data &txn) {
    if (txn.data_size == 0 || txn.data.ptr.buffer == 0) return;
    if (txn.target.handle != 0) return; // Only ServiceManager
    if (txn.flags & TF_ONE_WAY) return;  // Only synchronous transactions get replies

    g_pending_sm_reply = true;

    // For GET_SERVICE / CHECK_SERVICE, filter the service name in the request.
    uint32_t code = txn.code;
    if (code == SVC_GET_SERVICE || code == SVC_CHECK_SERVICE) {
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
    // For all other SM transactions (listServices, getServiceDebugInfo, etc.),
    // we don't filter the request, but we will scan the reply.
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
    if (!g_pending_sm_reply) return;
    g_pending_sm_reply = false;

    if (!txn || txn->data_size == 0 || txn->data.ptr.buffer == 0) return;

    auto *parcel = reinterpret_cast<uint8_t *>(txn->data.ptr.buffer);
    const size_t data_size = static_cast<size_t>(txn->data_size);

    // Use mprotect to ensure the buffer is writable (binder mmap may be read-only)
    make_writable(parcel, data_size);

    // Scan the reply for any String16 containing ROM keywords.
    // This covers listServices, getServiceDebugInfo, and any other
    // ServiceManager reply that embeds service names.
    scan_sm_reply_for_strings(parcel, data_size);
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
                g_pending_sm_reply = false;
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

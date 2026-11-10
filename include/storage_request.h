#ifndef UWU_STORAGE_REQUEST_H
#define UWU_STORAGE_REQUEST_H
#include <stdint.h>
#include <string.h>

// USB callbacks only hand off requests. The main-core worker exclusively owns
// cache data and staging. Protect request publication with short IRQ masking.
namespace storage {
constexpr uint32_t sector_size = 4096;
constexpr uint32_t block_size = 512;
// These are scheduling thresholds, not hard persistence deadlines. A worker
// pass attempts at most one sector; retry backoff can delay draining further.
constexpr uint32_t flush_idle_ms = 250;
constexpr uint32_t max_dirty_ms = 1000;
// Unsigned subtraction keeps elapsed-time checks valid across millis() wrap.
inline bool idle_flush_due(uint32_t now, uint32_t last_write) {
    return uint32_t(now - last_write) >= flush_idle_ms;
}
inline bool aged_flush_due(uint32_t now, uint32_t dirty_since) {
    return uint32_t(now - dirty_since) >= max_dirty_ms;
}
inline bool in_bounds(uint64_t offset, uint64_t count, uint32_t capacity) {
    return offset <= capacity && count <= capacity - offset;
}
/** SYNCHRONIZE CACHE(10/16), or START STOP UNIT with START clear. */
inline bool persistence_barrier(const uint8_t* command) {
    return command[0] == 0x35 || command[0] == 0x91 ||
        (command[0] == 0x1b && !(command[4] & 1));
}
/** WRITE10's FUA bit requires persistence before acknowledging its payload. */
inline bool force_unit_access(const uint8_t* command) {
    return command[0] == 0x2a && (command[1] & 8);
}
enum class State : uint8_t {
    idle,    ///< Mailbox available for a new USB request.
    pending, ///< Published payload waiting for cache acceptance/readback.
    working, ///< Accepted FUA write waiting for dirty data to drain.
    done,    ///< Success ready for USB completion.
    failed   ///< Failure ready for USB completion; cache recovery may continue.
};
enum class Operation : uint8_t { read, write };
/** Single-request mailbox; write bytes are owned, read destination is borrowed. */
struct Request {
    volatile State state = State::idle;
    volatile bool abandoned = false;
    uint32_t offset = 0; ///< Byte offset relative to the MSC partition.
    uint32_t length = 0; ///< Payload length in bytes, at most block_size.
    Operation operation = Operation::write;
    void* read_buffer = nullptr;
    uint8_t data[block_size];
    bool submit(Operation op, uint32_t at, void* buffer, uint32_t count) {
        if (state != State::idle || !count || count > block_size ||
            count > sector_size - at % sector_size) return false;
        operation = op;
        offset = at;
        length = count;
        read_buffer = op == Operation::read ? buffer : nullptr;
        if (op == Operation::write) memcpy(data, buffer, count);
        abandoned = false;
        state = State::pending; // Publish only after the payload/metadata are ready.
        return true;
    }
};

/**
 * Fixed-capacity LRU cache of complete flash erase sectors.
 * Payload storage is supplied separately for PSRAM in firmware / RAM in tests.
 * Only clean entries may be replaced; callers explicitly schedule dirty flushes.
 */
template<unsigned N> class SectorCache {
public:
    static_assert(N > 0, "Sector cache must not be empty");
    struct Entry {
        uint32_t sector = 0; ///< Sector-aligned partition byte offset, not an LBA.
        unsigned age = 0; ///< LRU rank: zero is most recently used.
        bool valid = false;
        bool dirty = false;
        uint32_t dirty_since = 0; ///< First clean-to-dirty time; hits do not reset it.
    } entries[N];

    explicit SectorCache(uint8_t* payload) : payload_(payload) {}
    uint8_t* data(unsigned slot) { return payload_ + slot * sector_size; }

    /** Return a cached/loaded slot, or -1 if no clean victim/read is available. */
    template<class Reader> int acquire(uint32_t sector, Reader read) {
        int victim = -1;
        for (unsigned i = 0; i < N; ++i) {
            if (entries[i].valid && entries[i].sector == sector) {
                touch(i);
                return i;
            }
            if (!entries[i].dirty && (victim < 0 || !entries[i].valid ||
                (entries[victim].valid && entries[i].age > entries[victim].age))) victim = i;
        }
        if (victim < 0) return -1;
        entries[victim].valid = false;
        if (!read(sector, data(victim))) return -1;
        entries[victim] = {sector, N, true, false, 0};
        touch(victim);
        return victim;
    }
    // Select by LRU rank, not dirty_since. Maximum dirty age is checked by worker.
    int oldest_dirty() const {
        int slot = -1;
        for (unsigned i = 0; i < N; ++i) {
            if (entries[i].valid && entries[i].dirty &&
                (slot < 0 || entries[i].age > entries[slot].age)) slot = i;
        }
        return slot;
    }
    bool dirty() const { return oldest_dirty() >= 0; }
    /** Overlay all valid sectors, including complete retry data over damaged flash. */
    void overlay(uint8_t* image) const {
        for (unsigned i = 0; i < N; ++i) {
            if (entries[i].valid) memcpy(image + entries[i].sector,
                payload_ + i * sector_size, sector_size);
        }
    }
private:
    uint8_t* payload_;
    void touch(unsigned slot) {
        // Bounded ranks, not timestamps: repeated hits cannot collapse the
        // ordering of older entries or overflow after long runtimes.
        const unsigned previous = entries[slot].age;
        for (auto& entry : entries) {
            if (entry.valid && entry.age < previous) ++entry.age;
        }
        entries[slot].age = 0;
    }
};
}
#endif
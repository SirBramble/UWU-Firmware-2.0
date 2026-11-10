#include "Adafruit_TinyUSB.h"
#include <filesystem.h>
#include "storage_request.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/mutex.h"
#include "device/usbd_pvt.h"

extern "C" {
extern uint8_t _FS_start, _FS_end, _EEPROM_start;
}

static_assert(TUSB_VERSION_NUMBER >= 2001, "Native async MSC support required");
static_assert(CFG_TUD_MSC_EP_BUFSIZE == 512, "MSC wrapper requires whole 512-byte blocks");
extern mutex_t __usb_mutex; // Arduino-Pico TinyUSB port's task serialization.

namespace {
constexpr uint32_t SNAPSHOT_CAPACITY = 1024 * 1024;
constexpr uint32_t CONFIG_RELOAD_QUIET_MS = 500;
constexpr uint32_t VERIFY_FAILURE_LIMIT = 3;

// The main-loop worker owns cache payloads and flash programming. USB callbacks
// publish one request; USB transport state is serialized with __usb_mutex.
storage::Request request;
constexpr unsigned CACHE_SECTORS = DISK_CACHE_SIZE_KB * 1024 / storage::sector_size;
static_assert(DISK_CACHE_SIZE_KB > 0 && DISK_CACHE_SIZE_KB % 4 == 0,
              "DISK_CACHE_SIZE_KB must be a positive multiple of 4");
uint8_t cache_payload[CACHE_SECTORS * storage::sector_size] PSRAM;
storage::SectorCache<CACHE_SECTORS> cache(cache_payload);
// Internal SRAM: PSRAM/XIP cannot be read during flash programming.
alignas(4) uint8_t staging[FLASH_SECTOR_SIZE];
uint8_t snapshot[SNAPSHOT_CAPACITY] PSRAM;
uint32_t disk_offset = 0, disk_capacity = 0;
bool geometry_valid = false;
volatile uint32_t write_generation = 0;
volatile uint32_t last_write_ms = 0;
uint32_t inspected_generation = 0;
uint32_t last_reload_attempt_ms = 0;
uint32_t failures = 0, retry_at = 0, retries = 0;
uint32_t max_blackout_us = 0, max_worker_us = 0;
uint32_t read_depth = 0;
bool servicing = false, completion_sent = false;
int flush_slot = -1; // Nonnegative while staging holds an immutable retry image.
uint32_t persisted_sectors = 0;
// A held command has not yet been consumed by TinyUSB. Retain its transfer
// metadata so it can be replayed only after persistence succeeds or fails.
uint8_t* cbw_buffer = nullptr;
uint8_t msc_out_endpoint = 0;
bool expecting_cbw = false;
bool force_unit_access = false;
bool barrier_waiting = false;
bool barrier_failed = false;
uint8_t barrier_port = 0, barrier_endpoint = 0;
xfer_result_t barrier_result = XFER_RESULT_SUCCESS;
uint32_t barrier_bytes = 0;
bool mode_sense_10 = false;
uint32_t cache_hits = 0, cache_misses = 0, unchanged_writes = 0;
config_file_read_callback file_callback;

const uint8_t* flash_ptr(uint32_t offset) {
    return reinterpret_cast<const uint8_t*>(XIP_BASE + offset);
}

/** Read-only SdFat adapter over the current immutable PSRAM snapshot. */
class SnapshotBlockDevice : public FsBlockDeviceInterface {
public:
    bool isBusy() override { return false; }
    // This synchronizes the snapshot device, not the USB write-back cache.
    bool syncDevice() override { return true; }
    uint32_t sectorCount() override { return disk_capacity / storage::block_size; }
    bool readSector(uint32_t sector, uint8_t* dst) override {
        return readSectors(sector, dst, 1);
    }
    bool readSectors(uint32_t sector, uint8_t* dst, size_t count) override {
        const uint64_t offset = uint64_t(sector) * storage::block_size;
        const uint64_t bytes = uint64_t(count) * storage::block_size;
        if (!read_depth || !storage::in_bounds(offset, bytes, disk_capacity)) return false;
        memcpy(dst, snapshot + offset, bytes);
        return true;
    }
    bool writeSector(uint32_t, const uint8_t*) override { return false; }
    bool writeSectors(uint32_t, const uint8_t*, size_t) override { return false; }
} snapshot_bd;
FatVolume volume;
Adafruit_USBD_MSC msc;

bool ready() { return geometry_valid; }

/** Publish bounded USB IO; completion is delivered later by the worker. */
int32_t submit(storage::Operation operation, uint32_t lba, void* buffer, uint32_t bytes) {
    const uint64_t offset = uint64_t(lba) * storage::block_size;
    if (!ready() || !storage::in_bounds(offset, bytes, disk_capacity)) return TUD_MSC_RET_ERROR;
    if (request.submit(operation, offset, buffer, bytes)) {
        completion_sent = false;
        return TUD_MSC_RET_ASYNC;
    }
    // A callback while the previous async payload is retained means a new
    // command followed a reset. Do not acknowledge old data as the new write.
    request.abandoned = true;
    return TUD_MSC_RET_ERROR;
}
int32_t read_cb(uint32_t lba, void* buffer, uint32_t bytes) {
    return submit(storage::Operation::read, lba, buffer, bytes);
}
int32_t write_cb(uint32_t lba, uint8_t* buffer, uint32_t bytes) {
    return submit(storage::Operation::write, lba, buffer, bytes);
}
bool start_stop_cb(uint8_t, bool start, bool /* load_eject */) {
    // The command adapter delays STOP/eject until the complete cache is clean.
    if (!start && (cache.dirty() || barrier_failed)) {
        tud_msc_set_sense(0, SCSI_SENSE_MEDIUM_ERROR, 0x0c, 0);
        return false;
    }
    // Fixed flash cannot physically eject. Keep the medium available for a
    // later remount, without detaching USB (which would also interrupt HID).
    // Success still requires the barrier above; subsequent writes require a
    // new flush/eject before unplugging.
    return true;
}

void cancel_request() {
    // Keep failed/pending WRITE data, but never complete it against a new CBW.
    request.abandoned = true;
    barrier_waiting = false;
    barrier_failed = false;
    expecting_cbw = false;
    force_unit_access = false;
}
void reset_msc(uint8_t port) {
    cancel_request();
    mscd_reset(port);
}
bool control_msc(uint8_t port, uint8_t stage, tusb_control_request_t const* control) {
    if (stage == CONTROL_STAGE_SETUP && control->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS &&
        control->bRequest == MSC_REQ_RESET) cancel_request();
    return mscd_control_xfer_cb(port, stage, control);
}
bool transfer_msc(uint8_t port, uint8_t endpoint, xfer_result_t result, uint32_t bytes) {
    if (expecting_cbw && !(endpoint & 0x80)) {
        expecting_cbw = false;
        if (bytes == sizeof(msc_cbw_t) && cbw_buffer) {
            auto* cbw = reinterpret_cast<msc_cbw_t*>(cbw_buffer);
            if (cbw->signature == MSC_CBW_SIGNATURE) {
                const uint8_t* cmd = cbw->command;
                force_unit_access = storage::force_unit_access(cmd);
                mode_sense_10 = cmd[0] == 0x5a;
                // Route MODE SENSE(6) through our generic handler instead of
                // TinyUSB's fixed header-only response. Original form retained above.
                if (cmd[0] == 0x1a) cbw->command[0] = 0x5a;
                barrier_failed = false;
                // Hold the unconsumed CBW, not an already-successful status.
                // Bulk OUT stays unarmed, so no later command can overtake it.
                const bool barrier = storage::persistence_barrier(cmd);
                if (barrier && cache.dirty()) {
                    barrier_port = port;
                    barrier_endpoint = endpoint;
                    barrier_result = result;
                    barrier_bytes = bytes;
                    barrier_waiting = true;
                    return true;
                }
            }
        }
    }
    return mscd_xfer_cb(port, endpoint, result, bytes);
}
uint16_t open_msc(uint8_t port, tusb_desc_interface_t const* interface, uint16_t length) {
    if (interface->bInterfaceClass != TUSB_CLASS_MSC) return 0;
    const uint16_t required = sizeof(tusb_desc_interface_t) + 2 * sizeof(tusb_desc_endpoint_t);
    if (length < required) return 0;
    const uint8_t* descriptor = reinterpret_cast<const uint8_t*>(interface) + sizeof(*interface);
    msc_out_endpoint = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const auto* endpoint = reinterpret_cast<const tusb_desc_endpoint_t*>(descriptor);
        if (endpoint->bDescriptorType != TUSB_DESC_ENDPOINT ||
            endpoint->bLength != sizeof(tusb_desc_endpoint_t)) return 0;
        if (!(endpoint->bEndpointAddress & 0x80)) msc_out_endpoint = endpoint->bEndpointAddress;
        descriptor += endpoint->bLength;
    }
    return mscd_open(port, interface, length);
}
// TinyUSB explicitly supports application drivers overriding built-in drivers.
// Delegate transport unchanged; observe reset so stale IO cannot complete a new command.
const usbd_class_driver_t msc_driver = {
    "UWU MSC", mscd_init, mscd_deinit, reset_msc, open_msc,
    control_msc, transfer_msc, nullptr, nullptr
};
void release_completed_request(void*) {
    if (request.state == storage::State::done || request.state == storage::State::failed)
        request.state = storage::State::idle;
}
}

extern "C" usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* count) {
    *count = 1;
    return &msc_driver;
}

/**
 * Persist and verify one complete sector, or retry its retained staging image.
 * No cache mutation is allowed while staging is pinned. Backoff/failure leaves
 * flush_slot intact; only a successful comparison releases it and clears dirty.
 */
static void flush_sector(int slot) {
    if (slot < 0 || (retries && int32_t(millis() - retry_at) < 0)) return;
    if (flush_slot < 0) {
        flush_slot = slot;
        memcpy(staging, cache.data(slot), sizeof(staging));
    }
    auto& entry = cache.entries[flush_slot];
    bool ok = memcmp(flash_ptr(disk_offset + entry.sector), staging, sizeof(staging)) == 0;
    if (!ok) {
        const uint32_t irq = save_and_disable_interrupts();
        const uint32_t start = micros();
        flash_range_erase(disk_offset + entry.sector, FLASH_SECTOR_SIZE);
        flash_range_program(disk_offset + entry.sector, staging, sizeof(staging));
        const uint32_t elapsed = micros() - start;
        restore_interrupts(irq);
        if (elapsed > max_blackout_us) max_blackout_us = elapsed;
        ok = memcmp(flash_ptr(disk_offset + entry.sector), staging, sizeof(staging)) == 0;
    }
    if (!ok) {
        ++failures;
        ++retries;
        retry_at = millis() + (retries < VERIFY_FAILURE_LIMIT ? 20 : 1000);
        return;
    }
    entry.dirty = false;
    flush_slot = -1;
    retries = 0;
    ++persisted_sectors;
}

/**
 * Handle a pending read/write without programming flash.
 * @return Dirty eviction candidate if cache pressure blocks acceptance, else -1.
 * The caller alone schedules flash work, at most once per worker pass.
 */
static int service_request() {
    if (flush_slot >= 0 || request.state != storage::State::pending) return -1;
    const uint32_t sector = request.offset & ~(FLASH_SECTOR_SIZE - 1);
    bool miss = false;
    const int slot = cache.acquire(sector, [&](uint32_t at, uint8_t* dst) {
        miss = true;
        memcpy(dst, flash_ptr(disk_offset + at), FLASH_SECTOR_SIZE);
        return true;
    });
    if (slot < 0) return cache.oldest_dirty();

    if (miss) ++cache_misses; else ++cache_hits;
    auto& entry = cache.entries[slot];
    uint8_t* data = cache.data(slot);
    const uint32_t within = request.offset - sector;
    if (request.operation == storage::Operation::read) {
        if (mutex_try_enter(&__usb_mutex, nullptr)) {
            if (!request.abandoned) memcpy(request.read_buffer, data + within, request.length);
            request.state = storage::State::done;
            mutex_exit(&__usb_mutex);
        }
    } else {
        // Acknowledging an ordinary write means PSRAM acceptance, not persistence.
        // FUA keeps the request working until the worker has drained dirty data.
        if (memcmp(data + within, request.data, request.length) == 0) {
            ++unchanged_writes;
        } else {
            memcpy(data + within, request.data, request.length);
            if (!entry.dirty) entry.dirty_since = millis();
            entry.dirty = true;
            ++write_generation;
            last_write_ms = millis();
        }
        request.state = force_unit_access ? storage::State::working : storage::State::done;
    }
    return -1;
}

static void service_usb_completions() {
    // Process completion while holding the SAME mutex as the RP USB task.
    // Keep the request gate closed until the queued completion is consumed.
    // Reset/new-CBW events cannot attach this completion to a new async IO.
    if (mutex_try_enter(&__usb_mutex, nullptr)) {
        if (!completion_sent && (request.state == storage::State::done ||
                                request.state == storage::State::failed)) {
            // False means the stack cancelled its operation (e.g. reset).
            if (!request.abandoned) {
                tud_msc_async_io_done(request.state == storage::State::done
                    ? int32_t(request.length) : TUD_MSC_RET_ERROR, false);
            }
            completion_sent = true;
            usbd_defer_func(release_completed_request, nullptr, false);
            tud_task();
        }
        mutex_exit(&__usb_mutex);
    }
    if (barrier_waiting && (!cache.dirty() || retries >= VERIFY_FAILURE_LIMIT) &&
        mutex_try_enter(&__usb_mutex, nullptr)) {
        // Recheck under the task mutex: a reset may have cancelled the barrier.
        if (barrier_waiting) {
            barrier_failed = cache.dirty();
            barrier_waiting = false;
            mscd_xfer_cb(barrier_port, barrier_endpoint, barrier_result, barrier_bytes);
        }
        mutex_exit(&__usb_mutex);
    }
}

/** Advance recovery, request acceptance, one flash attempt, then USB completion. */
static void filesystem_service() {
    if (servicing || !geometry_valid) return;
    servicing = true;
    const uint32_t started = micros();
    if (request.abandoned && (request.state == storage::State::pending ||
                              request.state == storage::State::working))
        request.state = storage::State::done;

    const bool attempted_recovery = flush_slot >= 0;
    if (attempted_recovery) flush_sector(flush_slot);
    int slot = service_request();
    // Even successful recovery consumes this pass's flash budget. A pending
    // request can still be accepted, but another sector waits until next pass.
    if (!attempted_recovery && flush_slot < 0) {
        const uint32_t now = millis();
        if (slot < 0) {
            if (barrier_waiting || request.state == storage::State::working ||
                storage::idle_flush_due(now, last_write_ms)) {
                slot = cache.oldest_dirty();
            } else {
                for (unsigned i = 0; i < CACHE_SECTORS; ++i) {
                    if (cache.entries[i].dirty && storage::aged_flush_due(now, cache.entries[i].dirty_since)) {
                        slot = i;
                        break;
                    }
                }
            }
        }
        flush_sector(slot);
    }
    if (request.state == storage::State::working && !cache.dirty()) request.state = storage::State::done;
    if (retries >= VERIFY_FAILURE_LIMIT && (request.state == storage::State::pending ||
                        request.state == storage::State::working)) request.state = storage::State::failed;
    service_usb_completions();
    const uint32_t elapsed = micros() - started;
    if (elapsed > max_worker_us) max_worker_us = elapsed;
    servicing = false;
}

FatVolume* filesystem_begin_read() {
    if (!geometry_valid) return nullptr;
    if (read_depth) { ++read_depth; return &volume; }
    // Flash is mutated ONLY by filesystem_service on this core. USB can queue
    // one write during capture, but cannot change the image being copied.
    memcpy(snapshot, flash_ptr(disk_offset), disk_capacity);
    // Cached sectors include acknowledged writes and the full recovery image
    // of any partially programmed sector. Main-core worker cannot run here.
    cache.overlay(snapshot);
    read_depth = 1;
    volume.cacheClear();
    if (!volume.begin(&snapshot_bd, true, 0, 0)) {
        volume.end();
        read_depth = 0;
        return nullptr;
    }
    return &volume;
}
void filesystem_end_read() {
    if (!read_depth || --read_depth) return;
    volume.end();
    volume.cacheClear();
}
void filesystem_set_config_file_read_callback(config_file_read_callback callback) {
    file_callback = callback;
}
static bool reload_configuration() {
    auto* current = filesystem_begin_read();
    if (!current) return false;
    if (file_callback && !file_callback(*current)) {
        Serial.println("Configuration reload rejected");
    }
    filesystem_end_read();
    return true; // Snapshot inspected; retry rejected config on the next write.
}
void filesystem_setup() {
    const uintptr_t start = reinterpret_cast<uintptr_t>(&_FS_start);
    const uintptr_t end = reinterpret_cast<uintptr_t>(&_FS_end);
    const uintptr_t eeprom = reinterpret_cast<uintptr_t>(&_EEPROM_start);
    geometry_valid = start >= XIP_BASE && end > start && end <= eeprom &&
        end <= XIP_BASE + PICO_FLASH_SIZE_BYTES &&
        start % FLASH_SECTOR_SIZE == 0 && end % FLASH_SECTOR_SIZE == 0 &&
        end - start == SNAPSHOT_CAPACITY;
    if (geometry_valid) { disk_offset = start - XIP_BASE; disk_capacity = end - start; }
    msc.setID("FG-Labs", USB_PRODUCT_ID, USB_PRODUCT_VERSION);
    msc.setCapacity(disk_capacity / storage::block_size, storage::block_size);
    msc.setReadWriteCallback(read_cb, write_cb, nullptr);
    msc.setReadyCallback(ready);
    msc.setStartStopCallback(start_stop_cb);
    msc.setUnitReady(geometry_valid);
    msc.begin();
    // No formatting or migration: old media must be handled manually.
    reload_configuration();
}
void filesystem_loop() {
    filesystem_service();
    const uint32_t generation = write_generation;
    const uint32_t now = millis();
    if (generation == inspected_generation || now - last_write_ms < CONFIG_RELOAD_QUIET_MS ||
        now - last_reload_attempt_ms < CONFIG_RELOAD_QUIET_MS ||
        request.state != storage::State::idle) return;
    // Quiet time is a reload policy, NOT a consistency guarantee.
    last_reload_attempt_ms = now;
    if (reload_configuration()) inspected_generation = generation;
}
FilesystemStats filesystem_stats() {
    const uint32_t irq = save_and_disable_interrupts();
    const FilesystemStats result{persisted_sectors, failures, max_blackout_us, max_worker_us,
        cache.dirty(), cache_hits, cache_misses, unchanged_writes};
    restore_interrupts(irq);
    return result;
}

// Explicit linker adapters; installed TinyUSB source is never modified.
// Observe CBW receive buffers before the driver consumes them. All endpoint
// scheduling still goes through the original library function unchanged.
extern "C" bool __real_usbd_edpt_xfer(uint8_t, uint8_t, uint8_t*, uint16_t, bool);
extern "C" bool __wrap_usbd_edpt_xfer(uint8_t port, uint8_t endpoint, uint8_t* buffer,
                                      uint16_t bytes, bool is_isr) {
    const bool cbw = endpoint == msc_out_endpoint && bytes == sizeof(msc_cbw_t);
    const bool result = __real_usbd_edpt_xfer(port, endpoint, buffer, bytes, is_isr);
    if (result && cbw) {
        cbw_buffer = buffer;
        expecting_cbw = true;
    }
    return result;
}
extern "C" int32_t __real_tud_msc_scsi_cb(uint8_t, uint8_t const[16], void*, uint16_t);
extern "C" int32_t __wrap_tud_msc_scsi_cb(uint8_t lun, uint8_t const command[16],
                                          void* buffer, uint16_t size) {
    if (command[0] == 0x35 || command[0] == 0x91) {
        if (!ready() || cache.dirty() || barrier_failed) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0c, 0);
            return -1;
        }
        return 0;
    }
    if (command[0] == 0x5a) {
        const uint8_t mode_page = command[2] & 0x3f;
        const uint8_t mode_control = command[2] >> 6;
        const uint16_t mode_allocation = mode_sense_10
            ? (uint16_t(command[7]) << 8) | command[8] : command[4];
        if (mode_control == 3 || (mode_page != 0 && mode_page != 8 && mode_page != 0x3f)) {
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0);
            return -1;
        }
        const unsigned header = mode_sense_10 ? 8 : 4;
        uint8_t response[28] = {};
        const unsigned length = header + (mode_page ? 20 : 0);
        response[mode_sense_10 ? 1 : 0] = length - (mode_sense_10 ? 2 : 1);
        if (mode_page) {
            response[header] = 8;
            response[header + 1] = 18;
            // WCE=1 for current/default, no changeable bits (fixed policy).
            response[header + 2] = mode_control == 1 ? 0 : 4;
        }
        const unsigned count = min(unsigned(size), min(unsigned(mode_allocation), length));
        memcpy(buffer, response, count);
        return count;
    }
    return __real_tud_msc_scsi_cb(lun, command, buffer, size);
}
#ifndef FILESYSTEM_H
#define FILESYSTEM_H
#include <Arduino.h>
#include <SdFat.h>
#include <functional>

#include "config.h"

typedef std::function<bool(FatVolume &volume)> config_file_read_callback;

/** Register the configuration reader invoked inside a snapshot read session. */
void filesystem_set_config_file_read_callback(config_file_read_callback callback);
/** Validate the reserved partition, expose USB MSC, and inspect initial config. */
void filesystem_setup();
/** Main-core service: advance async IO/write-back and reload config after quiet. */
void filesystem_loop();

/**
 * Begin a main-core, read-only session over flash plus accepted cached changes.
 * Nested sessions share the existing immutable image, not a fresh snapshot.
 * @return Mounted volume, or nullptr if geometry/mounting fails.
 * @note Pair each successful call with filesystem_end_read(). Close all files
 * before the outermost session ends; never retain its volume afterwards.
 * A snapshot is stable, but does not prove the host finished its FAT transaction.
 */
FatVolume* filesystem_begin_read();
/** Release one nesting level; the outermost release unmounts the snapshot. */
void filesystem_end_read();

/** Storage diagnostics; persistence counts refer to 4 KiB sectors, not USB IOs. */
struct FilesystemStats {
    uint32_t persisted_writes; ///< Successful sector flushes, including already-equal flash.
    uint32_t verify_failures;  ///< Failed flash comparisons after programming.
    uint32_t max_blackout_us;  ///< Longest interrupt-masked erase/program interval.
    uint32_t max_worker_us;    ///< Longest complete storage worker pass.
    bool dirty;               ///< Accepted cached data still needs persistence.
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t unchanged_writes;
};
FilesystemStats filesystem_stats();

#endif //FILESYSTEM_H
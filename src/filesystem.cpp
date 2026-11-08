#include "Adafruit_TinyUSB.h"
#include <filesystem.h>

// Pico SDK flash access
#include "hardware/flash.h"
#include "hardware/sync.h"

// ---- Disk geometry ----
static constexpr uint32_t BLOCK_SIZE = 512;
static constexpr uint32_t DISK_SIZE_BYTES = DISK_SIZE_MB * 1024 * 1024;
static constexpr uint32_t BLOCK_COUNT = DISK_SIZE_BYTES / BLOCK_SIZE;

#ifndef PICO_FLASH_SIZE_BYTES
  #error "PICO_FLASH_SIZE_BYTES is not defined. Define it via build_flags, or use a board definition that provides it."
#endif
static constexpr uint32_t DISK_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - DISK_SIZE_BYTES;

struct CachedSector {
    bool valid = false;
    bool dirty = false;
    uint32_t flash_sector_base = 0;
    uint8_t data[4096];
};

struct FlashWriteParams
{
    uint32_t flash_offset;
    const uint8_t* data;
};

// Functions
static CachedSector* load_sector_into_cache(CachedSector& s, uint32_t sectorBase);
static CachedSector* find_cached_sector(uint32_t sectorBase);
static CachedSector* get_cached_sector(uint32_t sectorBase);

static bool disk_read( uint32_t diskOffset, uint8_t* dst, uint32_t bytes);

static int32_t msc_write_cb( uint32_t lba, uint8_t* buffer, uint32_t bufsize);
static int32_t msc_read_cb(uint32_t lba, void* buffer, uint32_t bufsize);
static void msc_flush_cb();
static bool msc_start_stop_cb(uint8_t power_condition, bool start, bool load_eject);

static bool cache_is_dirty();
static bool flush_cached_sector(CachedSector& s);
static bool flush_cache();
static void invalidate_cache();
static bool flush_and_invalidate_cache();

class FlashBlockDevice : public FsBlockDeviceInterface
{
public:
    bool begin() { return true; }
    bool isBusy() override { return false; }
    bool syncDevice() override { return true; }
    uint32_t sectorCount() override { return BLOCK_COUNT; }

    bool readSector(uint32_t sector, uint8_t* dst) override
    {
        return readSectors(sector, dst, 1);
    }

    bool readSectors(uint32_t sector, uint8_t* dst, size_t count) override
    {
        return disk_read(sector * BLOCK_SIZE, dst, count * BLOCK_SIZE);
    }

    // Pico-side FAT access is deliberately read-only.
    bool writeSector(uint32_t, const uint8_t*) override { return false; }
    bool writeSectors( uint32_t, const uint8_t*, size_t) override { return false; }
};

FlashBlockDevice flashBD;
FatVolume fatVol;

Adafruit_USBD_MSC usb_internal_flash_msc;

static CachedSector cache[32] PSRAM;   // 32*4 KB total
static uint8_t flash_write_buffer[FLASH_SECTOR_SIZE];  // To prevent XIP->XIP copy operations

config_file_read_callback g_file_read_callback = nullptr;

// Flags
volatile bool g_msc_write_happened = false;
volatile bool g_msc_flush_happened = false;
volatile bool g_msc_ejected = false;

volatile uint32_t g_last_write_ms = 0;


// Read from XIP-mapped flash
static inline const uint8_t* flash_ptr(uint32_t flash_offset)
{
  return (const uint8_t*)(XIP_BASE + flash_offset);
}

static CachedSector* load_sector_into_cache(CachedSector& s, uint32_t sectorBase)
{
    memcpy(s.data, flash_ptr(sectorBase), FLASH_SECTOR_SIZE);

    s.flash_sector_base = sectorBase;
    s.valid = true;
    s.dirty = false;

    return &s;
}

static CachedSector* find_cached_sector(uint32_t sectorBase)
{
    for (auto& s : cache)
    {
        if (s.valid && s.flash_sector_base == sectorBase)
            return &s;
    }

    return nullptr;
}

static CachedSector* get_cached_sector(uint32_t sectorBase)
{
    // already cached?
    if (CachedSector* s = find_cached_sector(sectorBase))
        return s;

    // find empty slot
    for (auto& s : cache)
    {
        if (!s.valid)
        {
            return load_sector_into_cache(s, sectorBase);
        }
    }

    // Try to flush dirty sectors first (gives us clean slots to evict)
    for (auto& s : cache)
    {
        if (s.valid && s.dirty)
        {
            if (!flush_cached_sector(s))
                return nullptr;  // Flush failed, give up
            s.dirty = false;
        }
    }

    // Now try to find a clean slot
    for (auto& s : cache)
    {
        if (!s.valid)
        {
            return load_sector_into_cache(s, sectorBase);
        }
    }

    // Still no slot? Find victim intelligently
    // Prefer dirty sectors (can be flushed), then clean sectors
    uint32_t victimIndex = 0;
    bool victimDirty = false;
    
    for (uint32_t i = 1; i < sizeof(cache) / sizeof(cache[0]); i++)
    {
        if (cache[i].valid && cache[i].dirty)
        {
            // Prefer dirty sectors for eviction
            if (!victimDirty)
            {
                victimIndex = i;
                victimDirty = true;
            }
        }
        else if (!cache[i].valid)
        {
            // Empty slot is best
            victimIndex = i;
            break;
        }
    }

    CachedSector& victim = cache[victimIndex];

    // If victim is dirty, try to flush it
    if (victim.valid && victim.dirty)
    {
        if (!flush_cached_sector(victim))
            return nullptr;  // Flush failed, give up
        victim.dirty = false;
    }

    victim.valid = false;

    return load_sector_into_cache(victim, sectorBase);
}

static bool disk_read(uint32_t diskOffset, uint8_t* dst, uint32_t bytes)
{
    if ((uint64_t)diskOffset + bytes > DISK_SIZE_BYTES)
        return false;

    while (bytes > 0)
    {
        uint32_t flashOffset = DISK_FLASH_OFFSET + diskOffset;

        uint32_t sectorBase = flashOffset & ~(FLASH_SECTOR_SIZE - 1);

        uint32_t inSector = flashOffset - sectorBase;

        uint32_t chunk = min<uint32_t>( bytes, FLASH_SECTOR_SIZE - inSector );

        // Read from cache
        if (CachedSector* cached = find_cached_sector(sectorBase))
        {
            memcpy(dst, cached->data + inSector, chunk);
        }
        else  // Read from disk
        {
            memcpy(dst, flash_ptr(flashOffset), chunk);
        }

        dst        += chunk;
        diskOffset += chunk;
        bytes      -= chunk;
    }

    return true;
}

static int32_t msc_write_cb( uint32_t lba, uint8_t* buffer, uint32_t bufsize)
{
    uint32_t originalBufSize = bufsize;

    uint32_t diskOffset = lba * 512;

    if ((uint64_t)diskOffset + bufsize > DISK_SIZE_BYTES)
        return -1;

    while (bufsize)
    {
        uint32_t flashOffset = DISK_FLASH_OFFSET + diskOffset;

        uint32_t sectorBase = flashOffset & ~(FLASH_SECTOR_SIZE - 1);

        uint32_t inSector = flashOffset - sectorBase;

        uint32_t chunk = min<uint32_t>(bufsize, FLASH_SECTOR_SIZE - inSector);

        CachedSector* s = get_cached_sector(sectorBase);
        if (!s)
        {
            // All cache entries are occupied + dirty and no new slot could be allocated
            return -1;
        }

        memcpy( s->data + inSector, buffer, chunk);

        s->dirty = true;

        buffer     += chunk;
        diskOffset += chunk;
        bufsize    -= chunk;
    }

    g_msc_write_happened = true;
    g_last_write_ms = millis();

    return static_cast<int32_t>(originalBufSize);
}

static int32_t msc_read_cb(uint32_t lba, void* buffer, uint32_t bufsize)
{
    uint32_t diskOffset = lba * BLOCK_SIZE;

    if (!disk_read(diskOffset, static_cast<uint8_t*>(buffer), bufsize))
    {
        return -1;
    }

    return static_cast<int32_t>(bufsize);
}

static void msc_flush_cb()
{
  // Host signals “I’m done” (Windows does this on close; Linux often batches)
  g_msc_flush_happened = true;
}

static bool msc_start_stop_cb(uint8_t power_condition, bool start, bool load_eject)
{
    if (load_eject && !start)
    {
        // Host explicitly ejected the volume
        g_msc_ejected = true;
    }
    else if (start)
    {
        g_msc_ejected = false;
    }

    return true;
}

static void flash_sector_write_impl(void* param)
{
    auto* p = static_cast<FlashWriteParams*>(param);

    flash_range_erase(p->flash_offset, FLASH_SECTOR_SIZE);

    flash_range_program(p->flash_offset, p->data, FLASH_SECTOR_SIZE);
}

static bool cache_is_dirty()
{
    for (const auto& s : cache)
    {
        if (s.valid && s.dirty)
            return true;
    }

    return false;
}

static bool flush_cached_sector(CachedSector& s)
{
    if (!s.valid || !s.dirty)
        return true;

    // PSRAM -> internal SRAM while XIP is still available
    memcpy(flash_write_buffer, s.data, FLASH_SECTOR_SIZE);

    uint32_t ints = save_and_disable_interrupts();

    flash_range_erase(s.flash_sector_base, FLASH_SECTOR_SIZE);

    flash_range_program(s.flash_sector_base, flash_write_buffer, FLASH_SECTOR_SIZE );

    restore_interrupts(ints);
     
    if (memcmp(flash_ptr(s.flash_sector_base), flash_write_buffer, FLASH_SECTOR_SIZE) != 0)
    {
        Serial.printf( "Flash verify failed at 0x%08lx\n", s.flash_sector_base);
        return false;
    }

    s.dirty = false;

    Serial.printf("Flash persisted: 0x%08lx\n", s.flash_sector_base);
    return true;
}

static bool flush_cache()
{
    for (auto& s : cache)
    {
        if (s.valid && s.dirty)
        {
            if (!flush_cached_sector(s))
                return false;
        }
    }

    return true;
}

static void invalidate_cache()
{
    for (auto& s : cache)
    {
        s.valid = false;
        s.dirty = false;
    }
}

static bool flush_and_invalidate_cache()
{
    if (!flush_cache())
        return false;

    invalidate_cache();
    return true;
}

void dumpBoot() {
  uint8_t b[512];
  if (!flashBD.readSector(0, b)) {
    Serial.println("readSector(0) failed");
    return;
  }
  Serial.printf("B0: %02X %02X %02X  OEM:'%.8s'\n", b[0], b[1], b[2], (char*)&b[3]);
  Serial.printf("Sig: %02X %02X\n", b[510], b[511]);
}

void listRoot() {
  dumpBoot();
  if (!fatVol.begin(&flashBD, true, 0, 0)) {
    Serial.println("FAT mount failed");
    return;
  }

  FatFile dir;
  FatFile f;
  if (!dir.open("/")) {
    Serial.println("Open / failed");
    fatVol.end();
    return;
  }

  Serial.println("Root directory:");
  while (f.openNext(&dir, O_RDONLY)) {
    char name[64];
    f.getName(name, sizeof(name));
    Serial.print(" - ");
    Serial.print(name);
    if (f.isDir()) Serial.println("/");
    else {
      Serial.print("  ");
      Serial.print(f.fileSize());
      Serial.println(" bytes");
    }
    f.close();
  }

  dir.close();
  fatVol.end();   // unmount so next “update” remount is clean
}

void filesystem_set_config_file_read_calback(config_file_read_callback f)
{
  g_file_read_callback = f;
}

void filesystem_read_file(const char* path)
{
  if(g_file_read_callback != nullptr)
    g_file_read_callback(fatVol);
}

void filesystem_scan()
{
  if (!fatVol.begin(&flashBD, true, 0, 0)) {
    Serial.println("FAT mount failed");
    return;
  }

  if (fatVol.exists(CONFIG_FILE_PATH)) {
    Serial.print(CONFIG_FILE_PATH); Serial.println(" found");
    filesystem_read_file(CONFIG_FILE_PATH);
  } else {
    Serial.print(CONFIG_FILE_PATH); Serial.println(" not present");
  }

  fatVol.end();   // VERY IMPORTANT (avoid stale FAT cache)
}

void filesystem_setup()
{
  // Optional: customize what the host shows
  usb_internal_flash_msc.setID("FG-Labs", USB_PRODUCT_ID, USB_PRODUCT_VERSION);

  // Capacity in blocks
  usb_internal_flash_msc.setCapacity(BLOCK_COUNT, BLOCK_SIZE);

  // Read/write callbacks
  usb_internal_flash_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);

  // Start/Stop callback
  usb_internal_flash_msc.setStartStopCallback(msc_start_stop_cb);

  // This is the missing piece in many "0B drive" cases
  usb_internal_flash_msc.setUnitReady(true);

  // Start MSC
  usb_internal_flash_msc.begin();

  // Initial scan
  filesystem_scan();
}

void filesystem_loop()
{
  static bool pending = false;
  static uint32_t last_trigger_ms = 0;

  // If host wrote anything, wait for a flush + debounce quiet period
  if (g_msc_write_happened) {
    pending = true;
  }

  if (g_msc_flush_happened) {
    if (!flush_cache())
    {
        Serial.println("ERROR: flash cache flush failed");
    }
    g_msc_flush_happened = false;
    last_trigger_ms = millis();
  }

  // Debounce: wait until no writes for 500ms after a flush
  if (pending) {
    uint32_t now = millis();
    if ((now - last_trigger_ms) > 500 && (now - g_last_write_ms) > 500) {
      pending = false;
      g_msc_write_happened = false;

      // Now it’s relatively safe to parse FAT
      listRoot();
      filesystem_scan();

      Serial.printf("CACHE DITRY: %d", cache_is_dirty());
    }
  }

  // If ejected, flush
  if (g_msc_ejected && cache_is_dirty())
    {
            if (!flush_and_invalidate_cache())
            {
                Serial.println("Flash flush failed");
                return;
            }

            return;
    }

}

FatVolume* filesystem_get_flash_volume()
{
  return &fatVol;
}